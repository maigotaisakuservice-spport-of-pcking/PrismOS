/* PrismOS — 互換レイヤーのホストテスト。
 * ELF の解析・呼び出しの対応表・呼び出しの振り分けを、
 * 本物の実証ゲスト (shell/sample_compat.h) を使って確かめる。
 * シリアルはスタブ (tests/stubs/stub_hw.c) で、出力は
 * フックでバッファに受ける。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "sample_compat.h"
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

/* ---- フック: 出力を受けるバッファと入力のバッファ ---- */

static char g_out[4096];
static size_t g_out_len;
static const char *g_in;
static size_t g_in_pos;
static uint64_t g_last_unsupported;
static int g_unsupported_count;

static void test_write_out(const char *data, size_t len) {
    if (g_out_len + len < sizeof(g_out)) {
        memcpy(g_out + g_out_len, data, len);
        g_out_len += len;
    }
    g_out[g_out_len] = '\0';
}

static int test_read_in(char *buf, size_t len) {
    size_t n = 0;
    while (n < len && g_in != NULL && g_in[g_in_pos] != '\0') {
        buf[n++] = g_in[g_in_pos++];
    }
    return (int)n;
}

static void test_log_unsupported(uint64_t nr) {
    g_last_unsupported = nr;
    g_unsupported_count++;
}

/* 拡張フックの偽物: 描画は記録し、ネットは決まった
 * 応答を返す。ディスパッチャの検証 (切り取り・
 * ハンドル状態・ポインタ検査) が目的。 */
static uint32_t fake_fb[COMPAT_FB_WIDTH * COMPAT_FB_HEIGHT];
static int fake_flushes;
static char fake_sent[128];
static size_t fake_sent_len;
static int fake_net_open;

static int fake_fb_fill(uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h, uint32_t rgb) {
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            fake_fb[(y + row) * COMPAT_FB_WIDTH + (x + col)] = rgb;
        }
    }
    return 0;
}

static int fake_fb_blit(uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h, const uint32_t *pixels) {
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            fake_fb[(y + row) * COMPAT_FB_WIDTH + (x + col)] =
                pixels[row * w + col];
        }
    }
    return 0;
}

static int fake_fb_text(uint32_t x, uint32_t y, const char *s,
                        uint32_t rgb) {
    (void)x;
    (void)y;
    (void)s;
    (void)rgb;
    return 0;
}

static int fake_fb_flush(void) {
    fake_flushes++;
    return 0;
}

static int fake_net_resolve(const char *host, uint8_t ip_out[4]) {
    (void)host;
    ip_out[0] = 10;
    ip_out[1] = 0;
    ip_out[2] = 2;
    ip_out[3] = 2;
    return 0;
}

static int fake_net_connect(const char *host, uint16_t port) {
    (void)host;
    (void)port;
    if (fake_net_open) {
        return -1;
    }
    fake_net_open = 1;
    return 0;
}

static int fake_net_send(int handle, const uint8_t *data,
                         size_t len) {
    (void)handle;
    if (len > sizeof(fake_sent)) {
        return -1;
    }
    memcpy(fake_sent, data, len);
    fake_sent_len = len;
    return (int)len;
}

static int fake_net_recv(int handle, uint8_t *buf, size_t cap) {
    (void)handle;
    const char *reply = "pong!";
    size_t n = 5;
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, reply, n);
    return (int)n;
}

static int fake_net_close(int handle) {
    (void)handle;
    fake_net_open = 0;
    return 0;
}

static const compat_ext_io g_test_ext = {
    fake_fb_fill,  fake_fb_blit,   fake_fb_text,  fake_fb_flush,
    fake_net_resolve, fake_net_connect, fake_net_send,
    fake_net_recv, fake_net_close,
};

static const compat_io g_test_io = {
    test_write_out,
    test_read_in,
    test_log_unsupported,
    &g_test_ext,
};

static const compat_io g_test_io_noext = {
    test_write_out,
    test_read_in,
    test_log_unsupported,
    NULL,
};

static compat_regs make_regs(uint64_t nr, uint64_t a0, uint64_t a1,
                             uint64_t a2) {
    compat_regs r;
    memset(&r, 0, sizeof(r));
    r.rax = nr;
    r.rdi = a0;
    r.rsi = a1;
    r.rdx = a2;
    return r;
}

static compat_regs make_regs5(uint64_t nr, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3, uint64_t a4) {
    compat_regs r = make_regs(nr, a0, a1, a2);
    r.r10 = a3;
    r.r8 = a4;
    return r;
}

int main(void) {
    /* ---- ELF の解析 (本物のゲスト) ---- */
    compat_elf_info info;
    check(compat_elf_parse(compat_guest_hello, compat_guest_hello_len,
                           &info) == 0,
          "parse hello.elf");
    check(info.entry == 0x400000, "hello entry is 0x400000");
    check(info.is_exec == 1, "hello is EXEC");
    check(info.seg_count == 1, "hello has 1 segment");
    check(info.segs[0].filesz == info.segs[0].memsz,
          "hello has no bss");
    check(compat_elf_parse(compat_guest_calc, compat_guest_calc_len,
                           &info) == 0,
          "parse calc.elf");
    check(compat_elf_parse(compat_guest_cat, compat_guest_cat_len,
                           &info) == 0,
          "parse cat.elf");
    check(info.segs[0].memsz > info.segs[0].filesz,
          "cat has bss (memsz > filesz)");
    check(compat_elf_parse(compat_guest_unsup, compat_guest_unsup_len,
                           &info) == 0,
          "parse unsup.elf");

    /* ---- 壊れた ELF ---- */
    check(compat_elf_parse(compat_guest_hello, 10, &info) != 0,
          "too small file rejected");
    {
        uint8_t bad[512];
        memcpy(bad, compat_guest_hello, compat_guest_hello_len);
        bad[1] = 'X'; /* マジックを壊す */
        check(compat_elf_parse(bad, compat_guest_hello_len, &info) != 0,
              "bad magic rejected");
        check(strstr(compat_elf_error(-2), "not an ELF") != NULL,
              "error message for bad magic");
    }
    {
        uint8_t bad[512];
        memcpy(bad, compat_guest_hello, compat_guest_hello_len);
        bad[18] = 0x28; /* 種別を x86-64 (62) から変える */
        bad[19] = 0x00;
        check(compat_elf_parse(bad, compat_guest_hello_len, &info) != 0,
              "wrong type rejected");
    }
    {
        uint8_t bad[512];
        memcpy(bad, compat_guest_hello, compat_guest_hello_len);
        bad[16] = 1; /* type を EXEC (2) から REL (1) に変える */
        bad[17] = 0;
        check(compat_elf_parse(bad, compat_guest_hello_len, &info) != 0,
              "REL type rejected");
    }
    {
        /* maindyn.elf の PT_DYNAMIC の大きさを壊す:
         * 動的セクションとして不正なので、動的リンクの
         * 解析ではっきり断ること。 */
        static uint8_t bad[16384];
        memcpy(bad, compat_guest_maindyn, compat_guest_maindyn_len);
        uint64_t phoff = 0;
        memcpy(&phoff, bad + 32, 8);
        int found = 0;
        for (int i = 0; i < 16 && !found; i++) {
            uint8_t *ph = bad + phoff + (size_t)i * 56;
            uint32_t ptype = 0;
            memcpy(&ptype, ph, 4);
            if (ptype == 2) {
                uint64_t tiny = 8; /* 1 エントリ未満 */
                memcpy(ph + 32, &tiny, 8);
                found = 1;
            }
        }
        check(found == 1, "maindyn has a PT_DYNAMIC header");
        int rc = compat_elf_parse(bad, compat_guest_maindyn_len,
                                  &info);
        check(rc != 0, "malformed PT_DYNAMIC rejected");
        check(strstr(compat_elf_error(rc), "dynamic") != NULL,
              "error message mentions dynamic linking");
    }
    {
        /* 実物の動的ゲスト (maindyn.elf) が解析でき、
         * 依存ライブラリと再配置の場所が取れること。 */
        check(compat_elf_parse(compat_guest_maindyn,
                               compat_guest_maindyn_len, &info) == 0,
              "parse maindyn.elf");
        check(info.dyn.present == 1, "maindyn has a dynamic section");
        check(info.dyn.needed_count == 1, "maindyn needs 1 library");
        check(strcmp(info.dyn.needed[0], "libguest.so") == 0,
              "maindyn needs libguest.so");
        check(info.dyn.sym_count >= 2, "maindyn dynsym count sane");
        check(info.dyn.jmprel_sz == 24, "maindyn has 1 PLT relocation");
        check(info.dyn.rela_sz == 24, "maindyn has 1 RELA relocation");
        check(compat_elf_parse(compat_guest_libguest,
                               compat_guest_libguest_len, &info) == 0,
              "parse libguest.so");
        check(info.dyn.present == 1, "libguest has a dynamic section");
        check(info.dyn.needed_count == 0, "libguest needs nothing");
        check(info.dyn.init_array_sz == 8, "libguest has 1 init entry");
    }
    {
        /* 細工 ELF の回帰: セグメントのアドレス検証。 */
        static uint8_t bad2[16384];
        uint64_t phoff = 0;
        /* 1) カーネル上半分への配置を断る。 */
        memcpy(bad2, compat_guest_maindyn, compat_guest_maindyn_len);
        memcpy(&phoff, bad2 + 32, 8);
        for (int i = 0; i < 16; i++) {
            uint8_t *ph = bad2 + phoff + (size_t)i * 56;
            uint32_t ptype = 0;
            memcpy(&ptype, ph, 4);
            if (ptype == 1) {
                uint64_t kv = 0xFFFFFFFF80000000ULL;
                memcpy(ph + 16, &kv, 8);
                break;
            }
        }
        check(compat_elf_parse(bad2, compat_guest_maindyn_len,
                               &info) == -22,
              "kernel-half segment rejected");
        check(strstr(compat_elf_error(-22), "user space") != NULL,
              "error mentions user space");
        /* 2) セグメント同士の重なりを断る。 */
        memcpy(bad2, compat_guest_maindyn, compat_guest_maindyn_len);
        uint64_t first_v = 0;
        int seen = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t *ph = bad2 + phoff + (size_t)i * 56;
            uint32_t ptype = 0;
            memcpy(&ptype, ph, 4);
            if (ptype != 1) {
                continue;
            }
            if (seen == 0) {
                memcpy(&first_v, ph + 16, 8);
                seen = 1;
            } else {
                memcpy(ph + 16, &first_v, 8);
                break;
            }
        }
        check(compat_elf_parse(bad2, compat_guest_maindyn_len,
                               &info) == -23,
              "overlapping segments rejected");
        /* 3) vaddr + memsz の溢れを断る。 */
        memcpy(bad2, compat_guest_maindyn, compat_guest_maindyn_len);
        for (int i = 0; i < 16; i++) {
            uint8_t *ph = bad2 + phoff + (size_t)i * 56;
            uint32_t ptype = 0;
            memcpy(&ptype, ph, 4);
            if (ptype == 1) {
                uint64_t v = 0x00007FFFFFFFFF00ULL;
                uint64_t msz = 0x100000;
                memcpy(ph + 16, &v, 8);
                memcpy(ph + 40, &msz, 8);
                break;
            }
        }
        check(compat_elf_parse(bad2, compat_guest_maindyn_len,
                               &info) == -24,
              "overflowing segment range rejected");
    }
    {
        /* 入口を載せる範囲の外にずらす。 */
        uint8_t bad[512];
        memcpy(bad, compat_guest_hello, compat_guest_hello_len);
        bad[24] = 0x00;
        bad[25] = 0x00;
        bad[26] = 0x50;
        bad[27] = 0x00;
        check(compat_elf_parse(bad, compat_guest_hello_len, &info) != 0,
              "entry outside segments rejected");
    }

    /* ---- 呼び出しの対応表 ---- */
    check(strcmp(compat_syscall_name(0), "exit") == 0, "call 0 is exit");
    check(strcmp(compat_syscall_name(1), "write") == 0,
          "call 1 is write");
    check(strcmp(compat_syscall_name(2), "read") == 0, "call 2 is read");
    check(strcmp(compat_syscall_name(3), "open") == 0, "call 3 is open");
    check(strcmp(compat_syscall_name(4), "close") == 0,
          "call 4 is close");
    check(strcmp(compat_syscall_name(5), "fstat") == 0,
          "call 5 is fstat");
    check(strcmp(compat_syscall_name(6), "brk") == 0, "call 6 is brk");
    check(strcmp(compat_syscall_name(7), "getpid") == 0,
          "call 7 is getpid");
    check(strcmp(compat_syscall_name(8), "lseek") == 0,
          "call 8 is lseek");
    check(compat_syscall_supported(8) == 1, "lseek is supported");
    check(compat_syscall_name(999) == NULL, "call 999 has no name");
    check(compat_syscall_supported(0) == 1, "exit is supported");
    check(compat_syscall_supported(999) == 0, "999 is not supported");

    /* ---- イメージの載せ降ろし ---- */
    vfs_set_allocator(malloc, free);
    vfs_init();
    check(vfs_file_count() == 51, "51 built-in files");
    compat_image img;
    check(compat_elf_parse(compat_guest_cat, compat_guest_cat_len,
                           &info) == 0,
          "parse cat again for load");
    check(compat_load_image(&info, compat_guest_cat, compat_guest_cat_len,
                            malloc, &img) == 0,
          "load cat image");
    check(img.seg_count == 1, "loaded 1 segment");
    check(img.entry != 0, "loaded entry is set");
    /* 載せた中身は元のファイルと同じ。 */
    check(memcmp(img.seg_base[0], compat_guest_cat + info.segs[0].offset,
                 (size_t)info.segs[0].filesz) == 0,
          "loaded segment matches the file");
    /* bss の部分は 0 で埋まっている。 */
    {
        int zero = 1;
        for (size_t i = (size_t)info.segs[0].filesz;
             i < (size_t)info.segs[0].memsz; i++) {
            if (img.seg_base[0][i] != 0) {
                zero = 0;
                break;
            }
        }
        check(zero, "bss is zeroed");
    }
    check(img.brk_base != NULL, "brk arena allocated");
    check(img.brk_cur == (uint64_t)img.brk_base, "brk starts at base");

    /* ---- 呼び出しの振り分け ---- */
    {
        /* ゲストのメモリに見立てる領域: スタック用のバッファ。 */
        static uint8_t stack[COMPAT_STACK_SIZE];
        compat_set_ranges(&img, stack, sizeof(stack));
        compat_set_context(&g_test_io, &img, malloc, free);
        compat_reset_state();
        g_out_len = 0;
        g_out[0] = '\0';
        g_in = "ab";
        g_in_pos = 0;
        g_unsupported_count = 0;

        /* write(1, "hi", 2) -> 2、出力に "hi" が残る。 */
        const char *msg = "hi";
        memcpy(stack, msg, 2);
        compat_regs r =
            make_regs(COMPAT_SYS_WRITE, 1, (uint64_t)stack, 2);
        check(compat_syscall_dispatch_c(&r) == 2, "write returns 2");
        check(strcmp(g_out, "hi") == 0, "write output captured");

        /* write(0, ...) は EBADF。 */
        r = make_regs(COMPAT_SYS_WRITE, 0, (uint64_t)stack, 2);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EBADF,
              "write to fd 0 is EBADF");

        /* 範囲外のポインタは EFAULT (カーネルを壊さない)。 */
        r = make_regs(COMPAT_SYS_WRITE, 1, 0x1234, 2);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EFAULT,
              "write from bad pointer is EFAULT");

        /* read(0, buf, 2) -> 入力 "ab" が読める。 */
        r = make_regs(COMPAT_SYS_READ, 0, (uint64_t)(stack + 64), 2);
        check(compat_syscall_dispatch_c(&r) == 2, "read stdin returns 2");
        check(stack[64] == 'a' && stack[65] == 'b', "stdin bytes match");
        /* もう読むものは無い -> 0。 */
        r = make_regs(COMPAT_SYS_READ, 0, (uint64_t)(stack + 64), 2);
        check(compat_syscall_dispatch_c(&r) == 0, "read stdin empty is 0");

        /* getpid -> 1。 */
        r = make_regs(COMPAT_SYS_GETPID, 0, 0, 0);
        check(compat_syscall_dispatch_c(&r) == 1, "getpid returns 1");

        /* brk: 0 で今の位置、範囲内なら進む、範囲外なら動かない。 */
        uint64_t brk0 = (uint64_t)img.brk_base;
        r = make_regs(COMPAT_SYS_BRK, 0, 0, 0);
        check(compat_syscall_dispatch_c(&r) == (int64_t)brk0,
              "brk(0) returns base");
        r = make_regs(COMPAT_SYS_BRK, brk0 + 4096, 0, 0);
        check(compat_syscall_dispatch_c(&r) == (int64_t)(brk0 + 4096),
              "brk grows inside the arena");
        r = make_regs(COMPAT_SYS_BRK, brk0 + 1024 * 1024, 0, 0);
        check(compat_syscall_dispatch_c(&r) == (int64_t)(brk0 + 4096),
              "brk outside the arena does not move");

        /* open/read/fstat/close: about.txt を読む。 */
        const char *path = "about.txt";
        memcpy(stack + 128, path, strlen(path) + 1);
        r = make_regs(COMPAT_SYS_OPEN, (uint64_t)(stack + 128), 0, 0);
        int64_t fd = compat_syscall_dispatch_c(&r);
        check(fd >= 3, "open about.txt returns a fd");
        uint64_t sz = 0;
        r = make_regs(COMPAT_SYS_FSTAT, (uint64_t)fd,
                      (uint64_t)(stack + 256), 0);
        check(compat_syscall_dispatch_c(&r) == 0, "fstat succeeds");
        memcpy(&sz, stack + 256, 8);
        size_t want_len = 0;
        (void)vfs_read_ptr("about.txt", &want_len);
        check(sz == want_len, "fstat size matches the file");
        r = make_regs(COMPAT_SYS_READ, (uint64_t)fd,
                      (uint64_t)(stack + 512), 256);
        int64_t got = compat_syscall_dispatch_c(&r);
        check(got == (int64_t)want_len, "read returns the whole file");
        check(memcmp(stack + 512, vfs_read_ptr("about.txt", NULL),
                     want_len) == 0,
              "read content matches the file");
        r = make_regs(COMPAT_SYS_READ, (uint64_t)fd,
                      (uint64_t)(stack + 512), 256);
        check(compat_syscall_dispatch_c(&r) == 0, "read at EOF is 0");
        /* lseek: SEEK_END は大きさ、SEEK_SET で戻ると
         * 先頭から読み直せる。負の位置は EINVAL。 */
        r = make_regs(COMPAT_SYS_LSEEK, (uint64_t)fd, 0,
                      COMPAT_SEEK_END);
        check(compat_syscall_dispatch_c(&r) == (int64_t)want_len,
              "lseek END returns the size");
        r = make_regs(COMPAT_SYS_LSEEK, (uint64_t)fd, 7,
                      COMPAT_SEEK_SET);
        check(compat_syscall_dispatch_c(&r) == 7,
              "lseek SET returns the new offset");
        r = make_regs(COMPAT_SYS_READ, (uint64_t)fd,
                      (uint64_t)(stack + 512), 4);
        check(compat_syscall_dispatch_c(&r) == 4,
              "read after lseek returns bytes");
        check(memcmp(stack + 512,
                     vfs_read_ptr("about.txt", NULL) + 7, 4) == 0,
              "read after lseek starts at the offset");
        r = make_regs(COMPAT_SYS_LSEEK, (uint64_t)fd,
                      (uint64_t)-100, COMPAT_SEEK_SET);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EINVAL,
              "lseek to a negative offset is EINVAL");
        r = make_regs(COMPAT_SYS_LSEEK, 1, 0, COMPAT_SEEK_SET);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EINVAL,
              "lseek on stdout is EINVAL");
        r = make_regs(COMPAT_SYS_CLOSE, (uint64_t)fd, 0, 0);
        check(compat_syscall_dispatch_c(&r) == 0, "close succeeds");
        r = make_regs(COMPAT_SYS_READ, (uint64_t)fd,
                      (uint64_t)(stack + 512), 256);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EBADF,
              "read after close is EBADF");

        /* open: 無いファイルは ENOENT。 */
        memcpy(stack + 128, "nope.txt", 9);
        r = make_regs(COMPAT_SYS_OPEN, (uint64_t)(stack + 128), 0, 0);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_ENOENT,
              "open missing file is ENOENT");

        /* 保護ファイル: ゲストからは読みも書きも禁止。 */
        check(vfs_write(".account", "v1$xx$yy", 8) == 0,
              "host plants an account file");
        memcpy(stack + 128, ".account", 9);
        r = make_regs(COMPAT_SYS_OPEN, (uint64_t)(stack + 128), 0, 0);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EACCES,
              "guest cannot read .account");
        memcpy(stack + 128, "settings.txt", 13);
        r = make_regs(COMPAT_SYS_OPEN, (uint64_t)(stack + 128),
                      COMPAT_O_WRONLY | COMPAT_O_CREAT, 0);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EACCES,
              "guest cannot write settings.txt");
        memcpy(stack + 128, "pqkem.key", 10);
        r = make_regs(COMPAT_SYS_OPEN, (uint64_t)(stack + 128), 0, 0);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EACCES,
              "guest cannot read .key files");
        /* 既存ファイルの上書きも禁止 (自分で作った物以外)。 */
        memcpy(stack + 128, "about.txt", 10);
        r = make_regs(COMPAT_SYS_OPEN, (uint64_t)(stack + 128),
                      COMPAT_O_WRONLY | COMPAT_O_TRUNC, 0);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EACCES,
              "guest cannot overwrite existing files");

        /* 書き込み: 作って書いて閉じると VFS に残る。 */
        memcpy(stack + 128, "guest.txt", 10);
        r = make_regs(COMPAT_SYS_OPEN, (uint64_t)(stack + 128),
                      COMPAT_O_WRONLY | COMPAT_O_CREAT | COMPAT_O_TRUNC,
                      0);
        fd = compat_syscall_dispatch_c(&r);
        check(fd >= 3, "open for write returns a fd");
        memcpy(stack + 512, "guest-data", 10);
        r = make_regs(COMPAT_SYS_WRITE, (uint64_t)fd,
                      (uint64_t)(stack + 512), 10);
        check(compat_syscall_dispatch_c(&r) == 10,
              "write to file returns 10");
        r = make_regs(COMPAT_SYS_CLOSE, (uint64_t)fd, 0, 0);
        check(compat_syscall_dispatch_c(&r) == 0,
              "close after write succeeds");
        {
            size_t glen = 0;
            const char *g = vfs_read_ptr("guest.txt", &glen);
            check(g != NULL && glen == 10 &&
                      memcmp(g, "guest-data", 10) == 0,
                  "guest file is stored in VFS after close");
        }

        /* lseek + 書き直し: 自分で作ったファイルは
         * 開き直して位置を戻し、上書きできる。 */
        memcpy(stack + 128, "guest.txt", 10);
        r = make_regs(COMPAT_SYS_OPEN, (uint64_t)(stack + 128),
                      COMPAT_O_WRONLY, 0);
        fd = compat_syscall_dispatch_c(&r);
        check(fd >= 3, "reopen own file for write");
        r = make_regs(COMPAT_SYS_LSEEK, (uint64_t)fd, 6,
                      COMPAT_SEEK_SET);
        check(compat_syscall_dispatch_c(&r) == 6,
              "lseek in write mode returns 6");
        memcpy(stack + 512, "DATA", 4);
        r = make_regs(COMPAT_SYS_WRITE, (uint64_t)fd,
                      (uint64_t)(stack + 512), 4);
        check(compat_syscall_dispatch_c(&r) == 4,
              "write at the seek position returns 4");
        r = make_regs(COMPAT_SYS_CLOSE, (uint64_t)fd, 0, 0);
        check(compat_syscall_dispatch_c(&r) == 0,
              "close after seek-write succeeds");
        {
            size_t glen = 0;
            const char *g = vfs_read_ptr("guest.txt", &glen);
            check(g != NULL && glen == 10 &&
                      memcmp(g, "guest-DATA", 10) == 0,
                  "seek-write replaced bytes in place");
        }

        /* fb_info: 窓の大きさ (640x480) が返る。 */
        r = make_regs(COMPAT_SYS_FB_INFO, (uint64_t)(stack + 512),
                      0, 0);
        check(compat_syscall_dispatch_c(&r) == 0, "fb_info returns 0");
        check(*(uint32_t *)(stack + 512) == COMPAT_FB_WIDTH &&
                  *(uint32_t *)(stack + 516) == COMPAT_FB_HEIGHT,
              "fb_info reports 640x480");

        /* fb_fill: はみ出しは窓の範囲に切り取られる。 */
        memset(fake_fb, 0, sizeof(fake_fb));
        r = make_regs5(COMPAT_SYS_FB_FILL, 630, 470, 100, 100,
                       0x112233);
        check(compat_syscall_dispatch_c(&r) == 0, "fb_fill returns 0");
        check(fake_fb[470 * COMPAT_FB_WIDTH + 630] == 0x112233 &&
                  fake_fb[479 * COMPAT_FB_WIDTH + 639] == 0x112233,
              "fb_fill painted the clipped corner");
        check(fake_fb[0] == 0, "fb_fill stayed inside the window");

        /* fb_blit: 画素がそのまま入る。 */
        {
            uint32_t px4[4] = {1, 2, 3, 4};
            memcpy(stack + 600, px4, sizeof(px4));
        }
        r = make_regs5(COMPAT_SYS_FB_BLIT, 0, 0, 2, 2,
                       (uint64_t)(stack + 600));
        check(compat_syscall_dispatch_c(&r) == 0, "fb_blit returns 0");
        check(fake_fb[0] == 1 && fake_fb[1] == 2 &&
                  fake_fb[COMPAT_FB_WIDTH] == 3 &&
                  fake_fb[COMPAT_FB_WIDTH + 1] == 4,
              "fb_blit copied the pixels");

        /* fb_flush: フックが数えられる。 */
        fake_flushes = 0;
        r = make_regs(COMPAT_SYS_FB_FLUSH, 0, 0, 0);
        check(compat_syscall_dispatch_c(&r) == 0 &&
                  fake_flushes == 1,
              "fb_flush reaches the hook");

        /* net: 解決 → 接続 → 送受信 → 切断。 */
        memcpy(stack + 700, "host", 5);
        r = make_regs(COMPAT_SYS_NET_RESOLVE,
                      (uint64_t)(stack + 700),
                      (uint64_t)(stack + 720), 0);
        check(compat_syscall_dispatch_c(&r) == 0,
              "net_resolve returns 0");
        check(stack[720] == 10 && stack[721] == 0 &&
                  stack[722] == 2 && stack[723] == 2,
              "net_resolve wrote the address");
        r = make_regs(COMPAT_SYS_NET_CONNECT,
                      (uint64_t)(stack + 700), 8730, 0);
        check(compat_syscall_dispatch_c(&r) == 0,
              "net_connect returns handle 0");
        memcpy(stack + 730, "abc", 3);
        r = make_regs(COMPAT_SYS_NET_SEND, 1,
                      (uint64_t)(stack + 730), 3);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EBADF,
              "net_send on a closed handle is EBADF");
        r = make_regs(COMPAT_SYS_NET_SEND, 0,
                      (uint64_t)(stack + 730), 3);
        check(compat_syscall_dispatch_c(&r) == 3 &&
                  fake_sent_len == 3 &&
                  memcmp(fake_sent, "abc", 3) == 0,
              "net_send delivered the bytes");
        r = make_regs(COMPAT_SYS_NET_RECV, 0,
                      (uint64_t)(stack + 740), 16);
        check(compat_syscall_dispatch_c(&r) == 5 &&
                  memcmp(stack + 740, "pong!", 5) == 0,
              "net_recv got the reply");
        r = make_regs(COMPAT_SYS_NET_CLOSE, 0, 0, 0);
        check(compat_syscall_dispatch_c(&r) == 0,
              "net_close returns 0");
        r = make_regs(COMPAT_SYS_NET_CLOSE, 0, 0, 0);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_EBADF,
              "double net_close is EBADF");

        /* ext が無い文脈では ENOSYS (嘘の成功をしない)。 */
        compat_set_context(&g_test_io_noext, &img, malloc, free);
        r = make_regs5(COMPAT_SYS_FB_FILL, 0, 0, 10, 10, 0);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_ENOSYS,
              "fb_fill without ext is ENOSYS");
        compat_set_context(&g_test_io, &img, malloc, free);

        /* 未対応の呼び出し: -38 を返し、ログに残る。 */
        r = make_regs(999, 0, 0, 0);
        check(compat_syscall_dispatch_c(&r) == -COMPAT_ENOSYS,
              "unknown call returns ENOSYS");
        check(g_unsupported_count == 1 && g_last_unsupported == 999,
              "unsupported call is logged");

        /* exit: 合図が立ち、終了コードが残る。 */
        r = make_regs(COMPAT_SYS_EXIT, 42, 0, 0);
        check(compat_syscall_dispatch_c(&r) == 0, "exit returns 0");
        check(compat_exit_pending == 1, "exit sets the pending flag");
        check(compat_exit_code == 42, "exit code is kept");
        compat_reset_state();
        check(compat_exit_pending == 0, "reset clears the exit flag");

        compat_clear_ranges();
        compat_set_context(NULL, NULL, NULL, NULL);
    }
    compat_free_image(&img, free);
    check(img.seg_base[0] == NULL && img.brk_base == NULL,
          "free clears the image");

    printf("compat tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
