/* PrismOS — PE パーサーと Win32 コンソールサブセット。 */
#include "pe.h"

#include "string.h"
#include "vfs.h"

/* ---- 小さな読み取りヘルパー (範囲検査つき) ---- */

static int rd16(const uint8_t *d, size_t len, size_t off,
                uint16_t *out) {
    if (off + 2 > len) {
        return -1;
    }
    *out = (uint16_t)(d[off] | (d[off + 1] << 8));
    return 0;
}

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

/* RVA の指す文字列をバッファへ写す。 */
static int copy_cstr(const pe_info *info, const uint8_t *data,
                     size_t len, uint32_t rva, char *out,
                     size_t cap) {
    uint32_t off;
    if (pe_rva_to_offset(info, data, len, rva, &off) != 0) {
        return -1;
    }
    size_t i = 0;
    while (i + 1 < cap) {
        if (off + i >= len) {
            return -1;
        }
        out[i] = (char)data[off + i];
        if (out[i] == '\0') {
            return 0;
        }
        i++;
    }
    return -1;
}

int pe_rva_to_offset(const pe_info *info, const uint8_t *data,
                     size_t len, uint32_t rva, uint32_t *out_off) {
    (void)data;
    (void)len;
    /* ヘッダ領域 (SizeOfHeaders の内側) はそのまま。 */
    for (int i = 0; i < info->section_count; i++) {
        const pe_section *s = &info->sections[i];
        uint32_t span = s->raw_size;
        if (rva >= s->vaddr && rva - s->vaddr < span) {
            *out_off = s->raw_ptr + (rva - s->vaddr);
            return 0;
        }
    }
    if (rva < 0x1000) {
        *out_off = rva; /* ヘッダはファイル先頭に同じ配置 */
        return 0;
    }
    return -1;
}

const char *pe_error(int rc) {
    switch (rc) {
    case -1:
        return "not a PE file (bad signature)";
    case -2:
        return "not a 64-bit PE (machine is not x86-64)";
    case -3:
        return "truncated PE file";
    case -4:
        return "too many sections or imports";
    case -5:
        return "bad PE layout (entry or size out of range)";
    default:
        return "unknown PE error";
    }
}

int pe_parse(const uint8_t *data, size_t len, pe_info *out) {
    if (data == NULL || out == NULL || len < 0x40) {
        return -3;
    }
    memset(out, 0, sizeof(*out));
    if (data[0] != 'M' || data[1] != 'Z') {
        return -1;
    }
    uint32_t nt_off;
    if (rd32(data, len, 0x3C, &nt_off) != 0 || nt_off + 24 > len) {
        return -3;
    }
    uint32_t sig;
    if (rd32(data, len, nt_off, &sig) != 0 || sig != 0x00004550u) {
        return -1;
    }
    uint16_t machine, nsec, opt_size;
    if (rd16(data, len, nt_off + 4, &machine) != 0 ||
        rd16(data, len, nt_off + 6, &nsec) != 0 ||
        rd16(data, len, nt_off + 20, &opt_size) != 0) {
        return -3;
    }
    if (machine != 0x8664) {
        return -2;
    }
    if (nsec > PE_MAX_SECTIONS) {
        return -4;
    }
    size_t opt = nt_off + 24;
    if (opt + opt_size > len || opt_size < 112) {
        return -3;
    }
    uint16_t magic;
    if (rd16(data, len, opt, &magic) != 0 || magic != 0x20B) {
        return -2; /* PE32 (32bit) は対象外 */
    }
    uint32_t entry, size_image, n_rva;
    uint64_t image_base;
    uint32_t dir_import_rva = 0, dir_reloc_rva = 0,
             dir_reloc_size = 0;
    if (rd32(data, len, opt + 16, &entry) != 0 ||
        rd64(data, len, opt + 24, &image_base) != 0 ||
        rd32(data, len, opt + 56, &size_image) != 0 ||
        rd32(data, len, opt + 60, &out->size_of_headers) != 0 ||
        rd16(data, len, opt + 68, &out->subsystem) != 0 ||
        rd32(data, len, opt + 108, &n_rva) != 0) {
        return -3;
    }
    if (n_rva > 1 && opt + 112 + 16 <= opt + opt_size) {
        rd32(data, len, opt + 112 + 8, &dir_import_rva);
    }
    if (n_rva > 5 && opt + 112 + 48 <= opt + opt_size) {
        rd32(data, len, opt + 112 + 40, &dir_reloc_rva);
        rd32(data, len, opt + 112 + 44, &dir_reloc_size);
    }
    if (size_image == 0 || size_image > 256u * 1024 * 1024 ||
        entry >= size_image) {
        return -5;
    }
    out->image_base = image_base;
    out->entry_rva = entry;
    out->size_of_image = size_image;
    out->reloc_rva = dir_reloc_rva;
    out->reloc_size = dir_reloc_size;
    out->section_count = nsec;
    size_t sec_off = opt + opt_size;
    for (int i = 0; i < nsec; i++) {
        size_t so = sec_off + (size_t)i * 40;
        if (so + 40 > len) {
            return -3;
        }
        pe_section *s = &out->sections[i];
        memcpy(s->name, data + so, 8);
        s->name[8] = '\0';
        rd32(data, len, so + 8, &s->vsize);
        rd32(data, len, so + 12, &s->vaddr);
        rd32(data, len, so + 16, &s->raw_size);
        rd32(data, len, so + 20, &s->raw_ptr);
        rd32(data, len, so + 36, &s->chars);
        if (s->raw_ptr + s->raw_size > len && s->raw_size > 0) {
            return -3;
        }
    }
    /* インポート: ディスクリプタを歩く。 */
    out->import_count = 0;
    if (dir_import_rva != 0) {
        uint32_t desc_off;
        if (pe_rva_to_offset(out, data, len, dir_import_rva,
                             &desc_off) != 0) {
            return -5;
        }
        for (int d = 0; d < 16; d++) {
            size_t dof = desc_off + (size_t)d * 20;
            uint32_t oft, name_rva, ft;
            if (rd32(data, len, dof, &oft) != 0 ||
                rd32(data, len, dof + 12, &name_rva) != 0 ||
                rd32(data, len, dof + 16, &ft) != 0) {
                return -3;
            }
            if (oft == 0 && name_rva == 0 && ft == 0) {
                break; /* 終端 */
            }
            char dll[PE_DLL_NAME_LEN];
            if (copy_cstr(out, data, len, name_rva, dll,
                          sizeof(dll)) != 0) {
                return -5;
            }
            uint32_t thunk_rva = (oft != 0) ? oft : ft;
            for (int t = 0; t < 256; t++) {
                uint32_t t_off;
                if (pe_rva_to_offset(out, data, len,
                                     thunk_rva + (uint32_t)t * 8,
                                     &t_off) != 0) {
                    return -5;
                }
                uint64_t val;
                if (rd64(data, len, t_off, &val) != 0) {
                    return -3;
                }
                if (val == 0) {
                    break;
                }
                if (out->import_count >= PE_MAX_IMPORTS) {
                    return -4;
                }
                pe_import *imp =
                    &out->imports[out->import_count];
                memset(imp, 0, sizeof(*imp));
                memcpy(imp->dll, dll, sizeof(imp->dll));
                imp->iat_rva = ft + (uint32_t)t * 8;
                if ((val >> 63) != 0) {
                    imp->by_ordinal = 1;
                    imp->ordinal = (uint16_t)(val & 0xFFFF);
                } else {
                    /* hint (2 バイト) の後に名前。 */
                    if (copy_cstr(out, data, len,
                                  (uint32_t)val + 2, imp->func,
                                  sizeof(imp->func)) != 0) {
                        return -5;
                    }
                }
                out->import_count++;
            }
        }
    }
    return 0;
}

/* ---- 対応する Win32 API (KERNEL32.dll) ---- */

static const char *const PE_SUPPORTED_APIS[] = {
    "ExitProcess",    "TerminateProcess", "GetStdHandle",
    "WriteFile",      "ReadFile",         "WriteConsoleA",
    "ReadConsoleA",   "CreateFileA",      "CloseHandle",
    "FlushFileBuffers", "GetCommandLineA", "GetProcessHeap",
    "HeapAlloc",      "HeapFree",         "VirtualAlloc",
    "VirtualFree",    "Sleep",            "GetTickCount",
    "GetTickCount64", "GetLastError",     "SetConsoleTitleA",
    "GetCurrentProcessId", "IsDebuggerPresent",
};

int pe_import_supported(const pe_import *imp) {
    if (imp == NULL || imp->by_ordinal) {
        return 0;
    }
    /* DLL 名は大文字小文字を区別しない。 */
    const char *want = "KERNEL32.DLL";
    size_t i = 0;
    while (want[i] != '\0') {
        char c = imp->dll[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != want[i]) {
            return 0;
        }
        i++;
    }
    if (imp->dll[i] != '\0') {
        return 0;
    }
    for (size_t k = 0;
         k < sizeof(PE_SUPPORTED_APIS) / sizeof(PE_SUPPORTED_APIS[0]);
         k++) {
        if (strcmp(imp->func, PE_SUPPORTED_APIS[k]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---- Win32 呼び出しの実装 ---- */

static const pe_info *g_pe;
static pe_io g_pe_io;
static compat_image *g_pe_img;
static uint32_t g_last_error;
static uint64_t g_heap_cur; /* brk 領域の切り出し位置 */

#define PE_MAX_FILES 4
#define PE_HANDLE_BASE 0x100

typedef struct {
    int used;
    int write_mode;
    char name[32];
    const char *data; /* 読み用: VFS の中身 (借用) */
    size_t size;
    size_t off;
    char wbuf[4096]; /* 書き用: 閉じる時に VFS へ出す */
    size_t wlen;
} pe_file;

static pe_file g_pe_files[PE_MAX_FILES];

void pe_set_context(const pe_info *info, const pe_io *io,
                    compat_image *img) {
    g_pe = info;
    if (io != NULL) {
        g_pe_io = *io;
    } else {
        memset(&g_pe_io, 0, sizeof(g_pe_io));
    }
    g_pe_img = img;
    g_last_error = 0;
    g_heap_cur = (img != NULL) ? (uint64_t)(uintptr_t)img->brk_base
                               : 0;
    for (int i = 0; i < PE_MAX_FILES; i++) {
        g_pe_files[i].used = 0;
    }
}

void pe_clear_context(void) {
    g_pe = NULL;
    g_pe_img = NULL;
}

/* ゲストスタックのアドレス (compat_gate.S の保存並び:
 * 汎用レジスタ 15 個 + RIP/CS/RFLAGS の次が RSP)。 */
static uint64_t regs_guest_rsp(const compat_regs *r) {
    const uint64_t *q = (const uint64_t *)r;
    return q[18];
}

/* 5 番目以降の引数 (shadow space の後) を読む。 */
static uint64_t stack_arg(const compat_regs *r, int idx) {
    uint64_t rsp = regs_guest_rsp(r);
    uint64_t addr = rsp + 40 + (uint64_t)(idx - 5) * 8;
    if (!compat_ptr_ok(addr, 8)) {
        return 0;
    }
    return *(const uint64_t *)(uintptr_t)addr;
}

/* ゲストの文字列長 (終端込みで上限まで)。 */
static size_t guest_strlen(uint64_t ptr, size_t max) {
    size_t n = 0;
    while (n < max && compat_ptr_ok(ptr + n, 1) &&
           *(const char *)(uintptr_t)(ptr + n) != '\0') {
        n++;
    }
    return n;
}

static int guest_copy_in_str(uint64_t ptr, char *out, size_t cap) {
    size_t n = guest_strlen(ptr, cap);
    if (n >= cap) {
        return -1;
    }
    for (size_t i = 0; i <= n; i++) {
        out[i] = *(const char *)(uintptr_t)(ptr + i);
    }
    return 0;
}

/* 保護ファイル (compat.c と同じ考え方)。 */
static int pe_path_protected(const char *path) {
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
        return 1;
    }
    return 0;
}

static pe_file *pe_file_of(uint64_t h) {
    if (h < PE_HANDLE_BASE || h >= PE_HANDLE_BASE + PE_MAX_FILES) {
        return NULL;
    }
    pe_file *f = &g_pe_files[h - PE_HANDLE_BASE];
    return f->used ? f : NULL;
}

static void write_out(const char *data, size_t len) {
    if (g_pe_io.io != NULL && g_pe_io.io->write_out != NULL) {
        g_pe_io.io->write_out(data, len);
    }
}

static int64_t win_write_file(uint64_t h, uint64_t buf, uint64_t n,
                              uint64_t written_ptr) {
    if (written_ptr != 0 && !compat_ptr_ok(written_ptr, 4)) {
        return 0;
    }
    uint32_t written = 0;
    if (h == 1 || h == 2) {
        if (!compat_ptr_ok(buf, (size_t)n)) {
            g_last_error = 998; /* ERROR_NOACCESS 相当 */
            return 0;
        }
        write_out((const char *)(uintptr_t)buf, (size_t)n);
        written = (uint32_t)n;
    } else {
        pe_file *f = pe_file_of(h);
        if (f == NULL || !f->write_mode) {
            g_last_error = 6; /* ERROR_INVALID_HANDLE */
            return 0;
        }
        if (!compat_ptr_ok(buf, (size_t)n)) {
            g_last_error = 998;
            return 0;
        }
        const uint8_t *p = (const uint8_t *)(uintptr_t)buf;
        for (uint64_t i = 0; i < n && f->wlen < sizeof(f->wbuf);
             i++) {
            f->wbuf[f->wlen++] = (char)p[i];
        }
        written = (uint32_t)n;
    }
    if (written_ptr != 0) {
        *(uint32_t *)(uintptr_t)written_ptr = written;
    }
    return 1;
}

static int64_t win_read_file(uint64_t h, uint64_t buf, uint64_t n,
                             uint64_t read_ptr) {
    if (read_ptr != 0 && !compat_ptr_ok(read_ptr, 4)) {
        return 0;
    }
    uint32_t got = 0;
    if (h == 0) {
        if (g_pe_io.io == NULL || g_pe_io.io->read_in == NULL) {
            return 0;
        }
        if (!compat_ptr_ok(buf, (size_t)n)) {
            g_last_error = 998;
            return 0;
        }
        int r = g_pe_io.io->read_in((char *)(uintptr_t)buf,
                                    (size_t)n);
        got = (r > 0) ? (uint32_t)r : 0;
    } else {
        pe_file *f = pe_file_of(h);
        if (f == NULL || f->write_mode) {
            g_last_error = 6;
            return 0;
        }
        if (!compat_ptr_ok(buf, (size_t)n)) {
            g_last_error = 998;
            return 0;
        }
        size_t avail = (f->off < f->size) ? f->size - f->off : 0;
        size_t take = (size_t)n < avail ? (size_t)n : avail;
        memcpy((void *)(uintptr_t)buf, f->data + f->off, take);
        f->off += take;
        got = (uint32_t)take;
    }
    if (read_ptr != 0) {
        *(uint32_t *)(uintptr_t)read_ptr = got;
    }
    return 1;
}

static int64_t win_create_file(compat_regs *r) {
    char name[64];
    if (guest_copy_in_str(r->rcx, name, sizeof(name)) != 0) {
        g_last_error = 123; /* ERROR_INVALID_NAME 相当 */
        return -1;
    }
    uint32_t access = (uint32_t)r->rdx;
    uint32_t disposition = (uint32_t)stack_arg(r, 5);
    (void)disposition;
    int want_write = (access & 0x40000000u) != 0; /* GENERIC_WRITE */
    int want_read = (access & 0x80000000u) != 0;  /* GENERIC_READ */
    if (pe_path_protected(name)) {
        g_last_error = 5; /* ERROR_ACCESS_DENIED */
        return -1;
    }
    for (int i = 0; i < PE_MAX_FILES; i++) {
        pe_file *f = &g_pe_files[i];
        if (f->used) {
            continue;
        }
        memset(f, 0, sizeof(*f));
        f->used = 1;
        size_t nl = strlen(name);
        if (nl + 1 > sizeof(f->name)) {
            nl = sizeof(f->name) - 1;
        }
        memcpy(f->name, name, nl);
        f->name[nl] = '\0';
        if (want_write && !want_read) {
            f->write_mode = 1;
            return (int64_t)(PE_HANDLE_BASE + i);
        }
        size_t len = 0;
        const char *data = vfs_read_ptr(name, &len);
        if (data == NULL) {
            f->used = 0;
            g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
            return -1;
        }
        f->data = data;
        f->size = len;
        return (int64_t)(PE_HANDLE_BASE + i);
    }
    g_last_error = 4; /* ERROR_TOO_MANY_OPEN_FILES */
    return -1;
}

static int64_t win_close_handle(uint64_t h) {
    if (h <= 2) {
        return 1; /* 標準ハンドルは閉じない */
    }
    pe_file *f = pe_file_of(h);
    if (f == NULL) {
        g_last_error = 6;
        return 0;
    }
    if (f->write_mode) {
        vfs_write(f->name, f->wbuf, f->wlen);
    }
    f->used = 0;
    return 1;
}

static uint64_t heap_alloc(uint64_t bytes) {
    if (g_pe_img == NULL || g_heap_cur == 0) {
        return 0;
    }
    uint64_t base = (uint64_t)(uintptr_t)g_pe_img->brk_base;
    uint64_t limit = base + COMPAT_BRK_SIZE;
    uint64_t at = (g_heap_cur + 15) & ~15ULL;
    if (bytes > limit || at + bytes > limit || at < base) {
        return 0;
    }
    g_heap_cur = at + bytes;
    return at;
}

int64_t pe_win32_dispatch(uint32_t index, compat_regs *r) {
    if (g_pe == NULL || index >= (uint32_t)g_pe->import_count) {
        return -COMPAT_ENOSYS;
    }
    const char *fn = g_pe->imports[index].func;
    if (strcmp(fn, "ExitProcess") == 0 ||
        strcmp(fn, "TerminateProcess") == 0) {
        uint64_t code = (fn[0] == 'E') ? r->rcx : r->rdx;
        compat_exit_pending = 1;
        compat_exit_code = (int)(code & 0x7FFFFFFF);
        return 0;
    }
    if (strcmp(fn, "GetStdHandle") == 0) {
        int32_t which = (int32_t)(uint32_t)r->rcx;
        if (which == -10) {
            return 0;
        }
        if (which == -11) {
            return 1;
        }
        if (which == -12) {
            return 2;
        }
        return -1;
    }
    if (strcmp(fn, "WriteFile") == 0 ||
        strcmp(fn, "WriteConsoleA") == 0) {
        return win_write_file(r->rcx, r->rdx, r->r8, r->r9);
    }
    if (strcmp(fn, "ReadFile") == 0 ||
        strcmp(fn, "ReadConsoleA") == 0) {
        return win_read_file(r->rcx, r->rdx, r->r8, r->r9);
    }
    if (strcmp(fn, "CreateFileA") == 0) {
        return win_create_file(r);
    }
    if (strcmp(fn, "CloseHandle") == 0) {
        return win_close_handle(r->rcx);
    }
    if (strcmp(fn, "FlushFileBuffers") == 0) {
        pe_file *f = pe_file_of(r->rcx);
        if (f != NULL && f->write_mode) {
            vfs_write(f->name, f->wbuf, f->wlen);
        }
        return 1;
    }
    if (strcmp(fn, "GetCommandLineA") == 0) {
        return (int64_t)g_pe_io.cmdline_va;
    }
    if (strcmp(fn, "GetProcessHeap") == 0) {
        return 0x10000;
    }
    if (strcmp(fn, "HeapAlloc") == 0) {
        return (int64_t)heap_alloc(r->r8);
    }
    if (strcmp(fn, "HeapFree") == 0) {
        return 1; /* 解放は no-op (ヘッダの正直な制限) */
    }
    if (strcmp(fn, "VirtualAlloc") == 0) {
        if (r->rcx != 0) {
            return 0; /* 場所の指定は未対応 */
        }
        return (int64_t)heap_alloc(r->rdx);
    }
    if (strcmp(fn, "VirtualFree") == 0) {
        return 1; /* no-op (同上) */
    }
    if (strcmp(fn, "Sleep") == 0) {
        if (g_pe_io.sleep_ms != NULL) {
            g_pe_io.sleep_ms((uint32_t)r->rcx);
        }
        return 0;
    }
    if (strcmp(fn, "GetTickCount") == 0) {
        return (g_pe_io.tick_ms != NULL) ? g_pe_io.tick_ms() : 0;
    }
    if (strcmp(fn, "GetTickCount64") == 0) {
        return (g_pe_io.tick_ms != NULL) ? g_pe_io.tick_ms() : 0;
    }
    if (strcmp(fn, "GetLastError") == 0) {
        return g_last_error;
    }
    if (strcmp(fn, "SetConsoleTitleA") == 0) {
        return 1; /* 見出しは表示先が無いので受け取るだけ */
    }
    if (strcmp(fn, "GetCurrentProcessId") == 0) {
        return 1;
    }
    if (strcmp(fn, "IsDebuggerPresent") == 0) {
        return 0;
    }
    return -COMPAT_ENOSYS;
}
