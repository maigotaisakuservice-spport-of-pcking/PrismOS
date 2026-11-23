/* PrismOS — PE ローダーと Win32 サブセットのホストテスト。
 *
 * 合成した PE (hello-pe.exe と同じ配置) を pe_parse に
 * 読ませ、インポート表と対応判定を確かめる。振り分けは
 * 偽の文脈 (出力バッファ + アリーナ) で GetStdHandle /
 * WriteFile / CreateFileA / ReadFile / CloseHandle /
 * HeapAlloc / ExitProcess を確かめる。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "pe.h"
#include "vfs.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

/* ---- 合成 PE (build-pe-guest.py と同じ配置) ---- */

static void put16(uint8_t *d, size_t off, uint16_t v) {
    d[off] = (uint8_t)v;
    d[off + 1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *d, size_t off, uint32_t v) {
    d[off] = (uint8_t)v;
    d[off + 1] = (uint8_t)(v >> 8);
    d[off + 2] = (uint8_t)(v >> 16);
    d[off + 3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *d, size_t off, uint64_t v) {
    put32(d, off, (uint32_t)v);
    put32(d, off + 4, (uint32_t)(v >> 32));
}

static const char *const NAMES[6] = {
    "GetStdHandle", "WriteFile",   "ExitProcess",
    "CreateFileA",  "ReadFile",    "CloseHandle",
};

static size_t build_pe(uint8_t *d, int with_bad_import) {
    memset(d, 0, 2048);
    d[0] = 'M';
    d[1] = 'Z';
    put32(d, 0x3C, 0x80);
    size_t nt = 0x80;
    memcpy(d + nt, "PE\0\0", 4);
    put16(d, nt + 4, 0x8664);  /* machine */
    put16(d, nt + 6, 2);       /* sections */
    put16(d, nt + 20, 240);    /* optional size */
    size_t opt = nt + 24;
    put16(d, opt, 0x20B);
    put32(d, opt + 16, 0x1000);                /* entry */
    put64(d, opt + 24, 0x140000000ULL);        /* image base */
    put32(d, opt + 32, 0x1000);
    put32(d, opt + 36, 0x200);
    put32(d, opt + 56, 0x3000);                /* size of image */
    put32(d, opt + 60, 0x200);                 /* size of headers */
    put16(d, opt + 68, 3);                     /* console */
    put32(d, opt + 108, 16);
    put32(d, opt + 112 + 8, 0x2000);           /* import dir */
    put32(d, opt + 112 + 12, 40);
    size_t sec = opt + 240;
    memcpy(d + sec, ".text", 5);
    put32(d, sec + 8, 0x10);
    put32(d, sec + 12, 0x1000);
    put32(d, sec + 16, 0x200);
    put32(d, sec + 20, 0x200);
    put32(d, sec + 36, 0x60000020);
    memcpy(d + sec + 40, ".idata", 6);
    put32(d, sec + 40 + 8, 0x220);
    put32(d, sec + 40 + 12, 0x2000);
    put32(d, sec + 40 + 16, 0x400);
    put32(d, sec + 40 + 20, 0x400);
    put32(d, sec + 40 + 36, 0xC0000040);
    /* .idata の中身 (ファイルオフセット 0x400 = RVA 0x2000) */
    size_t id = 0x400;
    put32(d, id + 0, 0x2040);      /* ILT */
    put32(d, id + 12, 0x21A0);     /* DLL 名 */
    put32(d, id + 16, 0x2080);     /* IAT */
    for (int i = 0; i < 6; i++) {
        uint32_t name_rva = 0x20C0 + (uint32_t)i * 0x20;
        put64(d, id + 0x40 + (size_t)i * 8, name_rva);
        put64(d, id + 0x80 + (size_t)i * 8, name_rva);
        const char *nm = (with_bad_import && i == 5)
                             ? "MessageBoxA"
                             : NAMES[i];
        memcpy(d + id + 0xC0 + (size_t)i * 0x20 + 2, nm,
               strlen(nm) + 1);
    }
    memcpy(d + id + 0x1A0, "KERNEL32.dll", 13);
    return 2048;
}

/* ---- 偽の文脈 ---- */

static char g_out[4096];
static size_t g_out_len;

static void fake_write_out(const char *data, size_t len) {
    if (g_out_len + len < sizeof(g_out)) {
        memcpy(g_out + g_out_len, data, len);
        g_out_len += len;
    }
    g_out[g_out_len] = '\0';
}

static int fake_read_in(char *buf, size_t len) {
    (void)buf;
    (void)len;
    return 0;
}

static void fake_log(uint64_t nr) {
    (void)nr;
}

static const compat_io g_io = {fake_write_out, fake_read_in,
                               fake_log, NULL};

/* ゲストのメモリに見立てるアリーナ。 */
static uint8_t arena[64 * 1024];

static compat_regs *frame_regs(uint64_t frame[24], uint64_t nr) {
    memset(frame, 0, 24 * 8);
    compat_regs *r = (compat_regs *)frame;
    r->rax = nr;
    /* ゲートの保存並び: [18] がゲスト RSP。スタック引数の
     * 置き場 (アリーナ内) を指しておく。 */
    frame[18] = (uint64_t)(uintptr_t)(arena + 32768);
    return r;
}

int main(void) {
    static uint8_t pe[2048];
    size_t len = build_pe(pe, 0);
    pe_info info;
    check(pe_parse(pe, len, &info) == 0, "synthetic PE parses");
    check(info.image_base == 0x140000000ULL, "image base read");
    check(info.entry_rva == 0x1000, "entry RVA read");
    check(info.subsystem == 3, "console subsystem read");
    check(info.section_count == 2, "two sections read");
    check(strcmp(info.sections[0].name, ".text") == 0,
          "section 0 is .text");
    check(info.import_count == 6, "six imports read");
    check(strcmp(info.imports[0].dll, "KERNEL32.dll") == 0,
          "import dll name read");
    check(strcmp(info.imports[1].func, "WriteFile") == 0,
          "import 1 is WriteFile");
    check(info.imports[3].iat_rva == 0x2098,
          "import 3 IAT RVA computed");
    for (int i = 0; i < 6; i++) {
        check(pe_import_supported(&info.imports[i]),
              "supported import recognised");
    }
    /* 対応外のインポートは弾く。 */
    static uint8_t pe2[2048];
    build_pe(pe2, 1);
    pe_info info2;
    check(pe_parse(pe2, len, &info2) == 0, "bad-import PE parses");
    check(!pe_import_supported(&info2.imports[5]),
          "MessageBoxA is not supported");
    /* 壊れた入力。 */
    pe[0] = 'X';
    check(pe_parse(pe, len, &info) == -1, "bad MZ rejected");
    pe[0] = 'M';
    check(pe_parse(pe, 100, &info) == -3, "truncated PE rejected");

    /* ---- 振り分け ---- */
    len = build_pe(pe, 0);
    check(pe_parse(pe, len, &info) == 0, "PE re-parsed for dispatch");
    vfs_set_allocator(malloc, free);
    vfs_init();
    compat_image img;
    memset(&img, 0, sizeof(img));
    img.seg_count = 1;
    img.seg_base[0] = arena;
    img.seg_vaddr[0] = (uint64_t)(uintptr_t)arena;
    img.seg_memsz[0] = sizeof(arena);
    img.brk_base = arena + 16384;
    img.brk_cur = (uint64_t)(uintptr_t)img.brk_base;
    compat_set_ranges(&img, arena + 32768, 4096);
    pe_io pio;
    memset(&pio, 0, sizeof(pio));
    pio.io = &g_io;
    pio.cmdline_va = (uint64_t)(uintptr_t)(arena + 40960);
    pe_set_context(&info, &pio, &img);
    compat_reset_state();
    g_out_len = 0;
    g_out[0] = '\0';

    uint64_t frame[24];
    /* GetStdHandle(-11) -> 1 */
    compat_regs *r = frame_regs(frame, 0x1000 + 0);
    r->rcx = (uint64_t)-11;
    check(pe_win32_dispatch(0, r) == 1, "GetStdHandle(-11) is 1");
    /* WriteFile(1, "AB", 2, &written, NULL) */
    memcpy(arena + 1024, "AB", 2);
    r = frame_regs(frame, 0x1000 + 1);
    r->rcx = 1;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1024);
    r->r8 = 2;
    r->r9 = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(1, r) == 1, "WriteFile returns TRUE");
    check(strcmp(g_out, "AB") == 0, "WriteFile output captured");
    check(*(uint32_t *)(arena + 1040) == 2,
          "WriteFile wrote the byte count");
    /* CreateFileA("about.txt", GENERIC_READ, ..., OPEN_EXISTING) */
    memcpy(arena + 2048, "about.txt", 10);
    uint64_t *sargs = (uint64_t *)(uintptr_t)(arena + 32768);
    sargs[5] = 3; /* [rsp+40] = 第 5 引数 */
    sargs[6] = 0;
    sargs[7] = 0;
    r = frame_regs(frame, 0x1000 + 3);
    r->rcx = (uint64_t)(uintptr_t)(arena + 2048);
    r->rdx = 0x80000000ULL;
    int64_t h = pe_win32_dispatch(3, r);
    check(h >= 0x100, "CreateFileA returns a handle");
    /* ReadFile(h, buf, 16, &n, NULL) -> "PrismOS is built" */
    r = frame_regs(frame, 0x1000 + 4);
    r->rcx = (uint64_t)h;
    r->rdx = (uint64_t)(uintptr_t)(arena + 3072);
    r->r8 = 16;
    r->r9 = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(4, r) == 1, "ReadFile returns TRUE");
    check(memcmp(arena + 3072, "PrismOS is built", 16) == 0,
          "ReadFile got about.txt's head");
    check(*(uint32_t *)(arena + 1040) == 16,
          "ReadFile wrote the byte count");
    /* CloseHandle(h) */
    r = frame_regs(frame, 0x1000 + 5);
    r->rcx = (uint64_t)h;
    check(pe_win32_dispatch(5, r) == 1, "CloseHandle returns TRUE");
    /* HeapAlloc: brk から切り出され、2 回目は後ろに進む。 */
    pe_info hinfo = info;
    hinfo.import_count = 7;
    memset(hinfo.imports[6].dll, 0, sizeof(hinfo.imports[6].dll));
    memcpy(hinfo.imports[6].dll, "KERNEL32.dll", 13);
    memset(hinfo.imports[6].func, 0,
           sizeof(hinfo.imports[6].func));
    memcpy(hinfo.imports[6].func, "HeapAlloc", 10);
    pe_set_context(&hinfo, &pio, &img);
    r = frame_regs(frame, 0x1000 + 6);
    r->rcx = 0x10000;
    r->r8 = 100;
    int64_t p1 = pe_win32_dispatch(6, r);
    check(p1 == (int64_t)(uintptr_t)(arena + 16384),
          "HeapAlloc returns the brk start");
    r = frame_regs(frame, 0x1000 + 6);
    r->rcx = 0x10000;
    r->r8 = 100;
    int64_t p2 = pe_win32_dispatch(6, r);
    check(p2 > p1, "HeapAlloc advances");
    /* ExitProcess(7) */
    pe_set_context(&info, &pio, &img);
    compat_reset_state();
    r = frame_regs(frame, 0x1000 + 2);
    r->rcx = 7;
    check(pe_win32_dispatch(2, r) == 0, "ExitProcess returns 0");
    check(compat_exit_pending == 1 && compat_exit_code == 7,
          "ExitProcess sets the exit state");

    pe_clear_context();
    compat_clear_ranges();
    printf("pe tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
