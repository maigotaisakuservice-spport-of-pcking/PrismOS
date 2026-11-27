/* PrismOS — ディスク形式とアカウントのホストテスト
 *
 * カーネルと同じ shell/diskfs.c と kernel/account.c を
 * ホストで動かす。セクタの読み書きはメモリ上の仮想ディスク
 * に向け、QEMU は不要。`make test` で走る。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskfs.h"
#include "account.h"
#include "crypto.h"

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

/* 仮想ディスク: 11000 セクタ (約 5.6MB。スナップショット領域を含む)。 */
#define VDISK_SECTORS 21000
static uint8_t vdisk[VDISK_SECTORS][DISKFS_SECTOR_SIZE];

/* -I kernel があると <string.h> がカーネルの string.h に
 * なるため、memcpy / memcmp は使わず自前のループで書く。 */
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

/* 暗号化のテスト用フック: 本物の AES-256-CTR を使い、
 * IV は書くたびに増えるカウンタから作る。 */
static uint8_t test_key[32];
static uint64_t test_iv_counter;

static int test_encrypt(void *ctx, uint8_t *iv_out, const uint8_t *in,
                        uint8_t *out, size_t len) {
    (void)ctx;
    for (int i = 0; i < 16; i++) {
        iv_out[i] = 0;
    }
    test_iv_counter++;
    for (int i = 0; i < 8; i++) {
        iv_out[15 - i] = (uint8_t)((test_iv_counter >> (i * 8)) & 0xFF);
    }
    crypto_aes256_ctr(test_key, iv_out, in, out, len);
    return 0;
}

static int test_decrypt(void *ctx, const uint8_t *iv, const uint8_t *in,
                        uint8_t *out, size_t len) {
    (void)ctx;
    crypto_aes256_ctr(test_key, iv, in, out, len);
    return 0;
}

static const diskfs_crypto test_crypto = { NULL, test_encrypt, test_decrypt };

static uint32_t fake_now;
static uint32_t fake_time(void) {
    return fake_now;
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int main(void) {
    diskfs_dev dev = make_dev();
    memset(vdisk, 0, sizeof(vdisk));

    /* 初期化前は形式が正しくない。 */
    CHECK(diskfs_is_formatted(&dev) == 0);

    /* フォーマット: MBR とスーパーブロックを確認する。 */
    CHECK(diskfs_format(&dev) == 0);
    CHECK(diskfs_is_formatted(&dev) == 1);
    uint8_t mbr[DISKFS_SECTOR_SIZE];
    CHECK(vdisk_read(NULL, 0, mbr) == 0);
    CHECK(mbr[510] == 0x55 && mbr[511] == 0xAA);
    CHECK(mbr[446 + 4] == DISKFS_PART_TYPE);
    /* パーティション開始 LBA = 1、大きさ = 総数 - 1。 */
    uint32_t p_start = (uint32_t)mbr[446 + 8] |
                       ((uint32_t)mbr[446 + 9] << 8) |
                       ((uint32_t)mbr[446 + 10] << 16) |
                       ((uint32_t)mbr[446 + 11] << 24);
    uint32_t p_size = (uint32_t)mbr[446 + 12] |
                      ((uint32_t)mbr[446 + 13] << 8) |
                      ((uint32_t)mbr[446 + 14] << 16) |
                      ((uint32_t)mbr[446 + 15] << 24);
    CHECK(p_start == 1);
    CHECK(p_size == VDISK_SECTORS - 1);
    uint8_t sb[DISKFS_SECTOR_SIZE];
    CHECK(vdisk_read(NULL, 1, sb) == 0);
    CHECK(bytes_eq(sb, (const uint8_t *)"PRISMFS2", 8));
    /* 版5 で書かれる: 版番号 5、データ開始 20。 */
    {
        uint32_t ver = (uint32_t)sb[8] | ((uint32_t)sb[9] << 8) |
                       ((uint32_t)sb[10] << 16) |
                       ((uint32_t)sb[11] << 24);
        uint32_t dsec = (uint32_t)sb[16] | ((uint32_t)sb[17] << 8) |
                        ((uint32_t)sb[18] << 16) |
                        ((uint32_t)sb[19] << 24);
        CHECK(ver == 5);
        CHECK(dsec == 20);
    }

    /* 空の表。 */
    diskfs_entry entries[DISKFS_MAX_FILES];
    int count = -1;
    CHECK(diskfs_list(&dev, entries, &count) == 0);
    CHECK(count == 0);

    /* 書き込みと読み取り。 */
    const char *hello = "hello prism\n";
    CHECK(diskfs_write_file(&dev, "note.txt", (const uint8_t *)hello,
                            strlen(hello)) == 0);
    CHECK(diskfs_list(&dev, entries, &count) == 0);
    CHECK(count == 1);
    CHECK(strcmp(entries[0].name, "note.txt") == 0);
    CHECK(entries[0].size == strlen(hello));
    static uint8_t buf[DISKFS_MAX_FILE_SIZE];
    size_t len = 0;
    CHECK(diskfs_read_file(&dev, "note.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == strlen(hello));
    CHECK(bytes_eq(buf, (const uint8_t *)hello, len));

    /* 上書き。 */
    const char *longer = "a longer text for overwrite\n";
    CHECK(diskfs_write_file(&dev, "note.txt", (const uint8_t *)longer,
                            strlen(longer)) == 0);
    CHECK(diskfs_read_file(&dev, "note.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == strlen(longer));
    CHECK(bytes_eq(buf, (const uint8_t *)longer, len));

    /* 空ファイルと、無いファイル。 */
    CHECK(diskfs_write_file(&dev, "empty.txt", (const uint8_t *)"", 0) == 0);
    CHECK(diskfs_read_file(&dev, "empty.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == 0);
    CHECK(diskfs_read_file(&dev, "nope.txt", buf, sizeof(buf), &len) == -1);

    /* 版4: 時刻 (源を設定) とチェックサム照合。 */
    diskfs_set_time_source(fake_time);
    fake_now = 1700000000u;
    CHECK(diskfs_write_file(&dev, "stamp.txt",
                            (const uint8_t *)"timed", 5) == 0);
    {
        diskfs_entry es[DISKFS_MAX_FILES];
        int cnt = 0;
        CHECK(diskfs_list(&dev, es, &cnt) == 0);
        int found = 0;
        for (int i = 0; i < cnt; i++) {
            if (strcmp(es[i].name, "stamp.txt") == 0) {
                found = 1;
                CHECK(es[i].ctime == 1700000000u);
                CHECK(es[i].mtime == 1700000000u);
                CHECK(es[i].checksum != 0);
            }
        }
        CHECK(found);
    }
    fake_now = 1700001000u;
    CHECK(diskfs_write_file(&dev, "stamp.txt",
                            (const uint8_t *)"timed2", 6) == 0);
    {
        diskfs_entry es[DISKFS_MAX_FILES];
        int cnt = 0;
        CHECK(diskfs_list(&dev, es, &cnt) == 0);
        for (int i = 0; i < cnt; i++) {
            if (strcmp(es[i].name, "stamp.txt") == 0) {
                CHECK(es[i].ctime == 1700000000u);
                CHECK(es[i].mtime == 1700001000u);
            }
        }
    }
    /* データセクタを直接壊すと読み取りが -6 で失敗する。 */
    {
        diskfs_entry es[DISKFS_MAX_FILES];
        int cnt = 0;
        CHECK(diskfs_list(&dev, es, &cnt) == 0);
        uint32_t start = 0;
        for (int i = 0; i < cnt; i++) {
            if (strcmp(es[i].name, "stamp.txt") == 0) {
                start = es[i].start_sector;
            }
        }
        CHECK(start != 0);
        uint8_t sec[DISKFS_SECTOR_SIZE];
        CHECK(vdisk_read(NULL, start, sec) == 0);
        sec[0] ^= 0xFF;
        CHECK(vdisk_write(NULL, start, sec) == 0);
        CHECK(diskfs_read_file(&dev, "stamp.txt", buf, sizeof(buf),
                               &len) == -6);
        CHECK(diskfs_write_file(&dev, "stamp.txt",
                                (const uint8_t *)"timed2", 6) == 0);
        CHECK(diskfs_read_file(&dev, "stamp.txt", buf, sizeof(buf),
                               &len) == 0);
    }
    diskfs_set_time_source(NULL);
    CHECK(diskfs_delete_file(&dev, "stamp.txt") == 0);
    CHECK(diskfs_delete_file(&dev, "nope.txt") == -1);

    /* 削除。 */
    CHECK(diskfs_delete_file(&dev, "empty.txt") == 0);
    CHECK(diskfs_read_file(&dev, "empty.txt", buf, sizeof(buf), &len) == -1);
    CHECK(diskfs_list(&dev, entries, &count) == 0);
    CHECK(count == 1);

    /* 規則違反: 大きすぎる・名前が不正。 */
    static uint8_t big[DISKFS_MAX_FILE_SIZE + 1];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = 'x';
    }
    CHECK(diskfs_write_file(&dev, "big.txt", big, sizeof(big)) == -1);
    CHECK(diskfs_write_file(&dev, "a//b", (const uint8_t *)"x", 1) == -3);
    CHECK(diskfs_write_file(&dev, "", (const uint8_t *)"x", 1) == -3);
    CHECK(diskfs_write_file(&dev, ".", (const uint8_t *)"x", 1) == -3);

    /* 件数の上限: 残りの枠を埋める。 */
    for (int i = 0; i < DISKFS_MAX_FILES - 1; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%02d.txt", i);
        CHECK(diskfs_write_file(&dev, name, (const uint8_t *)"x", 1) == 0);
    }
    CHECK(diskfs_list(&dev, entries, &count) == 0);
    CHECK(count == DISKFS_MAX_FILES);
    CHECK(diskfs_write_file(&dev, "one-more.txt",
                            (const uint8_t *)"x", 1) == -2);
    /* 1 件空けて、再起動後の読み取り (同じ仮想ディスクを
     * 使い続ける = 電源を切って入れ直したのと同じ) を確認する。 */
    CHECK(diskfs_delete_file(&dev, "f00.txt") == 0);
    /* サブディレクトリ名 (a/b) は正規に書ける。 */
    CHECK(diskfs_write_file(&dev, "a/b", (const uint8_t *)"x", 1) == 0);
    CHECK(diskfs_delete_file(&dev, "a/b") == 0);
    CHECK(diskfs_read_file(&dev, "note.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == strlen(longer));
    CHECK(bytes_eq(buf, (const uint8_t *)longer, len));

    /* フォーマットし直すと消える。 */
    CHECK(diskfs_format(&dev) == 0);
    CHECK(diskfs_list(&dev, entries, &count) == 0);
    CHECK(count == 0);

    /* パーティションの検出。 */
    uint8_t mbr2[DISKFS_SECTOR_SIZE];
    uint32_t psize = 0;
    memset(mbr2, 0, sizeof(mbr2));
    CHECK(diskfs_find_data_partition(mbr2, &psize) == 0); /* 署名なし */
    mbr2[510] = 0x55;
    mbr2[511] = 0xAA;
    CHECK(diskfs_find_data_partition(mbr2, &psize) == 0); /* 項目なし */
    mbr2[446 + 4] = DISKFS_PART_TYPE;
    wr32le(mbr2 + 446 + 8, 1);
    wr32le(mbr2 + 446 + 12, VDISK_SECTORS - 1);
    CHECK(diskfs_find_data_partition(mbr2, &psize) == 0); /* 従来の配置 */
    /* 起動用パーティション + データ用パーティション。 */
    memset(mbr2, 0, sizeof(mbr2));
    mbr2[510] = 0x55;
    mbr2[511] = 0xAA;
    mbr2[446 + 0] = 0x80; /* 起動可 */
    mbr2[446 + 4] = 0x0E; /* FAT16 */
    wr32le(mbr2 + 446 + 8, 2048);
    wr32le(mbr2 + 446 + 12, 16384);
    mbr2[462 + 4] = DISKFS_PART_TYPE;
    wr32le(mbr2 + 462 + 8, 18432);
    wr32le(mbr2 + 462 + 12, 47104);
    CHECK(diskfs_find_data_partition(mbr2, &psize) == 18432);
    CHECK(psize == 47104);
    CHECK(diskfs_find_data_partition(NULL, &psize) == 0);

    /* base_lba 付きのフォーマット: MBR を書かず、パーティションの
     * 中だけを初期化する。 */
    memset(vdisk, 0, sizeof(vdisk));
    diskfs_dev dev2 = make_dev();
    dev2.base_lba = 1024;
    dev2.total_sectors = 8192; /* 版3 のデータ領域 (4114) 以上 */
    CHECK(diskfs_format(&dev2) == 0);
    CHECK(diskfs_is_formatted(&dev2) == 1);
    uint8_t zero[DISKFS_SECTOR_SIZE];
    CHECK(vdisk_read(NULL, 0, zero) == 0);
    int allzero = 1;
    for (int i = 0; i < DISKFS_SECTOR_SIZE; i++) {
        if (zero[i] != 0) {
            allzero = 0;
        }
    }
    CHECK(allzero); /* セクタ 0 (MBR) は触らない */
    CHECK(vdisk_read(NULL, 1025, sb) == 0);
    CHECK(bytes_eq(sb, (const uint8_t *)"PRISMFS2", 8));
    CHECK(diskfs_write_file(&dev2, "part.txt",
                            (const uint8_t *)"in partition\n", 13) == 0);
    CHECK(diskfs_read_file(&dev2, "part.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == 13);
    CHECK(bytes_eq(buf, (const uint8_t *)"in partition\n", len));
    CHECK(diskfs_list(&dev2, entries, &count) == 0);
    CHECK(count == 1);
    CHECK(entries[0].start_sector == 1024 + 20);
    /* base 0 の dev からはパーティションの中身は見えない。 */
    CHECK(diskfs_read_file(&dev, "part.txt", buf, sizeof(buf), &len) == -1);
    CHECK(diskfs_delete_file(&dev2, "part.txt") == 0);
    CHECK(diskfs_list(&dev2, entries, &count) == 0);
    CHECK(count == 0);

    /* 暗号化: 鍵は PIN から作る。 */
    memset(vdisk, 0, sizeof(vdisk));
    uint8_t csalt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    crypto_derive_key("1234", csalt, test_key);
    diskfs_dev dev3 = make_dev();
    dev3.crypto = &test_crypto;
    CHECK(diskfs_format(&dev3) == 0);
    const char *secret = "top secret message";
    CHECK(diskfs_write_file(&dev3, "secret.txt",
                            (const uint8_t *)secret, strlen(secret)) == 0);
    CHECK(diskfs_list(&dev3, entries, &count) == 0);
    CHECK(count == 1);
    CHECK(entries[0].encrypted == 1);
    int iv_nonzero = 0;
    for (int i = 0; i < 16; i++) {
        if (entries[0].iv[i] != 0) {
            iv_nonzero = 1;
        }
    }
    CHECK(iv_nonzero);
    uint8_t iv_first[16];
    copy_bytes(iv_first, entries[0].iv, 16);
    /* 暗号文のまま読むと平文と違う。 */
    CHECK(diskfs_read_file_raw(&dev3, "secret.txt", buf, sizeof(buf),
                               &len) == 0);
    CHECK(len == strlen(secret));
    CHECK(!bytes_eq(buf, (const uint8_t *)secret, len));
    /* 鍵付きで読むと平文に戻る。 */
    CHECK(diskfs_read_file(&dev3, "secret.txt", buf, sizeof(buf), &len) == 0);
    CHECK(len == strlen(secret));
    CHECK(bytes_eq(buf, (const uint8_t *)secret, len));
    /* 鍵なしでは読めない。 */
    CHECK(diskfs_read_file(&dev, "secret.txt", buf, sizeof(buf), &len) ==
          -1);
    /* 上書きすると IV が新しくなる (同じ鍵でも暗号文が変わる)。 */
    const char *secret2 = "a different secret";
    CHECK(diskfs_write_file(&dev3, "secret.txt",
                            (const uint8_t *)secret2, strlen(secret2)) == 0);
    CHECK(diskfs_list(&dev3, entries, &count) == 0);
    CHECK(!bytes_eq(entries[0].iv, iv_first, 16));
    CHECK(diskfs_read_file(&dev3, "secret.txt", buf, sizeof(buf), &len) == 0);
    CHECK(bytes_eq(buf, (const uint8_t *)secret2, len));
    /* 平文で書いた古いファイルは、鍵付きの dev でも読める。 */
    CHECK(diskfs_write_file(&dev, "plain.txt", (const uint8_t *)"plain\n",
                            6) == 0);
    CHECK(diskfs_list(&dev3, entries, &count) == 0);
    CHECK(count == 2);
    CHECK(diskfs_read_file(&dev3, "plain.txt", buf, sizeof(buf), &len) == 0);
    CHECK(bytes_eq(buf, (const uint8_t *)"plain\n", len));
    /* 空ファイルも暗号化の印が付くが、読むと空。 */
    CHECK(diskfs_write_file(&dev3, "empty2.txt", (const uint8_t *)"", 0) ==
          0);
    CHECK(diskfs_read_file(&dev3, "empty2.txt", buf, sizeof(buf), &len) ==
          0);
    CHECK(len == 0);

    /* アカウント: PIN の規則。 */
    CHECK(account_pin_ok("1234") == 1);
    CHECK(account_pin_ok("abc") == 0); /* 短い */
    CHECK(account_pin_ok("12345678901234567") == 0); /* 長い */
    CHECK(account_pin_ok("12 4") == 0); /* 空白 */
    CHECK(account_pin_ok("") == 0);

    /* ハッシュ: 同じソルトと PIN は同じハッシュ、違う PIN は違う。 */
    uint8_t salt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t salt2[8] = { 8, 7, 6, 5, 4, 3, 2, 1 };
    CHECK(account_hash(salt, "1234") == account_hash(salt, "1234"));
    CHECK(account_hash(salt, "1234") != account_hash(salt, "1235"));
    CHECK(account_hash(salt, "1234") != account_hash(salt2, "1234"));

    /* 作って照合する (v2: 16 バイトソルト + 反復ハッシュ)。 */
    char text[PRISM_ACCOUNT_TEXT_MAX];
    uint8_t salt16[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                           9, 10, 11, 12, 13, 14, 15, 16 };
    CHECK(account_make_text(salt16, 10000, "1234", text,
                            sizeof(text)) == 0);
    CHECK(account_version(text, strlen(text)) == 2);
    CHECK(account_verify("1234", text, strlen(text)) == 1);
    CHECK(account_verify("9999", text, strlen(text)) == 0);
    CHECK(account_verify("1234", "garbage", 7) == -1);
    CHECK(account_verify("1234", "v1\nsalt=zz\nhash=00\n", 18) == -1);
    CHECK(account_make_text(salt16, 10000, "abc", text,
                            sizeof(text)) == -1);
    CHECK(account_make_text(salt16, 0, "1234", text, sizeof(text)) ==
          -1);

    if (failures == 0) {
        printf("persist tests: %d checks, 0 failures\n", checks);
        printf("PASS\n");
    } else {
        printf("persist tests: %d checks, %d failures\n", checks, failures);
        printf("FAIL\n");
    }
    return failures == 0 ? 0 : 1;
}
