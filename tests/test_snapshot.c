/* PrismOS — 復元ポイント (スナップショット) と回復フラグのホストテスト
 *
 * カーネルと同じ shell/diskfs.c をホストで動かす。セクタの
 * 読み書きはメモリ上の仮想ディスクに向け、QEMU は不要。
 * `make test` で走る。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskfs.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

/* 仮想ディスク: 21000 セクタ (版3 のスナップショット領域を含む)。 */
#define VDISK_SECTORS 21000
static uint8_t vdisk[VDISK_SECTORS][DISKFS_SECTOR_SIZE];

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

static int vdisk_read(void *ctx, uint32_t lba, uint8_t *buf) {
    (void)ctx;
    if (lba >= VDISK_SECTORS) {
        return -1;
    }
    copy_bytes(buf, vdisk[lba], DISKFS_SECTOR_SIZE);
    return 0;
}

static int vdisk_write(void *ctx, uint32_t lba, const uint8_t *buf) {
    (void)ctx;
    if (lba >= VDISK_SECTORS) {
        return -1;
    }
    copy_bytes(vdisk[lba], buf, DISKFS_SECTOR_SIZE);
    return 0;
}

static diskfs_dev make_dev(void) {
    diskfs_dev dev;
    dev.ctx = NULL;
    dev.read_sector = vdisk_read;
    dev.write_sector = vdisk_write;
    dev.total_sectors = VDISK_SECTORS;
    dev.base_lba = 0;
    dev.crypto = NULL;
    return dev;
}

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(void) {
    diskfs_dev dev = make_dev();
    memset(vdisk, 0, sizeof(vdisk));

    /* 未フォーマットの盤では作れない。 */
    CHECK(diskfs_snapshot_create(&dev, "snap1", "2026-11-21 10:00:00") != 0);
    CHECK(diskfs_format(&dev) == 0);

    /* 回復フラグは初期状態で 0。 */
    CHECK(diskfs_get_recovery_flag(&dev) == 0);
    CHECK(diskfs_set_recovery_flag(&dev, 1) == 0);
    CHECK(diskfs_get_recovery_flag(&dev) == 1);
    CHECK(diskfs_set_recovery_flag(&dev, 0) == 0);
    CHECK(diskfs_get_recovery_flag(&dev) == 0);
    /* フォーマットし直すと印は消える。 */
    CHECK(diskfs_set_recovery_flag(&dev, 1) == 0);
    CHECK(diskfs_format(&dev) == 0);
    CHECK(diskfs_get_recovery_flag(&dev) == 0);

    /* ファイルを 2 件書く。 */
    static const uint8_t hello[] = "hello snapshot";
    static const uint8_t world[] = "world data 123";
    CHECK(diskfs_write_file(&dev, "a.txt", hello, sizeof(hello) - 1) == 0);
    CHECK(diskfs_write_file(&dev, "b.txt", world, sizeof(world) - 1) == 0);

    /* 復元ポイントを作る。 */
    CHECK(diskfs_snapshot_create(&dev, "snap1", "2026-11-21 10:00:00") == 0);
    diskfs_snapshot_info infos[DISKFS_SNAP_SLOTS];
    int count = -1;
    CHECK(diskfs_snapshot_list(&dev, infos, DISKFS_SNAP_SLOTS, &count) == 0);
    CHECK(count == 1);
    CHECK(str_eq(infos[0].name, "snap1"));
    CHECK(str_eq(infos[0].timestamp, "2026-11-21 10:00:00"));
    CHECK(infos[0].file_count == 2);

    /* ファイルを変える: a.txt を上書き、b.txt を消し、c.txt を作る。 */
    static const uint8_t changed[] = "changed content";
    CHECK(diskfs_write_file(&dev, "a.txt", changed, sizeof(changed) - 1) == 0);
    CHECK(diskfs_delete_file(&dev, "b.txt") == 0);
    CHECK(diskfs_write_file(&dev, "c.txt", hello, sizeof(hello) - 1) == 0);

    /* 復元すると、スナップショット時点に戻る。 */
    CHECK(diskfs_snapshot_restore(&dev, "snap1") == 0);
    uint8_t buf[64];
    size_t len = 0;
    CHECK(diskfs_read_file(&dev, "a.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == sizeof(hello) - 1);
    CHECK(bytes_eq(buf, hello, len));
    CHECK(diskfs_read_file(&dev, "b.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == sizeof(world) - 1);
    CHECK(bytes_eq(buf, world, len));
    CHECK(diskfs_read_file(&dev, "c.txt", buf, sizeof(buf), &len) != 0);

    /* 無い名前の復元は失敗する。 */
    CHECK(diskfs_snapshot_restore(&dev, "no-such") == -1);
    CHECK(diskfs_snapshot_delete(&dev, "no-such") == -1);

    /* 同じ名前で作り直すと上書きされる (件数は増えない)。 */
    CHECK(diskfs_snapshot_create(&dev, "snap1", "2026-11-21 11:00:00") == 0);
    count = -1;
    CHECK(diskfs_snapshot_list(&dev, infos, DISKFS_SNAP_SLOTS, &count) == 0);
    CHECK(count == 1);
    CHECK(str_eq(infos[0].timestamp, "2026-11-21 11:00:00"));

    /* 4 スロットを使い切ると、5 件目はいちばん古いものを上書きする。 */
    CHECK(diskfs_snapshot_create(&dev, "s2", "2026-11-21 11:01:00") == 0);
    CHECK(diskfs_snapshot_create(&dev, "s3", "2026-11-21 11:02:00") == 0);
    CHECK(diskfs_snapshot_create(&dev, "s4", "2026-11-21 11:03:00") == 0);
    count = -1;
    CHECK(diskfs_snapshot_list(&dev, infos, DISKFS_SNAP_SLOTS, &count) == 0);
    CHECK(count == 4);
    CHECK(diskfs_snapshot_create(&dev, "s5", "2026-11-21 11:04:00") == 0);
    count = -1;
    CHECK(diskfs_snapshot_list(&dev, infos, DISKFS_SNAP_SLOTS, &count) == 0);
    CHECK(count == 4);
    /* snap1 (いちばん古い) が消え、s5 が入っている。 */
    CHECK(diskfs_snapshot_restore(&dev, "snap1") == -1);
    CHECK(diskfs_snapshot_restore(&dev, "s5") == 0);

    /* 削除すると一覧から消える。 */
    CHECK(diskfs_snapshot_delete(&dev, "s5") == 0);
    count = -1;
    CHECK(diskfs_snapshot_list(&dev, infos, DISKFS_SNAP_SLOTS, &count) == 0);
    CHECK(count == 3);

    /* ヘッダを壊すと、そのスロットは一覧に出ない。
     * (スロット 1 = s2 のヘッダを壊す。スロット 0 は s5 の
     * 削除ですでに空き。) */
    memset(vdisk[DISKFS_SNAP_BASE + DISKFS_SNAP_SLOT_SECTORS], 0,
           DISKFS_SECTOR_SIZE);
    count = -1;
    CHECK(diskfs_snapshot_list(&dev, infos, DISKFS_SNAP_SLOTS, &count) == 0);
    CHECK(count == 2);

    /* 領域が足りない盤では作れない。 */
    diskfs_dev small = make_dev();
    small.total_sectors = 100;
    CHECK(diskfs_snapshot_create(&small, "x", "2026-11-21 10:00:00") == -2);

    /* 不正な名前・時刻は断る。 */
    CHECK(diskfs_snapshot_create(&dev, "", "2026-11-21 10:00:00") == -1);
    CHECK(diskfs_snapshot_create(&dev, "ok", "") == -1);
    CHECK(diskfs_snapshot_create(&dev, "a//b", "2026-11-21 10:00:00") == -1);

    /* ---- チェックサムと作成側の検証 (版2) ----
     * 新しい盤で: スナップショットのデータ領域を 1 バイト
     * 壊すと、復元はチェックサム不一致で断られ、現在の
     * ファイルは変わらない。 */
    memset(vdisk, 0, sizeof(vdisk));
    CHECK(diskfs_format(&dev) == 0);
    CHECK(diskfs_write_file(&dev, "keep.txt", hello,
                            sizeof(hello) - 1) == 0);
    CHECK(diskfs_snapshot_create(&dev, "guard", "2026-11-22 09:00:00") == 0);
    /* スロット 0 のデータ領域 (ヘッダ+表の直後) を壊す。 */
    vdisk[DISKFS_SNAP_BASE + 1 + DISKFS_TABLE_SECTORS][0] ^= 0xFF;
    CHECK(diskfs_snapshot_restore(&dev, "guard") == -2);
    len = 0;
    CHECK(diskfs_read_file(&dev, "keep.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == sizeof(hello) - 1);
    CHECK(bytes_eq(buf, hello, len));

    /* ライブ表の start_sector を壊すと、作成側も断る
     * (以前は無検証で領域外を読み書きし得た)。 */
    CHECK(diskfs_snapshot_create(&dev, "guard2",
                                 "2026-11-22 09:01:00") == 0);
    {
        /* a.txt ではなく keep.txt の項目を探して壊す。
         * 表はセクタ 2 から、項目は 64 バイト。 */
        int done = 0;
        for (int i = 0; i < DISKFS_MAX_FILES && !done; i++) {
            /* 表は 8 セクタにまたがるので絶対位置で触る。 */
            uint8_t *e = &vdisk[0][0] +
                         (DISKFS_TABLE_SECTOR * DISKFS_SECTOR_SIZE) +
                         i * 64;
            if (e[40] != 0 && e[0] == 'k') {
                e[36] = 0xFF; /* start_sector を領域外へ */
                e[37] = 0xFF;
                e[38] = 0xFF;
                e[39] = 0x7F;
                done = 1;
            }
        }
        CHECK(done);
    }
    CHECK(diskfs_snapshot_create(&dev, "guard3",
                                 "2026-11-22 09:02:00") == -3);
    /* 壊れた項目があると、そのファイルの読み取りも断る。 */
    len = 0;
    CHECK(diskfs_read_file(&dev, "keep.txt", buf, sizeof(buf), &len) != 0);

    /* ---- 版2 ディスクの互換 ----
     * 版2 の配置 (表 8 セクタ、データ開始 10) で手書きした
     * 盤が、そのまま読み書き・スナップショットできること。 */
    memset(vdisk, 0, sizeof(vdisk));
    {
        uint8_t *mbr = vdisk[0];
        mbr[446 + 4] = 0x83;
        mbr[446 + 8] = 1; /* 開始 LBA 1 */
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        uint8_t *sb = vdisk[1];
        memcpy(sb, "PRISMFS2", 8);
        sb[8] = 2;        /* 版2 */
        sb[12] = 1;       /* ファイル数 */
        sb[16] = 10;      /* データ開始セクタ */
        sb[20] = 32;      /* 1 ファイルのセクタ数 */
        uint8_t *e = vdisk[2]; /* 表の先頭項目 */
        memcpy(e, "legacy.txt", 10);
        e[32] = 9;        /* 大きさ */
        e[36] = 10;       /* 開始セクタ (版2 のデータ領域) */
        e[40] = 1;        /* 使用中 */
        memcpy(vdisk[10], "legacy-v2", 9);
    }
    CHECK(diskfs_is_formatted(&dev) == 1);
    len = 0;
    CHECK(diskfs_read_file(&dev, "legacy.txt", buf, sizeof(buf),
                           &len) == 0);
    CHECK(len == 9 && bytes_eq(buf, (const uint8_t *)"legacy-v2", 9));
    /* 版2 の配置のまま追記できる (2 件目はセクタ 42 から)。 */
    CHECK(diskfs_write_file(&dev, "added.txt", hello,
                            sizeof(hello) - 1) == 0);
    {
        diskfs_entry entries[DISKFS_MAX_FILES];
        int count = 0;
        CHECK(diskfs_list(&dev, entries, &count) == 0);
        CHECK(count == 2);
        int found = 0;
        for (int i = 0; i < count; i++) {
            if (str_eq(entries[i].name, "added.txt")) {
                found = 1;
                CHECK(entries[i].start_sector == 42);
            }
        }
        CHECK(found);
    }
    /* 版2 でもスナップショットの作成と復元ができる。 */
    CHECK(diskfs_snapshot_create(&dev, "v2snap",
                                 "2026-11-22 10:00:00") == 0);
    CHECK(diskfs_delete_file(&dev, "legacy.txt") == 0);
    CHECK(diskfs_snapshot_restore(&dev, "v2snap") == 0);
    len = 0;
    CHECK(diskfs_read_file(&dev, "legacy.txt", buf, sizeof(buf),
                           &len) == 0);
    CHECK(len == 9 && bytes_eq(buf, (const uint8_t *)"legacy-v2", 9));

    if (failures == 0) {
        printf("snapshot tests: %d checks, 0 failures\n", checks);
        printf("PASS\n");
        return 0;
    }
    printf("snapshot tests: %d checks, %d failures\n", checks, failures);
    printf("FAIL\n");
    return 1;
}
