/*
 * 後付けドライバローダの実装。ELF64 ET_REL を解析し、SHF_ALLOC
 * セクションを 1 つの確保ブロックに並べ、シンボルを解決して
 * RELA 再配置を適用する。未定義シンボルはドライバ API 表に
 * あるものだけを許可する (それ以外は -3)。
 */
#include "drvload.h"

#include "heap.h"
#include "rtc.h"
#include "serial.h"
#include "string.h"
#include "../shell/vfs.h"

/* ---- ドライバ API の実装 (カーネル側の実体) ---- */

void prism_drv_log(const char *msg) {
    serial_write("driver: ");
    serial_write_line(msg != NULL ? msg : "(null)");
}

void *prism_drv_alloc(size_t n) { return kmalloc(n); }

void prism_drv_free(void *p) { kfree(p); }

int prism_drv_vfs_write(const char *name, const char *data, size_t len) {
    return vfs_write(name, data, len);
}

int prism_drv_vfs_read(const char *name, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) {
        return -1; /* cap - 1 の下溢れ防止 (監査 F12) */
    }
    size_t len = 0;
    const char *p = vfs_read_ptr(name, &len);
    if (p == NULL) {
        return -1;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = p[i];
    }
    buf[len] = '\0';
    return (int)len;
}

uint64_t prism_drv_tick(void) {
    /* 時刻ベースの単調でないティック (秒)。ドライバの
     * ログ時刻付け程度の用途を想定する。 */
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        return 0;
    }
    return (uint64_t)rtc_epoch_seconds(&t);
}

/* ---- API 表 ---- */

struct api_sym {
    const char *name;
    void *addr;
};

static const struct api_sym g_api[] = {
    { "prism_drv_log", (void *)prism_drv_log },
    { "prism_drv_alloc", (void *)prism_drv_alloc },
    { "prism_drv_free", (void *)prism_drv_free },
    { "prism_drv_vfs_write", (void *)prism_drv_vfs_write },
    { "prism_drv_vfs_read", (void *)prism_drv_vfs_read },
    { "prism_drv_tick", (void *)prism_drv_tick },
};

static void *api_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(g_api) / sizeof(g_api[0]); i++) {
        if (strcmp(name, g_api[i].name) == 0) {
            return g_api[i].addr;
        }
    }
    return NULL;
}

/* ---- ELF64 (必要な部分だけ) ---- */

#define ELFCLASS64 2
#define ET_REL 1
#define EM_X86_64 62
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define STT_SECTION 3
#define R_64 1
#define R_PC32 2
#define R_PLT32 4
#define R_GOTPCREL 9
#define R_32 10
#define R_32S 11
#define R_PC64 13
#define R_GOTPCRELX 41
#define R_REX_GOTPCRELX 42

struct elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
};

struct elf64_sym {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};

struct elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
};

static char g_drv_names[DRV_MAX_LOADED][48];
static int g_drv_count;

int drv_loaded_count(void) { return g_drv_count; }

const char *drv_loaded_name(int i) {
    if (i < 0 || i >= g_drv_count) {
        return NULL;
    }
    return g_drv_names[i];
}

static uint64_t align_up(uint64_t v, uint64_t a) {
    if (a <= 1) {
        return v;
    }
    return (v + a - 1) & ~(a - 1);
}

/* 再配置の書き込み先は命令のオペランド位置などで 4/8 バイト
 * 整列していないことがある。ポインタ経由の直接ストアは
 * 未定義動作なので、memcpy 経由で書く。 */
static void store_u32(uint64_t addr, uint32_t v) {
    memcpy((void *)(uintptr_t)addr, &v, sizeof(v));
}

static void store_u64(uint64_t addr, uint64_t v) {
    memcpy((void *)(uintptr_t)addr, &v, sizeof(v));
}

int drv_load(const char *name, const uint8_t *image, size_t len) {
    if (g_drv_count >= DRV_MAX_LOADED) {
        return -6;
    }
    if (image == NULL || len < sizeof(struct elf64_ehdr)) {
        return -1;
    }
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_type != ET_REL ||
        eh->e_machine != EM_X86_64) {
        return -1;
    }
    if (eh->e_shoff == 0 || eh->e_shentsize < sizeof(struct elf64_shdr) ||
        eh->e_shoff + (uint64_t)eh->e_shnum * eh->e_shentsize > len ||
        eh->e_shnum == 0 || eh->e_shnum > 64 ||
        (eh->e_shoff % 8) != 0) {
        return -2;
    }
    const struct elf64_shdr *sh =
        (const struct elf64_shdr *)(image + eh->e_shoff);

    /* シンボル表と文字列表を探す。 */
    const struct elf64_shdr *symsh = NULL;
    const struct elf64_shdr *strsh = NULL;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symsh = &sh[i];
            if (sh[i].sh_link < eh->e_shnum) {
                strsh = &sh[sh[i].sh_link];
            }
        }
    }
    if (symsh == NULL || strsh == NULL ||
        symsh->sh_entsize != sizeof(struct elf64_sym) ||
        (symsh->sh_offset % 8) != 0 ||
        symsh->sh_offset + symsh->sh_size > len ||
        strsh->sh_offset + strsh->sh_size > len) {
        return -2;
    }
    const struct elf64_sym *syms =
        (const struct elf64_sym *)(image + symsh->sh_offset);
    size_t nsyms = symsh->sh_size / symsh->sh_entsize;
    const char *strtab = (const char *)(image + strsh->sh_offset);

    /* SHF_ALLOC セクションの配置サイズを数える。細工対策
     * (監査 F2): セクションサイズはファイル長以下に限り、
     * 配置揃えは 2 の冪 (4096 まで) のみ許可し、合計の
     * 溢れ・異常な巨大化をここで拒否する。 */
    uint64_t total = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_flags & SHF_ALLOC) {
            uint64_t al = sh[i].sh_addralign;
            if (sh[i].sh_size > (uint64_t)len ||
                (al & (al - 1)) != 0 || al > 4096) {
                return -2;
            }
            total = align_up(total, al);
            total += sh[i].sh_size;
            if (total > (64ull << 20)) {
                return -2;
            }
        }
    }
    if (total == 0) {
        return -2;
    }
    /* パッケージは -fpie でビルドされ、外部参照は GOT 経由に
     * なる。GOT スロットは配置ブロックの末尾に取る (シンボル
     * ごとに 8 バイト、最大 128 個)。 */
    uint64_t got_base_off = align_up(total, 8);
    total = got_base_off + 128 * 8;
    uint8_t *base = (uint8_t *)kmalloc((size_t)total);
    if (base == NULL) {
        return -7;
    }
    for (uint64_t i = 0; i < total; i++) {
        base[i] = 0;
    }
    /* セクションごとの配置先アドレス (0 = 配置しない)。 */
    uint64_t sec_addr[64];
    uint64_t off = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        sec_addr[i] = 0;
        if (sh[i].sh_flags & SHF_ALLOC) {
            off = align_up(off, sh[i].sh_addralign);
            if (off + sh[i].sh_size > got_base_off) {
                kfree(base);
                return -2;
            }
            sec_addr[i] = (uint64_t)(base + off);
            if (sh[i].sh_type == SHT_PROGBITS) {
                if (sh[i].sh_offset + sh[i].sh_size > len) {
                    kfree(base);
                    return -2;
                }
                const uint8_t *src = image + sh[i].sh_offset;
                for (uint64_t k = 0; k < sh[i].sh_size; k++) {
                    base[off + k] = src[k];
                }
            }
            off += sh[i].sh_size;
        }
    }

    /* シンボルの解決値を決める。 */
    uint64_t *sym_val =
        (uint64_t *)kmalloc(nsyms * sizeof(uint64_t));
    if (sym_val == NULL) {
        kfree(base);
        return -7;
    }
    int64_t init_addr = 0;
    for (size_t i = 0; i < nsyms; i++) {
        sym_val[i] = 0;
        if (syms[i].st_shndx == 0) {
            /* 未定義: API 表からのみ解決する。 */
            if (syms[i].st_name >= strsh->sh_size) {
                continue;
            }
            const char *sn = strtab + syms[i].st_name;
            if (sn[0] == '\0') {
                continue; /* セクション記号など */
            }
            if (strcmp(sn, "_GLOBAL_OFFSET_TABLE_") == 0) {
                /* GOT の先頭アドレスとして解決する。 */
                sym_val[i] = (uint64_t)(base + got_base_off);
                continue;
            }
            void *a = api_lookup(sn);
            if (a == NULL) {
                serial_write("driver: unresolved symbol: ");
                serial_write_line(sn);
                kfree(sym_val);
                kfree(base);
                return -3;
            }
            sym_val[i] = (uint64_t)a;
        } else if (syms[i].st_shndx < eh->e_shnum &&
                   sec_addr[syms[i].st_shndx] != 0) {
            sym_val[i] = sec_addr[syms[i].st_shndx] + syms[i].st_value;
        }
        if (syms[i].st_name < strsh->sh_size &&
            strcmp(strtab + syms[i].st_name, "prism_driver_init") == 0 &&
            syms[i].st_shndx != 0) {
            init_addr = (int64_t)sym_val[i];
        }
    }
    if (init_addr == 0) {
        kfree(sym_val);
        kfree(base);
        return -5;
    }

    uint64_t got_addr[128];
    for (int gi = 0; gi < 128; gi++) {
        got_addr[gi] = 0;
    }
    int got_next = 0;

    /* RELA 再配置を適用する。 */
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_RELA) {
            continue;
        }
        if (sh[i].sh_entsize != sizeof(struct elf64_rela) ||
            (sh[i].sh_offset % 8) != 0) {
            kfree(sym_val);
            kfree(base);
            return -2;
        }
        uint32_t target = sh[i].sh_info;
        if (target >= eh->e_shnum || sec_addr[target] == 0) {
            continue;
        }
        if (sh[i].sh_offset + sh[i].sh_size > len) {
            kfree(sym_val);
            kfree(base);
            return -2;
        }
        const struct elf64_rela *relas =
            (const struct elf64_rela *)(image + sh[i].sh_offset);
        size_t n = sh[i].sh_size / sh[i].sh_entsize;
        for (size_t k = 0; k < n; k++) {
            uint32_t symidx = (uint32_t)(relas[k].r_info >> 32);
            uint32_t type = (uint32_t)relas[k].r_info;
            if (symidx >= nsyms) {
                kfree(sym_val);
                kfree(base);
                return -2;
            }
            uint64_t S = sym_val[symidx];
            /* セクションシンボルはセクション先頭 + 加数が値。 */
            if ((syms[symidx].st_info & 0xF) == STT_SECTION &&
                syms[symidx].st_shndx < eh->e_shnum) {
                S = sec_addr[syms[symidx].st_shndx];
            }
            /* 書き込み先は対象セクションの範囲内に限る
             * (監査 F2: r_offset を大きくした細工で確保域外
             * へ書けてしまう問題への対策)。 */
            uint64_t width =
                (type == R_64 || type == R_PC64) ? 8 : 4;
            if (relas[k].r_offset > sh[target].sh_size ||
                width > sh[target].sh_size - relas[k].r_offset) {
                kfree(sym_val);
                kfree(base);
                return -2;
            }
            uint64_t P = sec_addr[target] + relas[k].r_offset;
            int64_t A = relas[k].r_addend;
            if (type == R_GOTPCREL || type == R_GOTPCRELX ||
                type == R_REX_GOTPCRELX) {
                if (symidx >= 128) {
                    kfree(sym_val);
                    kfree(base);
                    return -4;
                }
                if (got_addr[symidx] == 0) {
                    if (got_next >= 128) {
                        kfree(sym_val);
                        kfree(base);
                        return -4;
                    }
                    got_addr[symidx] =
                        (uint64_t)(base + got_base_off) +
                        (uint64_t)got_next * 8;
                    store_u64(got_addr[symidx], S);
                    got_next++;
                }
                store_u32(P, (uint32_t)((int64_t)got_addr[symidx] +
                                        A - (int64_t)P));
                continue;
            }
            switch (type) {
            case R_64:
                store_u64(P, S + (uint64_t)A);
                break;
            case R_PC32:
            case R_PLT32:
                store_u32(P, (uint32_t)((int64_t)S + A - (int64_t)P));
                break;
            case R_PC64:
                store_u64(P, (uint64_t)((int64_t)S + A - (int64_t)P));
                break;
            case R_32:
                store_u32(P, (uint32_t)(S + (uint64_t)A));
                break;
            case R_32S:
                store_u32(P, (uint32_t)((int64_t)S + A));
                break;
            default:
                kfree(sym_val);
                kfree(base);
                return -4;
            }
        }
    }
    kfree(sym_val);

    /* 初期化を呼ぶ。 */
    typedef int (*init_fn)(int api_version);
    int rc = ((init_fn)(uintptr_t)init_addr)(PRISM_DRV_API_VERSION);
    if (rc != 0) {
        serial_write_line("driver: init failed");
        kfree(base);
        return -5;
    }
    size_t nn = 0;
    while (name != NULL && name[nn] != '\0' && nn < 47) {
        g_drv_names[g_drv_count][nn] = name[nn];
        nn++;
    }
    g_drv_names[g_drv_count][nn] = '\0';
    g_drv_count++;
    serial_write("driver: loaded ");
    serial_write_line(g_drv_names[g_drv_count - 1]);
    /* base はドライバが生きている間は解放しない (アンロードは
     * 未対応: 関数ポインタが残るため。無効化は再起動後に効く)。 */
    return 0;
}
