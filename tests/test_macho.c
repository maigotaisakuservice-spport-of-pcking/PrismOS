/* PrismOS — Mach-O ローダーのホストテスト。
 *
 * 合成した Mach-O (hello.macho と同じ配置) を macho_parse に
 * 読ませ、セグメント・入口・動的リンク拒否・壊れた入力を
 * 確かめる。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macho.h"
#include "sample_macho.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
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

/* 合成 Mach-O: __TEXT セグメント (vmaddr 0x100000000、
 * fileoff 128、filesize 64) + LC_MAIN (entryoff 0)。
 * with_dyld が 1 なら LC_LOAD_DYLIB を 1 個足す。
 * with_thread が 1 なら LC_MAIN の代わりに LC_UNIXTHREAD
 * (RIP = 0x100000010) を置く。 */
static size_t build_macho(uint8_t *d, int with_dyld,
                          int with_thread) {
    memset(d, 0, 2048);
    uint32_t ncmds = with_dyld ? 3 : 2;
    uint32_t sizeofcmds = 72 + 24 + (with_dyld ? 24 : 0);
    if (with_thread) {
        sizeofcmds = 72 + 16 + 42 * 4;
    }
    put32(d, 0, 0xFEEDFACF);
    put32(d, 4, 0x01000007);
    put32(d, 8, 3);
    put32(d, 12, 2);
    put32(d, 16, ncmds);
    put32(d, 20, sizeofcmds);
    put32(d, 24, 0);
    put32(d, 28, 0);
    size_t off = 32;
    /* LC_SEGMENT_64 */
    put32(d, off, 0x19);
    put32(d, off + 4, 72);
    memcpy(d + off + 8, "__TEXT", 6);
    put64(d, off + 24, 0x100000000ULL);
    put64(d, off + 32, 0x1000);
    put64(d, off + 40, 128);
    put64(d, off + 48, 64);
    put32(d, off + 56, 7);
    put32(d, off + 60, 5);
    put32(d, off + 64, 0);
    put32(d, off + 68, 0);
    off += 72;
    if (with_thread) {
        /* LC_UNIXTHREAD: flavor 4, count 42, RIP は 17 番目 */
        put32(d, off, 0x5);
        put32(d, off + 4, 16 + 42 * 4);
        put32(d, off + 8, 4);
        put32(d, off + 12, 42);
        put64(d, off + 16 + 16 * 8, 0x100000010ULL);
        off += 16 + 42 * 4;
    } else {
        /* LC_MAIN */
        put32(d, off, 0x80000028);
        put32(d, off + 4, 24);
        put64(d, off + 8, 0);
        put64(d, off + 16, 0);
        off += 24;
    }
    if (with_dyld) {
        put32(d, off, 0xC);
        put32(d, off + 4, 24);
        put32(d, off + 8, 24);
        put32(d, off + 12, 2);
        put32(d, off + 16, 0);
        put32(d, off + 20, 0);
        off += 24;
    }
    (void)off;
    return 2048;
}

int main(void) {
    static uint8_t m[2048];
    macho_info info;
    size_t len = build_macho(m, 0, 0);
    check(macho_parse(m, len, &info) == 0, "synthetic Mach-O parses");
    check(info.entry == 0x100000000ULL, "entry from LC_MAIN");
    check(info.image_base == 0x100000000ULL, "image base read");
    check(info.seg_count == 1, "one segment read");
    check(strcmp(info.segs[0].segname, "__TEXT") == 0,
          "segment is __TEXT");
    check(info.segs[0].filesize == 64, "segment filesize read");
    check(info.has_main == 1, "LC_MAIN seen");
    /* LC_UNIXTHREAD からの入口。 */
    len = build_macho(m, 0, 1);
    check(macho_parse(m, len, &info) == 0,
          "UNIXTHREAD Mach-O parses");
    check(info.entry == 0x100000010ULL, "entry from thread state");
    check(info.has_main == 0, "no LC_MAIN in thread variant");
    /* dyld を要するものは断る。 */
    len = build_macho(m, 1, 0);
    check(macho_parse(m, len, &info) == -6,
          "dylib load command rejected");
    /* 壊れた入力。 */
    len = build_macho(m, 0, 0);
    m[0] = 0x00;
    check(macho_parse(m, len, &info) == -1, "bad magic rejected");
    m[0] = 0xCF;
    check(macho_parse(m, 100, &info) == -3,
          "truncated Mach-O rejected");
    put32(m, 4, 0x0100000C); /* ARM64 */
    check(macho_parse(m, len, &info) == -2,
          "wrong CPU type rejected");
    put32(m, 4, 0x01000007);
    /* 入口がセグメントの外。 */
    put64(m, 32 + 72 + 8, 0x2000);
    check(macho_parse(m, len, &info) == -5,
          "entry outside segment rejected");
    /* 実物の生成ゲスト (hello.macho) も読めること。 */
    {
        extern const uint8_t compat_guest_hello_macho[];
        extern const size_t compat_guest_hello_macho_len;
        check(macho_parse(compat_guest_hello_macho,
                          compat_guest_hello_macho_len,
                          &info) == 0,
              "generated hello.macho parses");
        check(info.entry == 0x100000000ULL,
              "generated guest entry is 0x100000000");
    }
    printf("macho tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
