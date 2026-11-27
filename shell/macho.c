/* PrismOS — Mach-O (64 ビット) パーサー。 */
#include "macho.h"

#include "string.h"

/* Mach-O の定数。 */
#define MACHO_MAGIC_64 0xfeedfacfu
#define MACHO_CPU_X86_64 0x01000007u
#define MACHO_FILE_EXECUTE 2u
#define MACHO_LC_SEGMENT_64 0x19u
#define MACHO_LC_MAIN 0x80000028u
#define MACHO_LC_UNIXTHREAD 0x5u
#define MACHO_LC_LOAD_DYLIB 0xcu
#define MACHO_LC_LOAD_DYLINKER 0xeu
#define MACHO_LC_LOAD_WEAK_DYLIB 0x18u
#define MACHO_LC_REEXPORT_DYLIB 0x1fu
#define MACHO_LC_LAZY_LOAD_DYLIB 0x20u
#define MACHO_LC_LOAD_UPWARD_DYLIB 0x23u
#define MACHO_X86_64_THREAD_STATE 4u
#define MACHO_X86_64_STATE_COUNT 42u /* 21 レジスタ x 2 ワード */

static int rd32(const uint8_t *d, size_t len, size_t off,
                uint32_t *out) {
    if (off + 4 > len) {
        return -1;
    }
    *out = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
           ((uint32_t)d[off + 2] << 16) |
           ((uint32_t)d[off + 3] << 24);
    return 0;
}

static int rd64(const uint8_t *d, size_t len, size_t off,
                uint64_t *out) {
    uint32_t lo, hi;
    if (rd32(d, len, off, &lo) != 0 ||
        rd32(d, len, off + 4, &hi) != 0) {
        return -1;
    }
    *out = (uint64_t)lo | ((uint64_t)hi << 32);
    return 0;
}

const char *macho_error(int rc) {
    switch (rc) {
    case -1:
        return "not a Mach-O file (bad magic)";
    case -2:
        return "not an x86-64 Mach-O (wrong CPU type)";
    case -3:
        return "truncated Mach-O file";
    case -4:
        return "too many load commands or segments";
    case -5:
        return "bad Mach-O layout (entry or segment out of range)";
    case -6:
        return "needs a dynamic loader (dyld) - not supported";
    case -7:
        return "no entry point (no LC_MAIN or LC_UNIXTHREAD)";
    default:
        return "unknown Mach-O error";
    }
}

int macho_parse(const uint8_t *data, size_t len, macho_info *out) {
    if (data == NULL || out == NULL || len < 32) {
        return -3;
    }
    memset(out, 0, sizeof(*out));
    uint32_t magic;
    if (rd32(data, len, 0, &magic) != 0) {
        return -3;
    }
    if (magic != MACHO_MAGIC_64) {
        return -1;
    }
    uint32_t cputype, filetype, ncmds, sizeofcmds;
    if (rd32(data, len, 4, &cputype) != 0 ||
        rd32(data, len, 12, &filetype) != 0 ||
        rd32(data, len, 16, &ncmds) != 0 ||
        rd32(data, len, 20, &sizeofcmds) != 0) {
        return -3;
    }
    if (cputype != MACHO_CPU_X86_64) {
        return -2;
    }
    if (filetype != MACHO_FILE_EXECUTE) {
        return -5; /* 実行ファイル以外 (MH_OBJECT 等) は対象外 */
    }
    if (ncmds > MACHO_MAX_COMMANDS) {
        return -4;
    }
    if (sizeofcmds > len || 32 + sizeofcmds > len) {
        return -3; /* コマンド領域がファイルに収まらない */
    }
    uint64_t text_vmaddr = 0;
    int have_text = 0;
    uint64_t entryoff = 0;
    int have_main = 0;
    uint64_t thread_rip = 0;
    int have_thread = 0;
    size_t off = 32;
    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd, cmdsize;
        if (rd32(data, len, off, &cmd) != 0 ||
            rd32(data, len, off + 4, &cmdsize) != 0) {
            return -3;
        }
        if (cmdsize < 8 || off + cmdsize > 32 + sizeofcmds) {
            return -3;
        }
        switch (cmd) {
        case MACHO_LC_SEGMENT_64: {
            if (cmdsize < 72) {
                return -3;
            }
            if (out->seg_count >= MACHO_MAX_SEGMENTS) {
                return -4;
            }
            macho_segment *s = &out->segs[out->seg_count];
            memset(s, 0, sizeof(*s));
            memcpy(s->segname, data + off + 8, 16);
            s->segname[16] = '\0';
            if (rd64(data, len, off + 24, &s->vmaddr) != 0 ||
                rd64(data, len, off + 32, &s->vmsize) != 0 ||
                rd64(data, len, off + 40, &s->fileoff) != 0 ||
                rd64(data, len, off + 48, &s->filesize) != 0 ||
                rd32(data, len, off + 56, &s->maxprot) != 0 ||
                rd32(data, len, off + 60, &s->initprot) != 0) {
                return -3;
            }
            if (s->vmsize > 256u * 1024 * 1024 ||
                s->vmaddr >= (1ULL << 48) ||
                s->fileoff > len || s->filesize > len ||
                s->fileoff + s->filesize > len) {
                return -5;
            }
            /* セグメント同士の仮想アドレス重なりは断る。 */
            for (int i = 0; i < out->seg_count; i++) {
                const macho_segment *o = &out->segs[i];
                if (s->vmsize > 0 && o->vmsize > 0 &&
                    s->vmaddr < o->vmaddr + o->vmsize &&
                    o->vmaddr < s->vmaddr + s->vmsize) {
                    return -5;
                }
            }
            if (strcmp(s->segname, "__TEXT") == 0) {
                text_vmaddr = s->vmaddr;
                have_text = 1;
            }
            out->seg_count++;
            break;
        }
        case MACHO_LC_MAIN: {
            if (cmdsize < 24) {
                return -3;
            }
            if (rd64(data, len, off + 8, &entryoff) != 0) {
                return -3;
            }
            have_main = 1;
            break;
        }
        case MACHO_LC_UNIXTHREAD: {
            if (cmdsize < 16) {
                return -3;
            }
            uint32_t flavor, count;
            if (rd32(data, len, off + 8, &flavor) != 0 ||
                rd32(data, len, off + 12, &count) != 0) {
                return -3;
            }
            /* x86_64 スレッド状態: 21 レジスタ、RIP は 17 番目。 */
            if (flavor == MACHO_X86_64_THREAD_STATE &&
                count >= MACHO_X86_64_STATE_COUNT &&
                cmdsize >= 16 + MACHO_X86_64_STATE_COUNT * 4) {
                size_t rip_off = off + 16 + 16 * 8;
                if (rd64(data, len, rip_off, &thread_rip) != 0) {
                    return -3;
                }
                have_thread = 1;
            }
            break;
        }
        case MACHO_LC_LOAD_DYLIB:
        case MACHO_LC_LOAD_DYLINKER:
        case MACHO_LC_LOAD_WEAK_DYLIB:
        case MACHO_LC_REEXPORT_DYLIB:
        case MACHO_LC_LAZY_LOAD_DYLIB:
        case MACHO_LC_LOAD_UPWARD_DYLIB:
            return -6;
        default:
            break; /* 知らないコマンドは読み飛ばす */
        }
        off += cmdsize;
    }
    if (out->seg_count == 0) {
        return -5;
    }
    out->image_base = out->segs[0].vmaddr;
    for (int i = 1; i < out->seg_count; i++) {
        if (out->segs[i].vmaddr < out->image_base) {
            out->image_base = out->segs[i].vmaddr;
        }
    }
    if (have_main) {
        if (!have_text) {
            return -5;
        }
        out->entry = text_vmaddr + entryoff;
        out->has_main = 1;
    } else if (have_thread) {
        out->entry = thread_rip;
    } else {
        return -7;
    }
    /* 入口がどこかのセグメントの中にあること。 */
    {
        int inside = 0;
        for (int i = 0; i < out->seg_count; i++) {
            const macho_segment *s = &out->segs[i];
            if (out->entry >= s->vmaddr &&
                out->entry < s->vmaddr + s->vmsize) {
                inside = 1;
            }
        }
        if (!inside) {
            return -5;
        }
    }
    return 0;
}
