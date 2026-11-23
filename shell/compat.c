/* PrismOS — 互換レイヤーの純粋ロジック (compat.h を見ること)。 */
#include "compat.h"

#include <string.h>

#include "pe.h"
#include "vfs.h"

/* ---- 小物 ---- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* ---- ELF の解析 ---- */

static int compat_parse_dynamic(const uint8_t *data, size_t len,
                                compat_elf_info *out, uint64_t dyn_off,
                                uint64_t dyn_filesz);

int compat_elf_parse(const uint8_t *data, size_t len, compat_elf_info *out) {
    if (data == NULL || out == NULL) {
        return -1;
    }
    if (len < 64) {
        return -1; /* 小さすぎてヘッダが読めない */
    }
    if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' ||
        data[3] != 'F') {
        return -2; /* ELF ではない */
    }
    if (data[4] != 2) {
        return -3; /* 64 ビットではない */
    }
    if (data[5] != 1) {
        return -4; /* リトルエンディアンではない */
    }
    if (data[6] != 1) {
        return -5; /* バージョンが違う */
    }
    uint16_t type = rd16(data + 16);
    if (type != 2 && type != 3) {
        return -6; /* 実行ファイル (EXEC/DYN) ではない */
    }
    if (rd16(data + 18) != 62) {
        return -7; /* x86-64 ではない */
    }
    uint64_t phoff = rd64(data + 32);
    uint16_t phentsize = rd16(data + 54);
    uint16_t phnum = rd16(data + 56);
    if (phentsize != 56) {
        return -8; /* プログラムヘッダの大きさが違う */
    }
    if (phnum == 0 || phnum > 16) {
        return -9; /* プログラムヘッダの数がおかしい */
    }
    if (phoff > len || (uint64_t)phnum * 56 > len - phoff) {
        return -10; /* プログラムヘッダがファイルをはみ出す */
    }
    out->entry = rd64(data + 24);
    out->is_exec = (type == 2);
    out->seg_count = 0;
    out->min_vaddr = 0;
    out->max_vaddr = 0;
    memset(&out->dyn, 0, sizeof(out->dyn));
    uint64_t dyn_off = 0;
    uint64_t dyn_filesz = 0;
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = data + phoff + (size_t)i * 56;
        uint32_t p_type = rd32(ph);
        if (p_type == 2) {
            dyn_off = rd64(ph + 8); /* PT_DYNAMIC */
            dyn_filesz = rd64(ph + 32);
        }
        if (p_type != 1) {
            continue; /* PT_LOAD だけ載せる */
        }
        if (out->seg_count >= COMPAT_MAX_SEGMENTS) {
            return -11; /* 載せる部分が多すぎる */
        }
        uint64_t offset = rd64(ph + 8);
        uint64_t vaddr = rd64(ph + 16);
        uint64_t filesz = rd64(ph + 32);
        uint64_t memsz = rd64(ph + 40);
        uint32_t flags = rd32(ph + 4);
        if (filesz > memsz) {
            return -12; /* filesz が memsz を超えている */
        }
        if (offset > len || filesz > len - offset) {
            return -13; /* 部分がファイルをはみ出す */
        }
        if (memsz > 1024 * 1024) {
            return -14; /* 1 個の部分が大きすぎる */
        }
        if (vaddr >= 0x0000800000000000ULL) {
            return -22; /* ユーザー空間 (下半分) の外 */
        }
        if (memsz > 0x0000800000000000ULL - vaddr) {
            return -24; /* vaddr + memsz が溢れる */
        }
        compat_segment *s = &out->segs[out->seg_count++];
        s->vaddr = vaddr;
        s->offset = offset;
        s->filesz = filesz;
        s->memsz = memsz;
        s->flags = flags;
        if (out->seg_count == 1 || vaddr < out->min_vaddr) {
            out->min_vaddr = vaddr;
        }
        if (vaddr + memsz > out->max_vaddr) {
            out->max_vaddr = vaddr + memsz;
        }
    }
    if (out->seg_count == 0) {
        return -16; /* 載せる部分が無い */
    }
    /* セグメント同士の重なりを許さない (共有ページで
     * 先に張った側の権限が残り、W^X が崩れるため)。 */
    for (int i = 0; i < out->seg_count; i++) {
        for (int j = i + 1; j < out->seg_count; j++) {
            uint64_t a0 = out->segs[i].vaddr;
            uint64_t a1 = a0 + out->segs[i].memsz;
            uint64_t b0 = out->segs[j].vaddr;
            uint64_t b1 = b0 + out->segs[j].memsz;
            if (a0 < b1 && b0 < a1) {
                return -23;
            }
        }
    }
    if (out->entry < out->min_vaddr || out->entry >= out->max_vaddr) {
        return -17; /* 入口が載せる範囲の外にある */
    }
    if (dyn_off != 0) {
        int drc = compat_parse_dynamic(data, len, out, dyn_off,
                                       dyn_filesz);
        if (drc != 0) {
            return drc;
        }
    }
    return 0;
}

/* 仮想アドレスをファイル内オフセットに直す (PT_LOAD の
 * file 範囲で探す)。見つからなければ 0 で失敗。 */
static int compat_v2o(const compat_elf_info *info, uint64_t vaddr,
                      uint64_t *out_off) {
    for (int i = 0; i < info->seg_count; i++) {
        const compat_segment *sg = &info->segs[i];
        if (vaddr >= sg->vaddr && vaddr - sg->vaddr < sg->filesz) {
            *out_off = sg->offset + (vaddr - sg->vaddr);
            return 1;
        }
    }
    return 0;
}

/* PT_DYNAMIC を読む。タグの値は仮想アドレスなので、使う前に
 * compat_v2o でファイル内オフセットに直す。 */
static int compat_parse_dynamic(const uint8_t *data, size_t len,
                                compat_elf_info *out, uint64_t dyn_off,
                                uint64_t dyn_filesz) {
    if (dyn_off > len || dyn_filesz > len - dyn_off ||
        dyn_filesz < 16 || (dyn_filesz % 16) != 0) {
        return -18;
    }
    uint64_t strtab_v = 0, symtab_v = 0, hash_v = 0, gnu_hash_v = 0;
    uint64_t rela_v = 0, rel_v = 0, jmprel_v = 0;
    uint64_t needed_off[COMPAT_MAX_NEEDED];
    int n_needed = 0;
    compat_dynamic_info *d = &out->dyn;
    size_t nent = (size_t)(dyn_filesz / 16);
    int terminated = 0;
    for (size_t i = 0; i < nent; i++) {
        const uint8_t *e = data + dyn_off + i * 16;
        int64_t tag = (int64_t)rd64(e);
        uint64_t val = rd64(e + 8);
        if (tag == 0) {
            terminated = 1;
            break;
        }
        switch (tag) {
        case 1: /* DT_NEEDED */
            if (n_needed >= COMPAT_MAX_NEEDED) {
                return -20;
            }
            needed_off[n_needed++] = val;
            break;
        case 4: hash_v = val; break;        /* DT_HASH */
        case 5: strtab_v = val; break;      /* DT_STRTAB */
        case 6: symtab_v = val; break;      /* DT_SYMTAB */
        case 7: rela_v = val; break;        /* DT_RELA */
        case 8: d->rela_sz = val; break;    /* DT_RELASZ */
        case 10: d->strsz = val; break;     /* DT_STRSZ */
        case 17: rel_v = val; break;        /* DT_REL */
        case 18: d->rel_sz = val; break;    /* DT_RELSZ */
        case 20:                            /* DT_PLTREL */
            d->pltrel_is_rela = (val == 7);
            break;
        case 23: jmprel_v = val; break;     /* DT_JMPREL */
        case 2: d->jmprel_sz = val; break;  /* DT_PLTRELSZ */
        case 12: d->init_vaddr = val; break; /* DT_INIT */
        case 25:                            /* DT_INIT_ARRAY */
            d->init_array_vaddr = val;
            break;
        case 27: d->init_array_sz = val; break; /* DT_INIT_ARRAYSZ */
        case 0x6ffffef5: gnu_hash_v = val; break; /* DT_GNU_HASH */
        default: break;
        }
    }
    if (!terminated || strtab_v == 0 || symtab_v == 0) {
        return -18;
    }
    if (!compat_v2o(out, strtab_v, &d->strtab_off) ||
        !compat_v2o(out, symtab_v, &d->symtab_off)) {
        return -21;
    }
    if (d->strtab_off > len || d->strsz > len - d->strtab_off) {
        return -18;
    }
    /* シンボル数: DT_HASH の nchain が確実。無ければ
     * GNU ハッシュのバケツをたどって最大の番号を求める。 */
    if (hash_v != 0) {
        uint64_t hoff;
        if (!compat_v2o(out, hash_v, &hoff) || hoff + 8 > len) {
            return -21;
        }
        d->sym_count = rd32(data + hoff + 4);
    } else if (gnu_hash_v != 0) {
        uint64_t goff;
        if (!compat_v2o(out, gnu_hash_v, &goff) || goff + 16 > len) {
            return -21;
        }
        uint32_t nbuckets = rd32(data + goff);
        uint32_t symoffset = rd32(data + goff + 4);
        uint32_t bloom = rd32(data + goff + 8);
        uint64_t buckets = goff + 16 + (uint64_t)bloom * 8;
        if (buckets + (uint64_t)nbuckets * 4 > len) {
            return -18;
        }
        uint32_t maxsym = 0;
        for (uint32_t b = 0; b < nbuckets; b++) {
            uint32_t v = rd32(data + buckets + (uint64_t)b * 4);
            if (v > maxsym) {
                maxsym = v;
            }
        }
        if (maxsym < symoffset) {
            d->sym_count = symoffset;
        } else {
            uint64_t chain = buckets + (uint64_t)nbuckets * 4 +
                             (uint64_t)(maxsym - symoffset) * 4;
            uint32_t count = maxsym;
            for (;;) {
                if (chain + 4 > len) {
                    return -18;
                }
                uint32_t cv = rd32(data + chain);
                count++;
                chain += 4;
                if ((cv & 1) != 0) {
                    break;
                }
                if (count > 100000) {
                    return -18;
                }
            }
            d->sym_count = count;
        }
    } else {
        return -18; /* シンボル数を知る手がかりが無い */
    }
    if (d->symtab_off + (uint64_t)d->sym_count * 24 > len) {
        return -18;
    }
    if (rela_v != 0 &&
        !compat_v2o(out, rela_v, &d->rela_off)) {
        return -21;
    }
    if (rel_v != 0 && !compat_v2o(out, rel_v, &d->rel_off)) {
        return -21;
    }
    if (jmprel_v != 0 &&
        !compat_v2o(out, jmprel_v, &d->jmprel_off)) {
        return -21;
    }
    if (d->init_array_vaddr != 0 &&
        !compat_v2o(out, d->init_array_vaddr, &d->init_array_off)) {
        return -21;
    }
    if ((d->rela_sz % 24) != 0 || (d->rel_sz % 16) != 0 ||
        (d->jmprel_sz % 24) != 0) {
        return -18;
    }
    /* DT_NEEDED の名前を文字列テーブルから複写する。 */
    for (int i = 0; i < n_needed; i++) {
        uint64_t so = needed_off[i];
        if (so >= d->strsz) {
            return -18;
        }
        const char *name = (const char *)data + d->strtab_off + so;
        size_t maxlen = (size_t)(d->strsz - so);
        size_t n = 0;
        while (n < maxlen && name[n] != '\0') {
            n++;
        }
        if (n >= COMPAT_NEEDED_NAME_LEN) {
            return -19;
        }
        memcpy(d->needed[i], name, n);
        d->needed[i][n] = '\0';
    }
    d->needed_count = n_needed;
    d->present = 1;
    return 0;
}

const char *compat_elf_error(int rc) {
    switch (rc) {
    case 0:
        return "ok";
    case -1:
        return "file is too small for an ELF header";
    case -2:
        return "not an ELF file";
    case -3:
        return "not a 64-bit ELF file";
    case -4:
        return "not a little-endian ELF file";
    case -5:
        return "bad ELF version";
    case -6:
        return "not an executable (needs EXEC or DYN type)";
    case -7:
        return "not an x86-64 executable";
    case -8:
        return "bad program header size";
    case -9:
        return "bad program header count";
    case -10:
        return "program headers run past the end of the file";
    case -11:
        return "too many load segments";
    case -12:
        return "segment file size exceeds memory size";
    case -13:
        return "segment runs past the end of the file";
    case -14:
        return "segment is too large";
    case -15:
        return "unused";
    case -16:
        return "no loadable segments";
    case -17:
        return "entry point is outside the loaded segments";
    case -18:
        return "malformed dynamic section";
    case -19:
        return "needed library name is too long";
    case -20:
        return "too many needed libraries";
    case -21:
        return "dynamic address is outside the loaded segments";
    case -22:
        return "segment address outside user space";
    case -23:
        return "load segments overlap";
    case -24:
        return "segment address range overflows";
    default:
        return "unknown ELF error";
    }
}

/* ---- 呼び出しの対応表 ---- */

static const struct {
    uint64_t nr;
    const char *name;
} COMPAT_CALLS[] = {
    { COMPAT_SYS_EXIT, "exit" },
    { COMPAT_SYS_WRITE, "write" },
    { COMPAT_SYS_READ, "read" },
    { COMPAT_SYS_OPEN, "open" },
    { COMPAT_SYS_CLOSE, "close" },
    { COMPAT_SYS_FSTAT, "fstat" },
    { COMPAT_SYS_BRK, "brk" },
    { COMPAT_SYS_GETPID, "getpid" },
    { COMPAT_SYS_LSEEK, "lseek" },
    { COMPAT_SYS_FB_INFO, "fb_info" },
    { COMPAT_SYS_FB_FILL, "fb_fill" },
    { COMPAT_SYS_FB_BLIT, "fb_blit" },
    { COMPAT_SYS_FB_TEXT, "fb_text" },
    { COMPAT_SYS_FB_FLUSH, "fb_flush" },
    { COMPAT_SYS_NET_RESOLVE, "net_resolve" },
    { COMPAT_SYS_NET_CONNECT, "net_connect" },
    { COMPAT_SYS_NET_SEND, "net_send" },
    { COMPAT_SYS_NET_RECV, "net_recv" },
    { COMPAT_SYS_NET_CLOSE, "net_close" },
};

const char *compat_syscall_name(uint64_t nr) {
    for (size_t i = 0; i < sizeof(COMPAT_CALLS) / sizeof(COMPAT_CALLS[0]);
         i++) {
        if (COMPAT_CALLS[i].nr == nr) {
            return COMPAT_CALLS[i].name;
        }
    }
    return NULL;
}

int compat_syscall_supported(uint64_t nr) {
    return compat_syscall_name(nr) != NULL;
}

/* ---- イメージの載せ降ろし ---- */

int compat_load_image(const compat_elf_info *info, const uint8_t *data,
                      size_t len, compat_alloc_fn alloc, compat_image *out) {
    if (info == NULL || data == NULL || alloc == NULL || out == NULL) {
        return -1;
    }
    for (int i = 0; i < COMPAT_MAX_SEGMENTS; i++) {
        out->seg_base[i] = NULL;
        out->seg_vaddr[i] = 0;
        out->seg_memsz[i] = 0;
    }
    out->seg_count = 0;
    out->entry = 0;
    out->base = 0;
    out->brk_base = NULL;
    out->brk_cur = 0;
    for (int i = 0; i < info->seg_count; i++) {
        const compat_segment *s = &info->segs[i];
        uint8_t *mem = NULL;
        if (s->memsz > 0) {
            mem = (uint8_t *)alloc((size_t)s->memsz);
            if (mem == NULL) {
                return -1;
            }
            if (s->filesz > 0) {
                memcpy(mem, data + s->offset, (size_t)s->filesz);
            }
            if (s->memsz > s->filesz) {
                memset(mem + s->filesz, 0,
                       (size_t)(s->memsz - s->filesz));
            }
        }
        out->seg_base[i] = mem;
        out->seg_vaddr[i] = s->vaddr;
        out->seg_memsz[i] = s->memsz;
        out->seg_count++;
    }
    /* 入口を載せた後のアドレスに直す。 */
    out->entry = 0;
    out->base = 0;
    for (int i = 0; i < info->seg_count; i++) {
        const compat_segment *s = &info->segs[i];
        if (info->entry >= s->vaddr &&
            info->entry < s->vaddr + s->memsz) {
            out->entry = (uint64_t)out->seg_base[i] +
                         (info->entry - s->vaddr);
        }
        if (out->base == 0 ||
            (uint64_t)out->seg_base[i] < out->base) {
            out->base = (uint64_t)out->seg_base[i];
        }
    }
    if (out->entry == 0) {
        return -1;
    }
    out->brk_base = (uint8_t *)alloc(COMPAT_BRK_SIZE);
    if (out->brk_base == NULL) {
        return -1;
    }
    out->brk_cur = (uint64_t)out->brk_base;
    (void)len;
    return 0;
}

void compat_free_image(compat_image *img, compat_free_fn free_fn) {
    if (img == NULL) {
        return;
    }
    if (free_fn != NULL) {
        for (int i = 0; i < img->seg_count; i++) {
            if (img->seg_base[i] != NULL) {
                free_fn(img->seg_base[i]);
            }
        }
        if (img->brk_base != NULL) {
            free_fn(img->brk_base);
        }
    }
    for (int i = 0; i < COMPAT_MAX_SEGMENTS; i++) {
        img->seg_base[i] = NULL;
        img->seg_vaddr[i] = 0;
        img->seg_memsz[i] = 0;
    }
    img->seg_count = 0;
    img->entry = 0;
    img->base = 0;
    img->brk_base = NULL;
    img->brk_cur = 0;
}

/* ---- ポインタの検証 ---- */

typedef struct {
    uint64_t base;
    uint64_t len;
} compat_range;

static compat_range g_ranges[COMPAT_MAX_SEGMENTS + 2];
static int g_range_count;

void compat_set_ranges(const compat_image *img, const uint8_t *stack_base,
                       size_t stack_size) {
    g_range_count = 0;
    if (img != NULL) {
        for (int i = 0; i < img->seg_count; i++) {
            if (img->seg_base[i] != NULL && img->seg_memsz[i] > 0) {
                g_ranges[g_range_count].base =
                    (uint64_t)img->seg_base[i];
                g_ranges[g_range_count].len = img->seg_memsz[i];
                g_range_count++;
            }
        }
        if (img->brk_base != NULL) {
            g_ranges[g_range_count].base = (uint64_t)img->brk_base;
            g_ranges[g_range_count].len = COMPAT_BRK_SIZE;
            g_range_count++;
        }
    }
    if (stack_base != NULL && stack_size > 0) {
        g_ranges[g_range_count].base = (uint64_t)stack_base;
        g_ranges[g_range_count].len = (uint64_t)stack_size;
        g_range_count++;
    }
}

void compat_clear_ranges(void) {
    g_range_count = 0;
}

int compat_ptr_ok(uint64_t ptr, size_t len) {
    if (len == 0) {
        return 1;
    }
    for (int i = 0; i < g_range_count; i++) {
        uint64_t base = g_ranges[i].base;
        uint64_t end = base + g_ranges[i].len;
        if (ptr >= base && ptr < end && len <= end - ptr) {
            return 1;
        }
    }
    return 0;
}

/* ゲストの指す文字列をコピーする。NUL が範囲内に無ければ -1。 */
static int compat_copy_guest_string(uint64_t ptr, char *out, size_t cap) {
    if (cap == 0) {
        return -1;
    }
    size_t n = 0;
    while (n + 1 < cap) {
        if (!compat_ptr_ok(ptr + n, 1)) {
            return -1;
        }
        char c = *(const char *)(uintptr_t)(ptr + n);
        out[n] = c;
        if (c == '\0') {
            return 0;
        }
        n++;
    }
    return -1;
}

/* ---- 呼び出しの実装 ---- */

int compat_exit_pending;
int compat_exit_code;

static const compat_io *g_io;
static compat_image *g_img;
static compat_alloc_fn g_alloc;
static compat_free_fn g_free;

void compat_set_context(const compat_io *io, compat_image *img,
                        compat_alloc_fn alloc, compat_free_fn free_fn) {
    g_io = io;
    g_img = img;
    g_alloc = alloc;
    g_free = free_fn;
}

typedef struct {
    int used;
    int write_mode;
    char name[32];
    char *data; /* 読み込み用: 中身のコピー。書き込み用: 未使用 */
    size_t size;
    size_t off;
    char *wbuf; /* 書き込み用: ためておく領域 */
    size_t wlen;
} compat_fd;

static compat_fd g_fds[COMPAT_MAX_FDS];

/* ゲストがこの実行で作ったファイル (書き込みを許す範囲)。
 * 既存ファイルへの書き込みは、保護ファイルでなくても
 * この一覧にあるものだけに限る。 */
static char g_created[COMPAT_MAX_FDS][32];
static int g_created_count;

static int compat_created_contains(const char *path) {
    for (int i = 0; i < g_created_count; i++) {
        if (strcmp(g_created[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void compat_created_add(const char *path) {
    if (g_created_count >= COMPAT_MAX_FDS ||
        compat_created_contains(path)) {
        return;
    }
    size_t i = 0;
    while (path[i] != '\0' && i + 1 < sizeof(g_created[0])) {
        g_created[g_created_count][i] = path[i];
        i++;
    }
    g_created[g_created_count][i] = '\0';
    g_created_count++;
}

/* ゲストから触らせないファイル。アカウント (PIN の
 * ハッシュ)、設定 (ロックアウト緩和)、許可リスト、
 * 監査ログ、メール資格情報、生体テンプレート、秘密鍵。 */
static int compat_path_protected(const char *path) {
    static const char *const names[] = {
        ".account", "settings.txt", "allowlist.txt", "audit.log",
        "remote.conf", "remote.lock", "mail.conf", "bio.db",
        "pqkem.key", "pqsign.key",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(path, names[i]) == 0) {
            return 1;
        }
    }
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".key") == 0) {
        return 1; /* 秘密鍵ファイル全般 */
    }
    return 0;
}

/* ネットワークのハンドルの開閉状態 (実物はフック側)。 */
static int g_net_open[COMPAT_NET_MAX_STREAMS];

void compat_reset_state(void) {
    compat_exit_pending = 0;
    compat_exit_code = 0;
    g_created_count = 0;
    for (int i = 0; i < COMPAT_NET_MAX_STREAMS; i++) {
        g_net_open[i] = 0;
    }
    for (int i = 0; i < COMPAT_MAX_FDS; i++) {
        g_fds[i].used = 0;
        g_fds[i].write_mode = 0;
        g_fds[i].name[0] = '\0';
        g_fds[i].data = NULL;
        g_fds[i].size = 0;
        g_fds[i].off = 0;
        g_fds[i].wbuf = NULL;
        g_fds[i].wlen = 0;
    }
}

static compat_fd *fd_lookup64(uint64_t fd) {
    if (fd < 3 || fd >= 3 + COMPAT_MAX_FDS) {
        return NULL;
    }
    compat_fd *f = &g_fds[fd - 3];
    return f->used ? f : NULL;
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd == 1 || fd == 2) {
        if (!compat_ptr_ok(buf, (size_t)len)) {
            return -COMPAT_EFAULT;
        }
        if (g_io != NULL && g_io->write_out != NULL) {
            g_io->write_out((const char *)(uintptr_t)buf, (size_t)len);
        }
        return (int64_t)len;
    }
    compat_fd *f = fd_lookup64(fd);
    if (f == NULL) {
        return -COMPAT_EBADF;
    }
    if (!f->write_mode) {
        return -COMPAT_EBADF;
    }
    if (!compat_ptr_ok(buf, (size_t)len)) {
        return -COMPAT_EFAULT;
    }
    /* 書き込み位置は f->off (lseek で動かせる)。位置が
     * 現在の末尾より先なら間を 0 で埋める。 */
    if (f->off > COMPAT_WRITE_BUF_MAX ||
        len > COMPAT_WRITE_BUF_MAX - f->off) {
        return -COMPAT_ENOSPC;
    }
    if (f->wbuf == NULL) {
        if (g_alloc == NULL) {
            return -COMPAT_ENOSPC;
        }
        f->wbuf = (char *)g_alloc(COMPAT_WRITE_BUF_MAX);
        if (f->wbuf == NULL) {
            return -COMPAT_ENOSPC;
        }
    }
    if (f->off > f->wlen) {
        memset(f->wbuf + f->wlen, 0, f->off - f->wlen);
        f->wlen = f->off;
    }
    memcpy(f->wbuf + f->off, (const void *)(uintptr_t)buf, (size_t)len);
    f->off += (size_t)len;
    if (f->off > f->wlen) {
        f->wlen = f->off;
    }
    return (int64_t)len;
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!compat_ptr_ok(buf, (size_t)len)) {
        return -COMPAT_EFAULT;
    }
    if (fd == 0) {
        if (g_io == NULL || g_io->read_in == NULL) {
            return 0;
        }
        int n = g_io->read_in((char *)(uintptr_t)buf, (size_t)len);
        return n < 0 ? 0 : n;
    }
    compat_fd *f = fd_lookup64(fd);
    if (f == NULL) {
        return -COMPAT_EBADF;
    }
    if (f->write_mode) {
        return -COMPAT_EBADF;
    }
    if (f->off >= f->size) {
        return 0;
    }
    size_t left = f->size - f->off;
    size_t n = (size_t)len < left ? (size_t)len : left;
    memcpy((void *)(uintptr_t)buf, f->data + f->off, n);
    f->off += n;
    return (int64_t)n;
}

static int64_t sys_open(uint64_t path_ptr, uint64_t flags) {
    char path[32];
    if (compat_copy_guest_string(path_ptr, path, sizeof(path)) != 0) {
        return -COMPAT_EFAULT;
    }
    uint64_t mode = flags & 3;
    if (mode == COMPAT_O_RDWR) {
        return -COMPAT_EINVAL; /* 読み書き両方は未対応 */
    }
    int write_mode = (mode == COMPAT_O_WRONLY);
    int create = (flags & COMPAT_O_CREAT) != 0;
    if (compat_path_protected(path)) {
        return -COMPAT_EACCES; /* 保護ファイルは読み書き禁止 */
    }
    size_t len = 0;
    const char *content = vfs_read_ptr(path, &len);
    if (content == NULL && !create) {
        return -COMPAT_ENOENT;
    }
    if (write_mode && content != NULL &&
        !compat_created_contains(path)) {
        /* 既存ファイルの上書きは、この実行で自分が
         * 作ったものだけ。組み込みや他人の成果物を
         * ゲストが壊せないようにする。 */
        return -COMPAT_EACCES;
    }
    if (content == NULL && create) {
        /* 空で作る。読み込み用なら大きさ 0 のまま。 */
        if (vfs_write(path, "", 0) != 0) {
            return -COMPAT_ENOSPC;
        }
        compat_created_add(path);
        len = 0;
        content = vfs_read_ptr(path, &len);
    }
    int slot = -1;
    for (int i = 0; i < COMPAT_MAX_FDS; i++) {
        if (!g_fds[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -COMPAT_EMFILE;
    }
    compat_fd *f = &g_fds[slot];
    f->used = 1;
    f->write_mode = write_mode;
    f->off = 0;
    f->wlen = 0;
    f->wbuf = NULL;
    f->data = NULL;
    f->size = 0;
    size_t i = 0;
    while (path[i] != '\0' && i + 1 < sizeof(f->name)) {
        f->name[i] = path[i];
        i++;
    }
    f->name[i] = '\0';
    if (!write_mode) {
        f->size = len;
        if (len > 0) {
            if (g_alloc == NULL) {
                f->used = 0;
                return -COMPAT_ENOSPC;
            }
            f->data = (char *)g_alloc(len);
            if (f->data == NULL) {
                f->used = 0;
                return -COMPAT_ENOSPC;
            }
            memcpy(f->data, content, len);
        }
    } else if (content != NULL && !(flags & COMPAT_O_TRUNC)) {
        /* 書き込みでも既存の内容をバッファへ先読みする
         * (lseek して一部だけ書き換える用途のため)。
         * TRUNC 指定時は空から始める。 */
        if (len > COMPAT_WRITE_BUF_MAX) {
            f->used = 0;
            return -COMPAT_ENOSPC;
        }
        if (len > 0) {
            if (g_alloc == NULL) {
                f->used = 0;
                return -COMPAT_ENOSPC;
            }
            f->wbuf = (char *)g_alloc(COMPAT_WRITE_BUF_MAX);
            if (f->wbuf == NULL) {
                f->used = 0;
                return -COMPAT_ENOSPC;
            }
            memcpy(f->wbuf, content, len);
            f->wlen = len;
        }
    }
    return 3 + slot;
}

static int64_t sys_close(uint64_t fd) {
    if (fd <= 2) {
        return 0; /* 標準入出力は閉じない */
    }
    compat_fd *f = fd_lookup64(fd);
    if (f == NULL) {
        return -COMPAT_EBADF;
    }
    if (f->write_mode) {
        /* ためた内容をファイルに書き出す。ここで作る・上書きする。 */
        if (vfs_write(f->name, f->wbuf == NULL ? "" : f->wbuf,
                      f->wlen) != 0) {
            if (f->data != NULL && g_free != NULL) {
                g_free(f->data);
            }
            if (f->wbuf != NULL && g_free != NULL) {
                g_free(f->wbuf);
            }
            f->used = 0;
            return -COMPAT_ENOSPC;
        }
    }
    if (f->data != NULL && g_free != NULL) {
        g_free(f->data);
    }
    if (f->wbuf != NULL && g_free != NULL) {
        g_free(f->wbuf);
    }
    f->used = 0;
    f->data = NULL;
    f->wbuf = NULL;
    f->size = 0;
    f->off = 0;
    f->wlen = 0;
    return 0;
}

static int64_t sys_fstat(uint64_t fd, uint64_t buf) {
    if (!compat_ptr_ok(buf, 8)) {
        return -COMPAT_EFAULT;
    }
    uint64_t size = 0;
    if (fd <= 2) {
        size = 0;
    } else {
        compat_fd *f = fd_lookup64(fd);
        if (f == NULL) {
            return -COMPAT_EBADF;
        }
        size = f->write_mode ? (uint64_t)f->wlen : (uint64_t)f->size;
    }
    *(uint64_t *)(uintptr_t)buf = size;
    return 0;
}

static int64_t sys_brk(uint64_t addr) {
    if (g_img == NULL || g_img->brk_base == NULL) {
        return -COMPAT_EINVAL;
    }
    uint64_t base = (uint64_t)g_img->brk_base;
    if (addr == 0) {
        return (int64_t)g_img->brk_cur;
    }
    if (addr >= base && addr <= base + COMPAT_BRK_SIZE) {
        g_img->brk_cur = addr;
        return (int64_t)addr;
    }
    return (int64_t)g_img->brk_cur;
}

static int64_t sys_lseek(uint64_t fd, uint64_t off_v,
                         uint64_t whence) {
    if (fd <= 2) {
        return -COMPAT_EINVAL; /* 標準入出力はシーク不可 */
    }
    compat_fd *f = fd_lookup64(fd);
    if (f == NULL) {
        return -COMPAT_EBADF;
    }
    int64_t off = (int64_t)off_v;
    int64_t base;
    int64_t limit;
    if (f->write_mode) {
        limit = (int64_t)f->wlen;
    } else {
        limit = (int64_t)f->size;
    }
    switch (whence) {
    case COMPAT_SEEK_SET:
        base = 0;
        break;
    case COMPAT_SEEK_CUR:
        base = (int64_t)f->off;
        break;
    case COMPAT_SEEK_END:
        base = limit;
        break;
    default:
        return -COMPAT_EINVAL;
    }
    int64_t next = base + off;
    if (next < 0) {
        return -COMPAT_EINVAL;
    }
    if (!f->write_mode && next > limit + 1048576) {
        return -COMPAT_EINVAL; /* 読み側の暴走防止 */
    }
    if (f->write_mode && next > COMPAT_WRITE_BUF_MAX) {
        return -COMPAT_EINVAL;
    }
    f->off = (size_t)next;
    return next;
}

/* ---- グラフィクスとネットワークの呼び出し ---- */

static const compat_ext_io *ext_io(void) {
    return (g_io != NULL) ? g_io->ext : NULL;
}

/* ゲストの文字列をカーネル側のバッファへ写す。1 バイト
 * ずつ範囲を確かめる (終端のない文字列で暴走しない)。 */
static int compat_copy_str(uint64_t ptr, char *out, size_t cap) {
    if (cap == 0) {
        return -1;
    }
    for (size_t i = 0; i + 1 < cap; i++) {
        if (!compat_ptr_ok(ptr + i, 1)) {
            return -1;
        }
        char c = *(const char *)(uintptr_t)(ptr + i);
        out[i] = c;
        if (c == '\0') {
            return 0;
        }
    }
    return -1; /* 長すぎる (終端が無い) */
}

static int64_t sys_fb_info(uint64_t ptr) {
    if (!compat_ptr_ok(ptr, 8)) {
        return -COMPAT_EFAULT;
    }
    uint32_t *out = (uint32_t *)(uintptr_t)ptr;
    out[0] = COMPAT_FB_WIDTH;
    out[1] = COMPAT_FB_HEIGHT;
    return 0;
}

/* 窓の範囲に切り取る。完全に外なら 0 (何もしない成功)。 */
static int fb_clip(uint32_t *x, uint32_t *y, uint32_t *w,
                   uint32_t *h) {
    if (*x >= COMPAT_FB_WIDTH || *y >= COMPAT_FB_HEIGHT) {
        return 0;
    }
    if (*w > COMPAT_FB_WIDTH - *x) {
        *w = COMPAT_FB_WIDTH - *x;
    }
    if (*h > COMPAT_FB_HEIGHT - *y) {
        *h = COMPAT_FB_HEIGHT - *y;
    }
    return *w > 0 && *h > 0;
}

static int64_t sys_fb_fill(uint64_t x, uint64_t y, uint64_t w,
                           uint64_t h, uint64_t color) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->fb_fill == NULL) {
        return -COMPAT_ENOSYS;
    }
    uint32_t cx = (uint32_t)x, cy = (uint32_t)y, cw = (uint32_t)w,
             ch = (uint32_t)h;
    if (!fb_clip(&cx, &cy, &cw, &ch)) {
        return 0;
    }
    return e->fb_fill(cx, cy, cw, ch, (uint32_t)color) == 0
               ? 0
               : -COMPAT_EINVAL;
}

static int64_t sys_fb_blit(uint64_t x, uint64_t y, uint64_t w,
                           uint64_t h, uint64_t ptr) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->fb_blit == NULL) {
        return -COMPAT_ENOSYS;
    }
    if (w == 0 || h == 0 || w > 256 || h > 256) {
        return -COMPAT_EINVAL;
    }
    uint32_t cx = (uint32_t)x, cy = (uint32_t)y, cw = (uint32_t)w,
             ch = (uint32_t)h;
    if (!fb_clip(&cx, &cy, &cw, &ch)) {
        return 0;
    }
    /* 切り取りで外れた分は元の画素列の行内オフセットに
     * なるが、ここでは左上起点のまま渡す (窓端にかかる
     * 場合は呼び出し側の責任で避けるのが前提)。 */
    if (cw != (uint32_t)w || ch != (uint32_t)h) {
        return -COMPAT_EINVAL;
    }
    if (!compat_ptr_ok(ptr, (size_t)w * (size_t)h * 4u)) {
        return -COMPAT_EFAULT;
    }
    return e->fb_blit(cx, cy, cw, ch,
                      (const uint32_t *)(uintptr_t)ptr) == 0
               ? 0
               : -COMPAT_EINVAL;
}

static int64_t sys_fb_text(uint64_t x, uint64_t y, uint64_t ptr,
                           uint64_t color) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->fb_text == NULL) {
        return -COMPAT_ENOSYS;
    }
    char buf[129];
    if (compat_copy_str(ptr, buf, sizeof(buf)) != 0) {
        return -COMPAT_EFAULT;
    }
    if (x >= COMPAT_FB_WIDTH || y >= COMPAT_FB_HEIGHT) {
        return 0;
    }
    return e->fb_text((uint32_t)x, (uint32_t)y, buf,
                      (uint32_t)color) == 0
               ? 0
               : -COMPAT_EINVAL;
}

static int64_t sys_fb_flush(void) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->fb_flush == NULL) {
        return -COMPAT_ENOSYS;
    }
    return e->fb_flush() == 0 ? 0 : -COMPAT_EINVAL;
}

static int64_t sys_net_resolve(uint64_t host_ptr, uint64_t out_ptr) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->net_resolve == NULL) {
        return -COMPAT_ENOSYS;
    }
    char host[64];
    if (compat_copy_str(host_ptr, host, sizeof(host)) != 0) {
        return -COMPAT_EFAULT;
    }
    if (!compat_ptr_ok(out_ptr, 4)) {
        return -COMPAT_EFAULT;
    }
    uint8_t ip[4];
    if (e->net_resolve(host, ip) != 0) {
        return -COMPAT_EINVAL;
    }
    uint8_t *out = (uint8_t *)(uintptr_t)out_ptr;
    for (int i = 0; i < 4; i++) {
        out[i] = ip[i];
    }
    return 0;
}

static int64_t sys_net_connect(uint64_t host_ptr, uint64_t port) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->net_connect == NULL) {
        return -COMPAT_ENOSYS;
    }
    if (port == 0 || port > 65535) {
        return -COMPAT_EINVAL;
    }
    char host[64];
    if (compat_copy_str(host_ptr, host, sizeof(host)) != 0) {
        return -COMPAT_EFAULT;
    }
    int h = e->net_connect(host, (uint16_t)port);
    if (h < 0 || h >= COMPAT_NET_MAX_STREAMS) {
        return -COMPAT_EINVAL;
    }
    g_net_open[h] = 1;
    return h;
}

static int net_handle_ok(uint64_t h) {
    return h < COMPAT_NET_MAX_STREAMS && g_net_open[h];
}

static int64_t sys_net_send(uint64_t h, uint64_t ptr, uint64_t len) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->net_send == NULL) {
        return -COMPAT_ENOSYS;
    }
    if (!net_handle_ok(h)) {
        return -COMPAT_EBADF;
    }
    if (len > COMPAT_WRITE_BUF_MAX) {
        return -COMPAT_EINVAL;
    }
    if (!compat_ptr_ok(ptr, (size_t)len)) {
        return -COMPAT_EFAULT;
    }
    int n = e->net_send((int)h, (const uint8_t *)(uintptr_t)ptr,
                        (size_t)len);
    return n < 0 ? -COMPAT_EINVAL : n;
}

static int64_t sys_net_recv(uint64_t h, uint64_t ptr, uint64_t cap) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->net_recv == NULL) {
        return -COMPAT_ENOSYS;
    }
    if (!net_handle_ok(h)) {
        return -COMPAT_EBADF;
    }
    if (cap > COMPAT_WRITE_BUF_MAX) {
        cap = COMPAT_WRITE_BUF_MAX;
    }
    if (!compat_ptr_ok(ptr, (size_t)cap)) {
        return -COMPAT_EFAULT;
    }
    int n = e->net_recv((int)h, (uint8_t *)(uintptr_t)ptr,
                        (size_t)cap);
    return n < 0 ? -COMPAT_EINVAL : n;
}

static int64_t sys_net_close(uint64_t h) {
    const compat_ext_io *e = ext_io();
    if (e == NULL || e->net_close == NULL) {
        return -COMPAT_ENOSYS;
    }
    if (!net_handle_ok(h)) {
        return -COMPAT_EBADF;
    }
    g_net_open[h] = 0;
    return e->net_close((int)h) == 0 ? 0 : -COMPAT_EINVAL;
}

int64_t compat_syscall_dispatch_c(compat_regs *r) {
    if (r == NULL) {
        return -COMPAT_EINVAL;
    }
    /* Win32 呼び出し (PE ゲストのインポートスタブ経由)。 */
    if (r->rax >= 0x1000 && r->rax < 0x1000 + PE_MAX_IMPORTS) {
        return pe_win32_dispatch((uint32_t)(r->rax - 0x1000), r);
    }
    switch (r->rax) {
    case COMPAT_SYS_EXIT:
        compat_exit_pending = 1;
        compat_exit_code = (int)(r->rdi & 0xFF);
        return 0;
    case COMPAT_SYS_WRITE:
        return sys_write(r->rdi, r->rsi, r->rdx);
    case COMPAT_SYS_READ:
        return sys_read(r->rdi, r->rsi, r->rdx);
    case COMPAT_SYS_OPEN:
        return sys_open(r->rdi, r->rsi);
    case COMPAT_SYS_CLOSE:
        return sys_close(r->rdi);
    case COMPAT_SYS_FSTAT:
        return sys_fstat(r->rdi, r->rsi);
    case COMPAT_SYS_BRK:
        return sys_brk(r->rdi);
    case COMPAT_SYS_GETPID:
        return 1;
    case COMPAT_SYS_LSEEK:
        return sys_lseek(r->rdi, r->rsi, r->rdx);
    case COMPAT_SYS_FB_INFO:
        return sys_fb_info(r->rdi);
    case COMPAT_SYS_FB_FILL:
        return sys_fb_fill(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case COMPAT_SYS_FB_BLIT:
        return sys_fb_blit(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case COMPAT_SYS_FB_TEXT:
        return sys_fb_text(r->rdi, r->rsi, r->rdx, r->r10);
    case COMPAT_SYS_FB_FLUSH:
        return sys_fb_flush();
    case COMPAT_SYS_NET_RESOLVE:
        return sys_net_resolve(r->rdi, r->rsi);
    case COMPAT_SYS_NET_CONNECT:
        return sys_net_connect(r->rdi, r->rsi);
    case COMPAT_SYS_NET_SEND:
        return sys_net_send(r->rdi, r->rsi, r->rdx);
    case COMPAT_SYS_NET_RECV:
        return sys_net_recv(r->rdi, r->rsi, r->rdx);
    case COMPAT_SYS_NET_CLOSE:
        return sys_net_close(r->rdi);
    default:
        if (g_io != NULL && g_io->log_unsupported != NULL) {
            g_io->log_unsupported(r->rax);
        }
        return -COMPAT_ENOSYS;
    }
}
