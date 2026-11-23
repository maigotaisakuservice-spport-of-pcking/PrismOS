/* PrismOS — FAT16 ライタのホストテスト。
 *
 * カーネルと同じ kernel/fat16.c をホストで動かす。ディスクは
 * メモリ上のイメージで、MBR + FAT16 パーティションをテスト側で
 * 組み立てる。`make test` で走る。
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "fat16.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

static void fill(uint8_t *dst, uint8_t v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = v;
    }
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---- メモリ上のディスク ---- */

#define DISK_SECTORS 10240u /* 5MB: ギャップ 2048 + P1 8192 */
#define PART_LBA 2048u
#define PART_SECTORS 8192u

static uint8_t *disk_img;
/* FAT 領域へのセクタ書き込み回数 (バッチ化の検査用)。 */
static long fat_writes = 0;

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int mem_read(void *ctx, uint32_t lba, uint8_t *buf) {
    (void)ctx;
    if (lba >= DISK_SECTORS) {
        return -1;
    }
    copy_bytes(buf, disk_img + (size_t)lba * 512, 512);
    return 0;
}

static int mem_write(void *ctx, uint32_t lba, const uint8_t *buf) {
    (void)ctx;
    if (lba >= DISK_SECTORS) {
        return -1;
    }
    /* FAT 領域 (P1 先頭 + 1 から 64 セクタ) への書き込みを数える。 */
    if (lba >= PART_LBA + 1 && lba < PART_LBA + 1 + 64) {
        fat_writes++;
    }
    copy_bytes(disk_img + (size_t)lba * 512, buf, 512);
    return 0;
}

static void write_dir_entry(uint8_t *sec, int idx, const char *name11,
                            uint8_t attr, uint16_t cluster, uint32_t size) {
    uint8_t *e = sec + idx * 32;
    for (int i = 0; i < 11; i++) {
        e[i] = (uint8_t)name11[i];
    }
    e[11] = attr;
    for (int i = 12; i < 26; i++) {
        e[i] = 0;
    }
    put16(e + 26, cluster);
    put32(e + 28, size);
}

/* P1: 8192 セクタ、1 セクタ/クラスタ、FAT 32 セクタ x2、
 * ルート 512 件 (32 セクタ)。クラスタ数 8095 で FAT16。 */
static void build_image(void) {
    free(disk_img); /* 再構築のたびに前の盤を解放する (漏れ防止) */
    disk_img = (uint8_t *)calloc(DISK_SECTORS, 512);
    /* MBR: P1 は FAT16 (0x0E)、LBA 2048、8192 セクタ。 */
    uint8_t *mbr = disk_img;
    mbr[446] = 0x80;
    mbr[446 + 4] = 0x0E;
    put32(mbr + 446 + 8, PART_LBA);
    put32(mbr + 446 + 12, PART_SECTORS);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    /* BPB。 */
    uint8_t *bpb = disk_img + (size_t)PART_LBA * 512;
    bpb[0] = 0xEB;
    bpb[1] = 0x3C;
    bpb[2] = 0x90;
    put16(bpb + 11, 512);
    bpb[13] = 1; /* 1 セクタ/クラスタ */
    put16(bpb + 14, 1);
    bpb[16] = 2;
    put16(bpb + 17, 512);
    put16(bpb + 19, 8192);
    bpb[21] = 0xF8;
    put16(bpb + 22, 32); /* FAT 32 セクタ */
    /* FAT (2 コピー): [0]=0xFFF8、[1]=0xFFFF、[2]=EOC (/boot)。 */
    for (int copy = 0; copy < 2; copy++) {
        uint8_t *fat = disk_img +
                       (size_t)(PART_LBA + 1 + copy * 32) * 512;
        put16(fat + 0, 0xFFF8);
        put16(fat + 2, 0xFFFF);
        put16(fat + 4, 0xFFFF); /* クラスタ 2 (/boot) は 1 個だけ */
    }
    /* ルート: ラベルと BOOT ディレクトリ (クラスタ 2)。 */
    uint8_t *root = disk_img + (size_t)(PART_LBA + 1 + 64) * 512;
    write_dir_entry(root, 0, "PRISMBOOT  ", 0x08, 0, 0);
    write_dir_entry(root, 1, "BOOT       ", 0x10, 2, 0);
    /* /boot: "." と ".."、それに長い名前の入口 (LFN) を 1 つ。
     * LFN の後ろの実エントリが正しく見つかることを確かめる。 */
    uint8_t *boot = disk_img + (size_t)(PART_LBA + 1 + 64 + 32) * 512;
    write_dir_entry(boot, 0, ".          ", 0x10, 2, 0);
    write_dir_entry(boot, 1, "..         ", 0x10, 0, 0);
    uint8_t *lfn = boot + 2 * 32;
    lfn[0] = 0x41; /* 順序 1 + 最後 */
    lfn[11] = 0x0F; /* LFN 属性 */
    write_dir_entry(boot, 3, "OLDKERN BIN", 0x20, 5, 1234);
    /* OLDKERN.BIN の鎖は無い (クラスタ 5 は FAT で空き)。
     * 見つかることだけ確かめ、あとで消す。 */
}

static int open_fs(fat16_fs *fs) {
    fat16_dev dev;
    dev.ctx = NULL;
    dev.read_sector = mem_read;
    dev.write_sector = mem_write;
    dev.total_sectors = DISK_SECTORS;
    return fat16_open(&dev, fs);
}

int main(void) {
    build_image();
    if (disk_img == NULL) {
        printf("FAIL: out of memory\n");
        return 1;
    }
    fat16_fs fs;
    check(open_fs(&fs) == 0, "open FAT16 partition");
    check(fs.cluster_count == 8095, "cluster count 8095");
    check(fs.sectors_per_cluster == 1, "1 sector per cluster");
    check(fs.fat_sectors == 32, "FAT 32 sectors");
    check(fs.data_start == PART_LBA + 1 + 64 + 32,
          "data start after FATs and root dir");

    uint64_t free0 = 0;
    check(fat16_free_bytes(&fs, &free0) == 0, "free bytes readable");
    /* 空き = 全クラスタ - /boot の 1 個。 */
    check(free0 == (uint64_t)(8095 - 1) * 512, "free bytes at start");

    /* LFN の後ろのエントリが見つかる。 */
    uint32_t sz = 0;
    check(fat16_file_size(&fs, "/boot/oldkern.bin", &sz) == 0 &&
              sz == 1234,
          "find entry after LFN");
    check(fat16_file_size(&fs, "/boot/nope.bin", &sz) != 0,
          "missing file not found");
    check(fat16_file_size(&fs, "/BOOT/OLDKERN.BIN", &sz) == 0,
          "names are case-insensitive");

    /* ---- 小さいファイルの往復 (変な刻みで書く) ---- */
    static uint8_t data[6000];
    for (int i = 0; i < 6000; i++) {
        data[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    }
    fat16_writer w;
    check(fat16_write_begin(&fs, "/boot/test.txt", &w) == 0,
          "write begin small file");
    check(fat16_write_chunk(&w, data, 100) == 0, "chunk 100");
    check(fat16_write_chunk(&w, data + 100, 4096) == 0, "chunk 4096");
    check(fat16_write_chunk(&w, data + 4196, 1804) == 0, "chunk 1804");
    check(fat16_write_finish(&w) == 0, "write finish");
    check(fat16_file_size(&fs, "/boot/test.txt", &sz) == 0 &&
              sz == 6000,
          "small file size 6000");
    static uint8_t back[6000];
    size_t got = 0;
    check(fat16_read_file(&fs, "/boot/test.txt", back, sizeof(back),
                          &got) == 0 &&
              got == 6000 && bytes_eq(back, data, 6000),
          "small file round-trip");
    /* FAT の 2 つのコピーが同じ。 */
    check(bytes_eq(disk_img + (size_t)(PART_LBA + 1) * 512,
                   disk_img + (size_t)(PART_LBA + 1 + 32) * 512,
                   32 * 512),
          "both FAT copies identical");
    uint64_t free1 = 0;
    check(fat16_free_bytes(&fs, &free1) == 0, "free bytes after write");
    check(free1 == free0 - (uint64_t)12 * 512,
          "free bytes drop by 12 clusters");

    /* ---- 上書き: 短くすると古いクラスタが戻る ---- */
    check(fat16_write_begin(&fs, "/boot/test.txt", &w) == 0,
          "overwrite begin");
    check(fat16_write_chunk(&w, data, 512) == 0, "overwrite 512");
    check(fat16_write_finish(&w) == 0, "overwrite finish");
    check(fat16_file_size(&fs, "/boot/test.txt", &sz) == 0 &&
              sz == 512,
          "overwritten size 512");
    uint64_t free2 = 0;
    check(fat16_free_bytes(&fs, &free2) == 0, "free bytes after overwrite");
    check(free2 == free0 - (uint64_t)1 * 512,
          "free bytes back after overwrite");

    /* ---- 削除 ---- */
    check(fat16_delete(&fs, "/boot/test.txt") == 0, "delete file");
    check(fat16_file_size(&fs, "/boot/test.txt", &sz) != 0,
          "deleted file is gone");
    uint64_t free3 = 0;
    check(fat16_free_bytes(&fs, &free3) == 0, "free bytes after delete");
    check(free3 == free0, "free bytes restored after delete");
    check(fat16_delete(&fs, "/boot/test.txt") != 0,
          "delete missing file fails");

    /* ---- 入れ替え (原子的な置き換えの要) ---- */
    static uint8_t a_data[3000];
    static uint8_t b_data[3000];
    fill(a_data, 0xAA, sizeof(a_data));
    fill(b_data, 0xBB, sizeof(b_data));
    check(fat16_write_begin(&fs, "/boot/prismos.elf", &w) == 0,
          "swap: write A begin");
    check(fat16_write_chunk(&w, a_data, sizeof(a_data)) == 0,
          "swap: write A");
    check(fat16_write_finish(&w) == 0, "swap: finish A");
    check(fat16_write_begin(&fs, "/boot/prismos.new", &w) == 0,
          "swap: write B begin");
    check(fat16_write_chunk(&w, b_data, sizeof(b_data)) == 0,
          "swap: write B");
    check(fat16_write_finish(&w) == 0, "swap: finish B");
    check(fat16_swap_entries(&fs, "/boot/prismos.elf",
                             "/boot/prismos.new") == 0,
          "swap entries");
    check(fat16_read_file(&fs, "/boot/prismos.elf", back, sizeof(back),
                          &got) == 0 &&
              got == 3000 && bytes_eq(back, b_data, 3000),
          "after swap, elf holds B");
    check(fat16_read_file(&fs, "/boot/prismos.new", back, sizeof(back),
                          &got) == 0 &&
              got == 3000 && bytes_eq(back, a_data, 3000),
          "after swap, new holds A");
    check(fat16_swap_entries(&fs, "/boot/prismos.elf",
                             "/boot/nope.bin") != 0,
          "swap with missing file fails");
    /* 掃除: .new を消すと古い中身のクラスタが戻る。 */
    check(fat16_delete(&fs, "/boot/prismos.new") == 0,
          "delete swapped-out old file");
    check(fat16_file_size(&fs, "/boot/prismos.elf", &sz) == 0 &&
              sz == 3000,
          "elf survives new cleanup");

    /* ---- 大きいファイル (200KB、FAT のセクタをまたぐ) ---- */
    static uint8_t big[200 * 1024];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i & 0xFF);
    }
    long fat_writes_before = fat_writes;
    check(fat16_write_begin(&fs, "/boot/big.bin", &w) == 0,
          "big: write begin");
    size_t off = 0;
    int big_ok = 1;
    while (off < sizeof(big)) {
        size_t n = sizeof(big) - off;
        if (n > 7000) {
            n = 7000;
        }
        if (fat16_write_chunk(&w, big + off, n) != 0) {
            big_ok = 0;
            break;
        }
        off += n;
    }
    check(big_ok, "big: all chunks written");
    check(fat16_write_finish(&w) == 0, "big: finish");
    /* FAT への書き出しはまとまっていること: 400 クラスタでも
     * FAT セクタ数 (最大 4 セクタ想定) x 2 コピー = 8 回以下。
     * まとめなければ割り当て 1 個ごとに 2 回で 800 回になる。 */
    check(fat_writes - fat_writes_before <= 8,
          "big: FAT writes batched");
    /* 生イメージの FAT を直接見て鎖が揃っていること
     * (API のキャッシュ経由ではなく、デバイス上に書けた証拠)。 */
    {
        const uint8_t *boot_sec =
            disk_img + (size_t)(PART_LBA + 1 + 64 + 32) * 512;
        uint16_t first = 0;
        for (int i = 0; i < 16; i++) {
            const uint8_t *e = boot_sec + i * 32;
            if (e[0] == 'B' && e[1] == 'I' && e[2] == 'G') {
                first = (uint16_t)(e[26] | ((uint16_t)e[27] << 8));
                break;
            }
        }
        check(first != 0, "big: first cluster in raw dir");
        int raw_ok = 0;
        if (first != 0) {
            raw_ok = 1;
            uint32_t c = first;
            uint32_t count = 0;
            while (count < 1000) {
                if (c < 2 || c >= 8095 + 2) {
                    raw_ok = 0;
                    break;
                }
                const uint8_t *fat0 =
                    disk_img + (size_t)(PART_LBA + 1) * 512;
                const uint8_t *fat1 =
                    disk_img + (size_t)(PART_LBA + 1 + 32) * 512;
                uint16_t v0 = (uint16_t)(fat0[c * 2] |
                                         ((uint16_t)fat0[c * 2 + 1] << 8));
                uint16_t v1 = (uint16_t)(fat1[c * 2] |
                                         ((uint16_t)fat1[c * 2 + 1] << 8));
                if (v0 != v1) {
                    raw_ok = 0; /* 2 コピーが食い違う */
                    break;
                }
                count++;
                if (v0 >= 0xFFF8u) {
                    break;
                }
                c = v0;
            }
            if (count != 400) {
                raw_ok = 0;
            }
        }
        check(raw_ok, "big: raw FAT chain is 400 clusters, both copies");
    }
    static uint8_t big_back[200 * 1024];
    check(fat16_read_file(&fs, "/boot/big.bin", big_back,
                          sizeof(big_back), &got) == 0 &&
              got == sizeof(big) && bytes_eq(big_back, big, sizeof(big)),
          "big file round-trip");
    check(fat16_delete(&fs, "/boot/big.bin") == 0, "big: delete");

    /* ---- 中断 (abort): ファイルもクラスタも残らない ---- */
    uint64_t free_before_abort = 0;
    check(fat16_free_bytes(&fs, &free_before_abort) == 0,
          "free bytes before abort");
    check(fat16_write_begin(&fs, "/boot/part.bin", &w) == 0,
          "abort: begin");
    check(fat16_write_chunk(&w, big, 5000) == 0, "abort: some chunks");
    check(fat16_write_abort(&w) == 0, "abort");
    check(fat16_file_size(&fs, "/boot/part.bin", &sz) != 0,
          "aborted file is gone");
    uint64_t free_after_abort = 0;
    check(fat16_free_bytes(&fs, &free_after_abort) == 0,
          "free bytes after abort");
    check(free_after_abort == free_before_abort,
          "free bytes restored after abort");

    /* ---- 空き切れ: 残り 10 クラスタにして大きい書き込み ---- */
    {
        /* FAT を直接いじって、クラスタ 3 以降を全部使用中にする。
         * (クラスタ 2 は /boot だけが残る。prismos.elf の鎖は
         * 壊れるが、この後の検査では使わない。) */
        uint8_t *fat = disk_img + (size_t)(PART_LBA + 1) * 512;
        for (uint32_t c = 3; c < 8095 + 2; c++) {
            put16(fat + c * 2, 0xFFFF);
        }
        uint8_t *fat2 = disk_img + (size_t)(PART_LBA + 1 + 32) * 512;
        for (uint32_t c = 3; c < 8095 + 2; c++) {
            put16(fat2 + c * 2, 0xFFFF);
        }
        /* キャッシュが古くなるので開き直す。 */
        check(open_fs(&fs) == 0, "reopen after FAT fill");
        uint64_t free_now = 0;
        check(fat16_free_bytes(&fs, &free_now) == 0,
              "free bytes when nearly full");
        check(free_now == (uint64_t)0, "no free clusters left");
        check(fat16_write_begin(&fs, "/boot/full.bin", &w) == 0,
              "full: begin works (entry only)");
        check(fat16_write_chunk(&w, big, 100) != 0,
              "full: chunk fails with no space");
        check(fat16_write_abort(&w) == 0, "full: abort");
        check(fat16_file_size(&fs, "/boot/full.bin", &sz) != 0,
              "full: partial file is gone");
    }

    /* ---- 名前の検査 ---- */
    build_image(); /* きれいな盤に戻す */
    check(open_fs(&fs) == 0, "reopen clean image");
    check(fat16_write_begin(&fs, "/boot/toolongname.txt", &w) != 0,
          "name too long rejected");
    check(fat16_write_begin(&fs, "/a/b/c/d/e/f/g/h/i.txt", &w) != 0,
          "path too deep rejected");
    check(fat16_write_begin(&fs, "/boot/ok.dat", &w) == 0,
          "short name accepted");
    check(fat16_write_chunk(&w, data, 10) == 0, "short name chunk");
    check(fat16_write_finish(&w) == 0, "short name finish");
    check(fat16_delete(&fs, "/boot/ok.dat") == 0, "short name delete");

    /* ---- FAT12 は断る (古い 8MB の起動パーティション) ---- */
    {
        /* 4096 セクタの区切りを作る: クラスタ数 4031 < 4085。 */
        uint8_t *small = (uint8_t *)calloc(2048 + 4096, 512);
        small[446] = 0x80;
        small[446 + 4] = 0x0E;
        put32(small + 446 + 8, 2048);
        put32(small + 446 + 12, 4096);
        small[510] = 0x55;
        small[511] = 0xAA;
        uint8_t *b = small + (size_t)2048 * 512;
        b[0] = 0xEB;
        b[1] = 0x3C;
        b[2] = 0x90;
        put16(b + 11, 512);
        b[13] = 4; /* 4 セクタ/クラスタ */
        put16(b + 14, 1);
        b[16] = 2;
        put16(b + 17, 512);
        put16(b + 19, 4096);
        b[21] = 0xF8;
        put16(b + 22, 12);
        free(disk_img);
        disk_img = small;
        fat16_fs fs2;
        check(open_fs(&fs2) == -2, "FAT12 partition refused");
        free(small);
        disk_img = NULL;
    }

    /* ---- 空のファイル ---- */
    build_image();
    check(open_fs(&fs) == 0, "reopen for empty file");
    check(fat16_write_begin(&fs, "/boot/empty.bin", &w) == 0,
          "empty: begin");
    check(fat16_write_finish(&w) == 0, "empty: finish");
    check(fat16_file_size(&fs, "/boot/empty.bin", &sz) == 0 &&
              sz == 0,
          "empty file size 0");
    check(fat16_read_file(&fs, "/boot/empty.bin", back, sizeof(back),
                          &got) == 0 &&
              got == 0,
          "empty file reads 0 bytes");
    check(fat16_delete(&fs, "/boot/empty.bin") == 0, "empty: delete");

    free(disk_img);
    if (failures == 0) {
        printf("fat16 tests: %d checks, 0 failures\n", checks);
        printf("PASS\n");
        return 0;
    }
    printf("fat16 tests: %d checks, %d failures\n", checks, failures);
    printf("FAIL\n");
    return 1;
}
