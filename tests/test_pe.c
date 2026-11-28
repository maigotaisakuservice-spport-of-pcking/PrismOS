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

static uint64_t fake_unix_time(void) {
    /* 2023-11-14 22:13:20 UTC (火曜)。 */
    return 1700000000ULL;
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

/* ---- 拡大した Win32 API の名前表 (先頭 6 件は上の合成 PE と同じ) ---- */

static const char *const XNAMES[] = {
    "GetFileSize",    "SetFilePointer",   "GetFileAttributesA",
    "DeleteFileA",    "CopyFileA",        "MoveFileA",
    "GetCurrentDirectoryA", "SetCurrentDirectoryA",
    "GetEnvironmentVariableA", "SetEnvironmentVariableA",
    "GetModuleFileNameA", "GetModuleHandleA",
    "GetVersion",     "GetSystemTime",    "GetLocalTime",
    "GetConsoleMode", "SetConsoleMode",
    "GetConsoleScreenBufferInfo",
    "GetTempPathA",   "GetFullPathNameA",
    "FindFirstFileA", "FindNextFileA",    "FindClose",
    "SetLastError",   "GetLastError",
    "GetComputerNameA", "GetUserNameA",
    "GetSystemInfo",  "GetStartupInfoA",
    "GetCurrentThreadId",
    "WriteConsoleW",  "ReadConsoleW",
    "GetDiskFreeSpaceA",
    "lstrlenA",       "lstrcpyA",         "lstrcatA",
    "FormatMessageA",
};
#define XN ((int)(sizeof(XNAMES) / sizeof(XNAMES[0])))

static int xidx(const char *name) {
    for (int i = 0; i < XN; i++) {
        if (strcmp(XNAMES[i], name) == 0) {
            return 6 + i;
        }
    }
    return -1;
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

    /* ペンテスト F2 の回帰テスト: ファイル長を超える
     * SizeOfHeaders と、合計が 32 ビットで巻き戻るセクション
     * 範囲を拒否する。 */
    {
        static uint8_t evil[2048];
        size_t elen = build_pe(evil, 0);
        size_t eopt = 0x80 + 24;
        put32(evil, eopt + 60, 0x3000); /* SizeOfHeaders > len */
        check(pe_parse(evil, elen, &info) == -3,
              "oversized SizeOfHeaders rejected");
        elen = build_pe(evil, 0);
        size_t esec = eopt + 240;
        put32(evil, esec + 16, 0x20);        /* raw_size */
        put32(evil, esec + 20, 0xFFFFFFF0u); /* raw_ptr: 合計が巻き戻る */
        check(pe_parse(evil, elen, &info) == -3,
              "wrapping section range rejected");
        elen = build_pe(evil, 0);
        put32(evil, 0x3C, 0xFFFFFFF8u); /* e_lfanew + 24 が巻き戻る */
        check(pe_parse(evil, elen, &info) != 0,
              "wrapping e_lfanew rejected");
        /* 細工していないものは引き続き受理する。 */
        elen = build_pe(evil, 0);
        check(pe_parse(evil, elen, &info) == 0,
              "clean PE still parses after crafted cases");
    }

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

    /* ---- 拡大した Win32 API ---- */
    pe_info xinfo = info;
    for (int i = 0; i < XN; i++) {
        int idx = 6 + i;
        memset(xinfo.imports[idx].dll, 0,
               sizeof(xinfo.imports[idx].dll));
        memcpy(xinfo.imports[idx].dll, "KERNEL32.dll", 13);
        memset(xinfo.imports[idx].func, 0,
               sizeof(xinfo.imports[idx].func));
        memcpy(xinfo.imports[idx].func, XNAMES[i],
               strlen(XNAMES[i]));
    }
    xinfo.import_count = 6 + XN;
    pe_io xpio = pio;
    xpio.image_name = "hello2-pe.exe";
    xpio.unix_time = fake_unix_time;
    pe_set_context(&xinfo, &xpio, &img);
    compat_reset_state();
    /* バッファ: 文字列 1024、結果 2048、構造体 3072、数値 1040。 */
    uint64_t *sargs2 = (uint64_t *)(uintptr_t)(arena + 32768);

    /* CreateFileA("pe2.txt") + WriteFile("0123456789")。
     * 書き込みは CloseHandle で VFS へ反映される。 */
    memcpy(arena + 1024, "pe2.txt", 8);
    sargs2[5] = 2; /* CREATE_ALWAYS */
    sargs2[6] = 0;
    sargs2[7] = 0;
    r = frame_regs(frame, 0x1000 + 3);
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = 0x40000000ULL;
    int64_t h2 = pe_win32_dispatch(3, r);
    check(h2 >= 0x100, "CreateFileA pe2.txt returns a handle");
    memcpy(arena + 1024, "0123456789", 10);
    r = frame_regs(frame, 0x1000 + 1);
    r->rcx = (uint64_t)h2;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1024);
    r->r8 = 10;
    r->r9 = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(1, r) == 1, "WriteFile pe2.txt");
    /* GetFileSize -> 10、高位 0 */
    *(uint32_t *)(arena + 1040) = 0xDEAD;
    r = frame_regs(frame, xidx("GetFileSize"));
    r->rcx = (uint64_t)h2;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(xidx("GetFileSize"), r) == 10,
          "GetFileSize returns 10");
    check(*(uint32_t *)(arena + 1040) == 0,
          "GetFileSize high dword is 0");
    /* 書き込みハンドルでのシーク: BEGIN 5、END 0 -> 10 */
    r = frame_regs(frame, xidx("SetFilePointer"));
    r->rcx = (uint64_t)h2;
    r->rdx = 5;
    r->r8 = 0;
    r->r9 = 0;
    check(pe_win32_dispatch(xidx("SetFilePointer"), r) == 5,
          "SetFilePointer BEGIN 5");
    r = frame_regs(frame, xidx("SetFilePointer"));
    r->rcx = (uint64_t)h2;
    r->rdx = 0;
    r->r8 = 0;
    r->r9 = 2;
    check(pe_win32_dispatch(xidx("SetFilePointer"), r) == 10,
          "SetFilePointer END 0");
    r = frame_regs(frame, 0x1000 + 5);
    r->rcx = (uint64_t)h2;
    check(pe_win32_dispatch(5, r) == 1, "CloseHandle pe2.txt");
    /* 読み直し: GENERIC_READ で開く */
    memcpy(arena + 1024, "pe2.txt", 8);
    sargs2[5] = 3; /* OPEN_EXISTING */
    r = frame_regs(frame, 0x1000 + 3);
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = 0x80000000ULL;
    int64_t h3 = pe_win32_dispatch(3, r);
    check(h3 >= 0x100, "reopen pe2.txt for reading");
    r = frame_regs(frame, xidx("GetFileSize"));
    r->rcx = (uint64_t)h3;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(xidx("GetFileSize"), r) == 10,
          "GetFileSize of reopened file is 10");
    r = frame_regs(frame, xidx("SetFilePointer"));
    r->rcx = (uint64_t)h3;
    r->rdx = 5;
    r->r8 = 0;
    r->r9 = 0;
    check(pe_win32_dispatch(xidx("SetFilePointer"), r) == 5,
          "SetFilePointer BEGIN 5 on read handle");
    r = frame_regs(frame, 0x1000 + 4);
    r->rcx = (uint64_t)h3;
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    r->r8 = 5;
    r->r9 = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(4, r) == 1, "ReadFile after seek");
    check(memcmp(arena + 2048, "56789", 5) == 0,
          "ReadFile got bytes 5..9");
    /* FILE_CURRENT -3 -> 7、FILE_END 0 -> 10 */
    r = frame_regs(frame, xidx("SetFilePointer"));
    r->rcx = (uint64_t)h3;
    r->rdx = (uint64_t)-3;
    r->r8 = 0;
    r->r9 = 1;
    check(pe_win32_dispatch(xidx("SetFilePointer"), r) == 7,
          "SetFilePointer CURRENT -3");
    r = frame_regs(frame, xidx("SetFilePointer"));
    r->rcx = (uint64_t)h3;
    r->rdx = 0;
    r->r8 = 0;
    r->r9 = 2;
    check(pe_win32_dispatch(xidx("SetFilePointer"), r) == 10,
          "SetFilePointer END 0");
    r = frame_regs(frame, 0x1000 + 5);
    r->rcx = (uint64_t)h3;
    check(pe_win32_dispatch(5, r) == 1, "CloseHandle pe2.txt");
    /* GetFileAttributesA */
    memcpy(arena + 1024, "pe2.txt", 8);
    r = frame_regs(frame, xidx("GetFileAttributesA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("GetFileAttributesA"), r) == 0x80,
          "GetFileAttributesA pe2.txt is NORMAL");
    memcpy(arena + 1024, "nope.txt", 9);
    r = frame_regs(frame, xidx("GetFileAttributesA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("GetFileAttributesA"), r) ==
              (int64_t)0xFFFFFFFFu,
          "GetFileAttributesA missing returns -1");
    r = frame_regs(frame, xidx("GetLastError"));
    check(pe_win32_dispatch(xidx("GetLastError"), r) == 2,
          "GetLastError is 2 after missing file");
    /* CopyFileA / MoveFileA / DeleteFileA */
    memcpy(arena + 1024, "pe2.txt", 8);
    memcpy(arena + 2048, "pe2copy.txt", 12);
    r = frame_regs(frame, xidx("CopyFileA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    r->r8 = 0;
    check(pe_win32_dispatch(xidx("CopyFileA"), r) == 1,
          "CopyFileA succeeds");
    r = frame_regs(frame, xidx("CopyFileA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    r->r8 = 1;
    check(pe_win32_dispatch(xidx("CopyFileA"), r) == 0,
          "CopyFileA with fail-if-exists fails");
    r = frame_regs(frame, xidx("GetLastError"));
    check(pe_win32_dispatch(xidx("GetLastError"), r) == 80,
          "GetLastError is 80 after existing copy");
    memcpy(arena + 1024, "pe2copy.txt", 12);
    memcpy(arena + 2048, "pe2moved.txt", 13);
    r = frame_regs(frame, xidx("MoveFileA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    r->r8 = 0;
    check(pe_win32_dispatch(xidx("MoveFileA"), r) == 1,
          "MoveFileA succeeds");
    memcpy(arena + 1024, "pe2copy.txt", 12);
    r = frame_regs(frame, xidx("GetFileAttributesA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("GetFileAttributesA"), r) ==
              (int64_t)0xFFFFFFFFu,
          "moved-away file is gone");
    memcpy(arena + 1024, "pe2moved.txt", 13);
    r = frame_regs(frame, xidx("DeleteFileA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("DeleteFileA"), r) == 1,
          "DeleteFileA succeeds");
    r = frame_regs(frame, xidx("DeleteFileA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("DeleteFileA"), r) == 0,
          "DeleteFileA of missing file fails");
    /* カレントディレクトリ */
    r = frame_regs(frame, xidx("GetCurrentDirectoryA"));
    r->rcx = 64;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("GetCurrentDirectoryA"), r) == 2,
          "GetCurrentDirectoryA returns 2");
    check(strcmp((char *)(arena + 1024), ".\\") == 0,
          "default cwd is .\\");
    memcpy(arena + 1024, "nope", 5);
    r = frame_regs(frame, xidx("SetCurrentDirectoryA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("SetCurrentDirectoryA"), r) == 0,
          "SetCurrentDirectoryA to missing path fails");
    /* 環境変数 */
    memcpy(arena + 1024, "FOO", 4);
    memcpy(arena + 2048, "bar", 4);
    r = frame_regs(frame, xidx("SetEnvironmentVariableA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    check(pe_win32_dispatch(xidx("SetEnvironmentVariableA"), r) == 1,
          "SetEnvironmentVariableA succeeds");
    r = frame_regs(frame, xidx("GetEnvironmentVariableA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    r->r8 = 64;
    check(pe_win32_dispatch(xidx("GetEnvironmentVariableA"), r) == 3,
          "GetEnvironmentVariableA returns 3");
    check(strcmp((char *)(arena + 2048), "bar") == 0,
          "environment value is bar");
    memcpy(arena + 1024, "MISSING", 8);
    r = frame_regs(frame, xidx("GetEnvironmentVariableA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    r->r8 = 64;
    check(pe_win32_dispatch(xidx("GetEnvironmentVariableA"), r) == 0,
          "missing env var returns 0");
    r = frame_regs(frame, xidx("GetLastError"));
    check(pe_win32_dispatch(xidx("GetLastError"), r) == 203,
          "GetLastError is 203 after missing env var");
    memcpy(arena + 1024, "FOO", 4);
    r = frame_regs(frame, xidx("SetEnvironmentVariableA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = 0;
    check(pe_win32_dispatch(xidx("SetEnvironmentVariableA"), r) == 1,
          "SetEnvironmentVariableA NULL deletes");
    /* モジュール名 / バージョン / 時刻 */
    r = frame_regs(frame, xidx("GetModuleFileNameA"));
    r->rcx = 0;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1024);
    r->r8 = 64;
    check(pe_win32_dispatch(xidx("GetModuleFileNameA"), r) == 13,
          "GetModuleFileNameA returns 13");
    check(strcmp((char *)(arena + 1024), "hello2-pe.exe") == 0,
          "module file name is hello2-pe.exe");
    r = frame_regs(frame, xidx("GetModuleHandleA"));
    r->rcx = 0;
    check(pe_win32_dispatch(xidx("GetModuleHandleA"), r) ==
              (int64_t)0x140000000ULL,
          "GetModuleHandleA NULL is the image base");
    memcpy(arena + 1024, "other.dll", 10);
    r = frame_regs(frame, xidx("GetModuleHandleA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("GetModuleHandleA"), r) == 0,
          "GetModuleHandleA of other DLL is NULL");
    r = frame_regs(frame, xidx("GetVersion"));
    int64_t ver = pe_win32_dispatch(xidx("GetVersion"), r);
    check((ver & 0xFF) == 10 && (ver >> 16) == 19045,
          "GetVersion is 10.0 build 19045");
    r = frame_regs(frame, xidx("GetSystemTime"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 3072);
    check(pe_win32_dispatch(xidx("GetSystemTime"), r) == 0,
          "GetSystemTime returns 0");
    check(*(uint16_t *)(arena + 3072) == 2023 &&
              *(uint16_t *)(arena + 3074) == 11 &&
              *(uint16_t *)(arena + 3076) == 2 &&
              *(uint16_t *)(arena + 3078) == 14 &&
              *(uint16_t *)(arena + 3080) == 22 &&
              *(uint16_t *)(arena + 3082) == 13 &&
              *(uint16_t *)(arena + 3084) == 20,
          "GetSystemTime is 2023-11-14 Tue 22:13:20");
    /* コンソール情報 */
    *(uint32_t *)(arena + 1040) = 0;
    r = frame_regs(frame, xidx("GetConsoleMode"));
    r->rcx = 1;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(xidx("GetConsoleMode"), r) == 1,
          "GetConsoleMode returns TRUE");
    check(*(uint32_t *)(arena + 1040) == 7,
          "console mode is 7");
    r = frame_regs(frame, xidx("SetConsoleMode"));
    r->rcx = 1;
    r->rdx = 7;
    check(pe_win32_dispatch(xidx("SetConsoleMode"), r) == 1,
          "SetConsoleMode returns TRUE");
    r = frame_regs(frame, xidx("GetConsoleScreenBufferInfo"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 3072);
    check(pe_win32_dispatch(xidx("GetConsoleScreenBufferInfo"), r) == 1,
          "GetConsoleScreenBufferInfo returns TRUE");
    check(*(uint16_t *)(arena + 3072) == 80 &&
              *(uint16_t *)(arena + 3074) == 25 &&
              *(uint16_t *)(arena + 3080) == 7 &&
              *(uint16_t *)(arena + 3086) == 79 &&
              *(uint16_t *)(arena + 3088) == 24,
          "screen buffer is 80x25, attrs 7, window 79x24");
    /* パス系 */
    r = frame_regs(frame, xidx("GetTempPathA"));
    r->rcx = 64;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("GetTempPathA"), r) == 2,
          "GetTempPathA returns 2");
    check(strcmp((char *)(arena + 1024), ".\\") == 0,
          "temp path is .\\");
    memcpy(arena + 1024, "a.txt", 6);
    r = frame_regs(frame, xidx("GetFullPathNameA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = 64;
    r->r8 = (uint64_t)(uintptr_t)(arena + 2048);
    r->r9 = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(xidx("GetFullPathNameA"), r) == 5,
          "GetFullPathNameA returns 5");
    check(strcmp((char *)(arena + 2048), "a.txt") == 0,
          "full path is a.txt");
    check(*(uint64_t *)(arena + 1040) ==
              (uint64_t)(uintptr_t)(arena + 2048),
          "file part points at the file name");
    /* 検索 */
    memcpy(arena + 1024, "pe2.txt", 8);
    r = frame_regs(frame, xidx("FindFirstFileA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 3072);
    int64_t fh = pe_win32_dispatch(xidx("FindFirstFileA"), r);
    check(fh == 0x200, "FindFirstFileA returns 0x200");
    check(strcmp((char *)(arena + 3072 + 44), "pe2.txt") == 0,
          "find data name is pe2.txt");
    check(*(uint32_t *)(arena + 3072 + 32) == 10,
          "find data size is 10");
    r = frame_regs(frame, xidx("FindNextFileA"));
    r->rcx = (uint64_t)fh;
    r->rdx = (uint64_t)(uintptr_t)(arena + 3072);
    check(pe_win32_dispatch(xidx("FindNextFileA"), r) == 0,
          "FindNextFileA has no more pe2.txt");
    r = frame_regs(frame, xidx("GetLastError"));
    check(pe_win32_dispatch(xidx("GetLastError"), r) == 18,
          "GetLastError is 18 after no more files");
    r = frame_regs(frame, xidx("FindClose"));
    r->rcx = (uint64_t)fh;
    check(pe_win32_dispatch(xidx("FindClose"), r) == 1,
          "FindClose returns TRUE");
    memcpy(arena + 1024, "*.txt", 6);
    r = frame_regs(frame, xidx("FindFirstFileA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 3072);
    int64_t fh2 = pe_win32_dispatch(xidx("FindFirstFileA"), r);
    check(fh2 == 0x200, "FindFirstFileA *.txt returns 0x200");
    r = frame_regs(frame, xidx("FindNextFileA"));
    r->rcx = (uint64_t)fh2;
    r->rdx = (uint64_t)(uintptr_t)(arena + 3072);
    check(pe_win32_dispatch(xidx("FindNextFileA"), r) == 1,
          "FindNextFileA *.txt finds a second file");
    r = frame_regs(frame, xidx("FindClose"));
    r->rcx = (uint64_t)fh2;
    check(pe_win32_dispatch(xidx("FindClose"), r) == 1,
          "FindClose *.txt returns TRUE");
    /* SetLastError / GetLastError */
    r = frame_regs(frame, xidx("SetLastError"));
    r->rcx = 42;
    pe_win32_dispatch(xidx("SetLastError"), r);
    r = frame_regs(frame, xidx("GetLastError"));
    check(pe_win32_dispatch(xidx("GetLastError"), r) == 42,
          "SetLastError 42 then GetLastError is 42");
    /* 計算機名 / ユーザー名 */
    *(uint32_t *)(arena + 1040) = 64;
    r = frame_regs(frame, xidx("GetComputerNameA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(xidx("GetComputerNameA"), r) == 1,
          "GetComputerNameA returns TRUE");
    check(strcmp((char *)(arena + 1024), "PRISM") == 0,
          "computer name is PRISM");
    check(*(uint32_t *)(arena + 1040) == 5,
          "computer name length is 5");
    *(uint32_t *)(arena + 1040) = 64;
    r = frame_regs(frame, xidx("GetUserNameA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(xidx("GetUserNameA"), r) == 1,
          "GetUserNameA returns TRUE");
    check(strcmp((char *)(arena + 1024), "admin") == 0,
          "user name is admin");
    /* システム情報 / 起動情報 */
    r = frame_regs(frame, xidx("GetSystemInfo"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 3072);
    pe_win32_dispatch(xidx("GetSystemInfo"), r);
    check(*(uint16_t *)(arena + 3072) == 9 &&
              *(uint32_t *)(arena + 3076) == 4096 &&
              *(uint32_t *)(arena + 3104) == 1 &&
              *(uint32_t *)(arena + 3108) == 8664 &&
              *(uint32_t *)(arena + 3112) == 65536 &&
              *(uint16_t *)(arena + 3116) == 6,
          "GetSystemInfo fields are sane");
    r = frame_regs(frame, xidx("GetStartupInfoA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 3072);
    pe_win32_dispatch(xidx("GetStartupInfoA"), r);
    check(*(uint32_t *)(arena + 3072) == 96,
          "GetStartupInfoA cb is 96");
    r = frame_regs(frame, xidx("GetCurrentThreadId"));
    check(pe_win32_dispatch(xidx("GetCurrentThreadId"), r) == 1,
          "GetCurrentThreadId is 1");
    /* WriteConsoleW / ReadConsoleW */
    static const uint16_t w_hi[2] = {'H', 'i'};
    memcpy(arena + 1024, w_hi, 4);
    size_t out_before = g_out_len;
    r = frame_regs(frame, xidx("WriteConsoleW"));
    r->rcx = 1;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1024);
    r->r8 = 2;
    r->r9 = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(xidx("WriteConsoleW"), r) == 1,
          "WriteConsoleW returns TRUE");
    check(*(uint32_t *)(arena + 1040) == 2,
          "WriteConsoleW wrote 2 chars");
    check(g_out_len == out_before + 2 &&
              memcmp(g_out + out_before, "Hi", 2) == 0,
          "WriteConsoleW output is Hi");
    r = frame_regs(frame, xidx("ReadConsoleW"));
    r->rcx = 0;
    r->rdx = (uint64_t)(uintptr_t)(arena + 1024);
    r->r8 = 16;
    r->r9 = (uint64_t)(uintptr_t)(arena + 1040);
    check(pe_win32_dispatch(xidx("ReadConsoleW"), r) == 1,
          "ReadConsoleW returns TRUE");
    check(*(uint32_t *)(arena + 1040) == 0,
          "ReadConsoleW reads 0 from the fake input");
    /* GetDiskFreeSpaceA */
    sargs2[5] = (uint64_t)(uintptr_t)(arena + 2064);
    r = frame_regs(frame, xidx("GetDiskFreeSpaceA"));
    r->rcx = 0;
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    r->r8 = (uint64_t)(uintptr_t)(arena + 2052);
    r->r9 = (uint64_t)(uintptr_t)(arena + 2056);
    check(pe_win32_dispatch(xidx("GetDiskFreeSpaceA"), r) == 1,
          "GetDiskFreeSpaceA returns TRUE");
    check(*(uint32_t *)(arena + 2048) == 8 &&
              *(uint32_t *)(arena + 2052) == 512 &&
              *(uint32_t *)(arena + 2056) == 100000 &&
              *(uint32_t *)(arena + 2064) == 200000,
          "disk free space values are sane");
    /* 文字列関数 */
    memcpy(arena + 1024, "hello", 6);
    r = frame_regs(frame, xidx("lstrlenA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    check(pe_win32_dispatch(xidx("lstrlenA"), r) == 5,
          "lstrlenA hello is 5");
    memcpy(arena + 2048, "abc", 4);
    r = frame_regs(frame, xidx("lstrcpyA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    check(pe_win32_dispatch(xidx("lstrcpyA"), r) ==
              (int64_t)(uintptr_t)(arena + 1024),
          "lstrcpyA returns the destination");
    check(strcmp((char *)(arena + 1024), "abc") == 0,
          "lstrcpyA copied abc");
    memcpy(arena + 2048, "de", 3);
    r = frame_regs(frame, xidx("lstrcatA"));
    r->rcx = (uint64_t)(uintptr_t)(arena + 1024);
    r->rdx = (uint64_t)(uintptr_t)(arena + 2048);
    check(pe_win32_dispatch(xidx("lstrcatA"), r) ==
              (int64_t)(uintptr_t)(arena + 1024),
          "lstrcatA returns the destination");
    check(strcmp((char *)(arena + 1024), "abcde") == 0,
          "lstrcatA made abcde");
    /* FormatMessageA */
    sargs2[5] = (uint64_t)(uintptr_t)(arena + 2048);
    sargs2[6] = 64;
    r = frame_regs(frame, xidx("FormatMessageA"));
    r->rcx = 0x1000;
    r->rdx = 0;
    r->r8 = 2;
    r->r9 = 0;
    check(pe_win32_dispatch(xidx("FormatMessageA"), r) == 15,
          "FormatMessageA returns 15");
    check(strcmp((char *)(arena + 2048), "Unknown error 2") == 0,
          "FormatMessageA text is Unknown error 2");

    pe_clear_context();
    compat_clear_ranges();
    printf("pe tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
