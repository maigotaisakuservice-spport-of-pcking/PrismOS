/* PrismOS — diskfs 版5 (大きなファイル) のホストテスト。
 *
 * 仮想ディスク (メモリ上) で確かめる:
 *   - format は版5 を書き、小さなファイルは従来通り固定枠。
 *   - 1280x800x3 の BMP 原寸 (3072054 バイト) を書いて読み、
 *     内容・チェックサム・表の big フラグを確認する。
 *   - 大きなファイルの first-fit 再利用 (消して書き直すと
 *     同じ場所に入る)。
 *   - 小さい <-> 大きいの上書きで置き場所が切り替わる。
 *   - 境界 (16384 は小さい、16385 は大きい) と上限超過。
 *   - 小さなディスクでは大きなファイルが書けない (-1)。
 *   - 版4 ディスクは読み書きでき、大きなファイルは -1、
 *     移行後に書けるようになり、小さなファイルは残る。
 *   - 復元ポイントは小さなファイルだけを戻し、大きな
 *     ファイルのデータは残る (写していないため)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskfs.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

/* 65000 セクタ = 33.28MB (版5 の全配置 + 大きなファイル領域)。 */
#define VDISK_SECTORS 65000
static uint8_t vdisk[VDISK_SECTORS][DISKFS_SECTOR_SIZE];

static int vdisk_read(void *ctx, uint32_t lba, uint8_t *buf) {
    (void)ctx;
    if (lba >= VDISK_SECTORS) {
        return -1;
    }
    memcpy(buf, vdisk[lba], DISKFS_SECTOR_SIZE);
    return 0;
}

static int vdisk_write(void *ctx, uint32_t lba, const uint8_t *buf) {
    (void)ctx;
    if (lba >= VDISK_SECTORS) {
        return -1;
    }
    memcpy(vdisk[lba], buf, DISKFS_SECTOR_SIZE);
    return 0;
}

static diskfs_dev make_dev(void) {
    diskfs_dev dev;
    memset(&dev, 0, sizeof(dev));
    dev.ctx = NULL;
    dev.read_sector = vdisk_read;
    dev.write_sector = vdisk_write;
    dev.total_sectors = VDISK_SECTORS;
    dev.base_lba = 0;
    dev.crypto = NULL;
    return dev;
}

/* 3MB 級の内容 (BMP 原寸と同じ大きさ)。 */
#define BIG_LEN 3072054
static uint8_t big_data[BIG_LEN];
static uint8_t big_read[BIG_LEN];

static void fill_big(uint8_t seed) {
    for (size_t i = 0; i < BIG_LEN; i++) {
        big_data[i] = (uint8_t)((i * 7 + seed) & 0xFF);
    }
}

static int data_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int find_entry(diskfs_dev *dev, const char *name,
                      diskfs_entry *out) {
    static diskfs_entry entries[DISKFS_MAX_FILES];
    int count = 0;
    if (diskfs_list(dev, entries, &count) != 0) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            *out = entries[i];
            return 1;
        }
    }
    return 0;
}

int main(void) {
    diskfs_dev dev = make_dev();
    memset(vdisk, 0, sizeof(vdisk));

    /* ---- format は版5 ---- */
    check(diskfs_format(&dev) == 0, "format v5 succeeds");
    check(diskfs_is_formatted(&dev) == 1, "v5 disk is formatted");
    {
        uint8_t sb[DISKFS_SECTOR_SIZE];
        check(vdisk_read(NULL, 1, sb) == 0, "read superblock");
        check(sb[0] == 'P' && sb[1] == 'R', "superblock magic");
        uint32_t ver = (uint32_t)sb[8] | ((uint32_t)sb[9] << 8) |
                       ((uint32_t)sb[10] << 16) |
                       ((uint32_t)sb[11] << 24);
        check(ver == 5, "superblock version is 5");
    }

    /* ---- 小さなファイルは従来通り ---- */
    const char *hello = "hello diskfs v5\n";
    check(diskfs_write_file(&dev, "note.txt", (const uint8_t *)hello,
                            strlen(hello)) == 0,
          "write small file");
    diskfs_entry e;
    check(find_entry(&dev, "note.txt", &e), "small file listed");
    check(e.big == 0, "small file is not big");
    check(e.size == strlen(hello), "small file size");
    check(e.start_sector == 20, "small file in fixed frame 0");
    {
        static uint8_t buf[DISKFS_SMALL_FILE_SIZE];
        size_t len = 0;
        check(diskfs_read_file(&dev, "note.txt", buf, sizeof(buf),
                               &len) == 0,
              "read small file");
        check(len == strlen(hello) &&
                  memcmp(buf, hello, len) == 0,
              "small file content matches");
    }

    /* ---- 境界: 16384 は小さい、16385 は大きい ---- */
    static uint8_t edge[DISKFS_SMALL_FILE_SIZE + 1];
    memset(edge, 'e', sizeof(edge));
    check(diskfs_write_file(&dev, "edge16k.bin", edge,
                            DISKFS_SMALL_FILE_SIZE) == 0,
          "write exactly 16384 bytes");
    check(find_entry(&dev, "edge16k.bin", &e) && e.big == 0,
          "16384-byte file is small");
    check(diskfs_write_file(&dev, "edge16k1.bin", edge,
                            DISKFS_SMALL_FILE_SIZE + 1) == 0,
          "write exactly 16385 bytes");
    check(find_entry(&dev, "edge16k1.bin", &e) && e.big == 1,
          "16385-byte file is big");
    check(e.start_sector >= DISKFS_BIG_SECTOR,
          "big file lives in the big area");
    check(diskfs_delete_file(&dev, "edge16k.bin") == 0,
          "delete 16384-byte file");
    check(diskfs_delete_file(&dev, "edge16k1.bin") == 0,
          "delete 16385-byte file");

    /* ---- BMP 原寸 (3072054 バイト) の往復 ---- */
    fill_big(3);
    check(diskfs_write_file(&dev, "shot.bmp", big_data, BIG_LEN) == 0,
          "write full-size BMP");
    check(find_entry(&dev, "shot.bmp", &e) && e.big == 1,
          "shot.bmp is big");
    check(e.size == BIG_LEN, "shot.bmp size is 3072054");
    uint32_t shot_start = e.start_sector;
    check(shot_start == DISKFS_BIG_SECTOR,
          "first big file starts at the big area head");
    {
        size_t len = 0;
        check(diskfs_read_file(&dev, "shot.bmp", big_read,
                               sizeof(big_read), &len) == 0,
              "read full-size BMP");
        check(len == BIG_LEN, "read length is 3072054");
        check(data_eq(big_read, big_data, BIG_LEN),
              "full-size BMP content matches (checksum ok)");
    }

    /* ---- first-fit: 消して書き直すと同じ場所 ---- */
    fill_big(9);
    check(diskfs_write_file(&dev, "shot2.bmp", big_data, BIG_LEN) == 0,
          "write second big file");
    check(find_entry(&dev, "shot2.bmp", &e) &&
              e.start_sector == DISKFS_BIG_SECTOR + 6001,
          "second big file follows the first");
    check(diskfs_delete_file(&dev, "shot.bmp") == 0,
          "delete first big file");
    fill_big(5);
    check(diskfs_write_file(&dev, "shot3.bmp", big_data, BIG_LEN) == 0,
          "write third big file after delete");
    check(find_entry(&dev, "shot3.bmp", &e) &&
              e.start_sector == shot_start,
          "freed big space is reused (first-fit)");
    {
        size_t len = 0;
        check(diskfs_read_file(&dev, "shot3.bmp", big_read,
                               sizeof(big_read), &len) == 0 &&
                  len == BIG_LEN &&
                  data_eq(big_read, big_data, BIG_LEN),
              "reused big file reads back");
    }

    /* ---- 小さい <-> 大きいの上書き ---- */
    check(diskfs_write_file(&dev, "shot3.bmp",
                            (const uint8_t *)"tiny", 4) == 0,
          "overwrite big file with small content");
    check(find_entry(&dev, "shot3.bmp", &e) && e.big == 0,
          "overwritten file is small now");
    check(diskfs_write_file(&dev, "shot3.bmp", big_data, BIG_LEN) == 0,
          "overwrite small file with big content");
    check(find_entry(&dev, "shot3.bmp", &e) && e.big == 1,
          "overwritten file is big again");
    check(diskfs_delete_file(&dev, "shot3.bmp") == 0,
          "delete shot3.bmp");
    check(diskfs_delete_file(&dev, "shot2.bmp") == 0,
          "delete shot2.bmp");

    /* ---- 上限: 4MB+1 は書けない ---- */
    static uint8_t too_big[DISKFS_MAX_FILE_SIZE + 1];
    memset(too_big, 'x', sizeof(too_big));
    check(diskfs_write_file(&dev, "huge.bin", too_big,
                            sizeof(too_big)) == -1,
          "file over 4MB is rejected");

    /* ---- 復元ポイント: 小さいファイルだけ戻る ---- */
    check(diskfs_write_file(&dev, "note.txt",
                            (const uint8_t *)"version 1\n",
                            10) == 0,
          "rewrite note.txt v1");
    fill_big(11);
    check(diskfs_write_file(&dev, "snap.bmp", big_data, BIG_LEN) == 0,
          "write snap.bmp before snapshot");
    check(diskfs_snapshot_create(&dev, "before",
                                 "2026-11-24 22:00:00") == 0,
          "snapshot created with a big file present");
    check(diskfs_write_file(&dev, "note.txt",
                            (const uint8_t *)"version 2\n",
                            10) == 0,
          "rewrite note.txt v2");
    fill_big(12);
    check(diskfs_write_file(&dev, "snap.bmp", big_data, BIG_LEN) == 0,
          "overwrite snap.bmp after snapshot");
    check(diskfs_snapshot_restore(&dev, "before") == 0,
          "snapshot restored");
    {
        static uint8_t buf[DISKFS_SMALL_FILE_SIZE];
        size_t len = 0;
        check(diskfs_read_file(&dev, "note.txt", buf, sizeof(buf),
                               &len) == 0 &&
                  len == 10 && memcmp(buf, "version 1\n", 10) == 0,
              "small file reverted by snapshot");
        size_t blen = 0;
        check(diskfs_read_file(&dev, "snap.bmp", big_read,
                               sizeof(big_read), &blen) == 0 &&
                  blen == BIG_LEN &&
                  data_eq(big_read, big_data, BIG_LEN),
              "big file keeps its new content after restore");
    }
    check(diskfs_snapshot_delete(&dev, "before") == 0,
          "snapshot deleted");

    /* ---- 版4 ディスクの互換と移行 ---- */
    memset(vdisk, 0, sizeof(vdisk));
    check(diskfs_format(&dev) == 0, "format again for v4 test");
    {
        /* 版番号だけ 5 -> 4 に戻す (配置は同じなので有効な
         * 版4 ディスクになる)。 */
        uint8_t sb[DISKFS_SECTOR_SIZE];
        check(vdisk_read(NULL, 1, sb) == 0, "read sb for downgrade");
        sb[8] = 4;
        sb[9] = 0;
        sb[10] = 0;
        sb[11] = 0;
        check(vdisk_write(NULL, 1, sb) == 0, "write sb as v4");
    }
    check(diskfs_is_formatted(&dev) == 1, "downgraded disk is v4");
    check(diskfs_write_file(&dev, "old.txt", (const uint8_t *)"old",
                            3) == 0,
          "write small file on v4 disk");
    check(diskfs_write_file(&dev, "big.bmp", big_data, BIG_LEN) == -1,
          "big file rejected on v4 disk");
    check(diskfs_migrate_to_v5(&dev) == 0, "migrate v4 to v5");
    check(diskfs_migrate_to_v5(&dev) == -2,
          "migrating a v5 disk again fails");
    check(diskfs_write_file(&dev, "big.bmp", big_data, BIG_LEN) == 0,
          "big file accepted after migration");
    {
        static uint8_t buf[DISKFS_SMALL_FILE_SIZE];
        size_t len = 0;
        check(diskfs_read_file(&dev, "old.txt", buf, sizeof(buf),
                               &len) == 0 &&
                  len == 3 && memcmp(buf, "old", 3) == 0,
              "small file survives migration");
        size_t blen = 0;
        check(diskfs_read_file(&dev, "big.bmp", big_read,
                               sizeof(big_read), &blen) == 0 &&
                  blen == BIG_LEN &&
                  data_eq(big_read, big_data, BIG_LEN),
              "big file reads back after migration");
    }

    /* ---- 小さなディスクでは大きなファイルが書けない ---- */
    {
        /* 21000 セクタ: 大きなファイル領域は 424 セクタだけ。
         * vdisk の先頭 21000 セクタを流用する。 */
#define SMALL_SECTORS 21000
        diskfs_dev sdev = make_dev();
        sdev.total_sectors = SMALL_SECTORS;
        check(diskfs_format(&sdev) == 0,
              "format small disk succeeds");
        static uint8_t med[300 * 1024];
        memset(med, 'm', sizeof(med));
        check(diskfs_write_file(&sdev, "med.bin", med,
                                sizeof(med)) == -1,
              "300KB file rejected when the big area is tiny");
        check(diskfs_write_file(&sdev, "tiny.txt",
                                (const uint8_t *)"t", 1) == 0,
              "small file still works on a small disk");
    }

    printf("diskfs tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
