/* PrismOS — スクリーンショット原寸保存のホスト側検査ツール。
 *
 * QEMU で撮った shot.bmp が入った生ディスクイメージ
 * (build/test-disk-shot.img など) を開き、MBR からデータ用
 * パーティションを探して diskfs を読み、shot.bmp が原寸の
 * 24 ビット BMP (1280x800、3072054 バイト) であることを
 * 確かめる。`scripts/screenshot-test.py` から呼ぶ。
 * ログイン後のディスクは PIN から導いた鍵で暗号化されて
 * いるため、平文の .account からソルトと反復回数を読んで
 * 同じ KDF で鍵を作り、復号してから検査する (カーネルと
 * 同じ kernel/crypto.c を使う)。画素は構造だけでなく、
 * 行ごとのばらつき (グラデーション) も見る。旧 144x90
 * 形式なら寸法で失敗する。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "diskfs.h"

#define SHOT_LEN (54 + 1280 * 800 * 3)

static FILE *img = NULL;
static uint32_t base_lba = 0;

static int img_read(void *ctx, uint32_t lba, uint8_t *buf) {
    (void)ctx;
    if (fseek(img, (long)(base_lba + lba) * DISKFS_SECTOR_SIZE,
              SEEK_SET) != 0) {
        return -1;
    }
    return fread(buf, 1, DISKFS_SECTOR_SIZE, img) ==
                   DISKFS_SECTOR_SIZE
               ? 0
               : -1;
}

static int img_write(void *ctx, uint32_t lba, const uint8_t *buf) {
    (void)ctx;
    (void)lba;
    (void)buf;
    return -1; /* 読み取り専用 */
}

static uint8_t disk_key[32];

static int shot_encrypt(void *ctx, uint8_t *iv_out, const uint8_t *in,
                        uint8_t *out, size_t len) {
    (void)ctx;
    (void)iv_out;
    (void)in;
    (void)out;
    (void)len;
    return -1; /* 検査ツールは読むだけ */
}

static int shot_decrypt(void *ctx, const uint8_t *iv,
                        const uint8_t *in, uint8_t *out, size_t len) {
    (void)ctx;
    crypto_aes256_ctr(disk_key, iv, in, out, len);
    return 0;
}

static const diskfs_crypto shot_crypto = {NULL, shot_encrypt,
                                          shot_decrypt};

static int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* .account (v2: "v2\nsalt=<32桁16進>\niter=<回数>\n...") から
 * ソルトと反復回数を読む。失敗したら -1。 */
static int read_account(diskfs_dev *dev, uint8_t *salt_out,
                        uint32_t *iter_out) {
    static uint8_t text[1024];
    size_t len = 0;
    if (diskfs_read_file(dev, ".account", text, sizeof(text) - 1,
                         &len) != 0) {
        return -1;
    }
    text[len] = '\0';
    const char *s = strstr((const char *)text, "salt=");
    const char *it = strstr((const char *)text, "iter=");
    if (s == NULL || it == NULL) {
        return -1;
    }
    s += 5;
    for (int i = 0; i < 16; i++) {
        int hi = hex_val(s[i * 2]);
        int lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        salt_out[i] = (uint8_t)((hi << 4) | lo);
    }
    *iter_out = (uint32_t)strtoul(it + 5, NULL, 10);
    return *iter_out == 0 ? -1 : 0;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: check_shot <disk image> [pin]\n");
        return 2;
    }
    const char *pin = argc == 3 ? argv[2] : "1234";
    img = fopen(argv[1], "rb");
    if (!img) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    uint8_t mbr[DISKFS_SECTOR_SIZE];
    if (fread(mbr, 1, sizeof(mbr), img) != sizeof(mbr)) {
        fprintf(stderr, "cannot read MBR\n");
        return 2;
    }
    uint32_t part_size = 0;
    base_lba = diskfs_find_data_partition(mbr, &part_size);
    /* パーティション表が無い (従来配置) 場合は、イメージ全体が
     * diskfs で、開始 LBA は 0。 */
    uint32_t total_sectors;
    if (part_size == 0) {
        fseek(img, 0, SEEK_END);
        long img_len = ftell(img);
        fseek(img, 0, SEEK_SET);
        total_sectors = (uint32_t)(img_len / DISKFS_SECTOR_SIZE);
        printf("no data partition; whole image is diskfs "
               "(%u sectors)\n",
               total_sectors);
    } else {
        total_sectors = part_size;
        printf("data partition at LBA %u (%u sectors)\n", base_lba,
               part_size);
    }

    diskfs_dev dev;
    memset(&dev, 0, sizeof(dev));
    dev.ctx = NULL;
    dev.read_sector = img_read;
    dev.write_sector = img_write;
    dev.total_sectors = total_sectors;
    dev.base_lba = 0;
    dev.crypto = NULL;

    /* 鍵を導く: .account は平文なので、まず暗号なしで読む。 */
    uint8_t salt[16];
    uint32_t iter = 0;
    if (read_account(&dev, salt, &iter) != 0) {
        fprintf(stderr, "FAIL: cannot read .account salt/iter\n");
        return 1;
    }
    printf(".account: iter=%u\n", iter);
    crypto_kdf(pin, salt, sizeof(salt), iter, disk_key);
    dev.crypto = &shot_crypto;
    printf("ok: disk key derived from PIN\n");

    diskfs_entry entries[DISKFS_MAX_FILES];
    int count = -1;
    if (diskfs_list(&dev, entries, &count) != 0) {
        fprintf(stderr, "diskfs_list failed\n");
        return 1;
    }
    printf("files on disk: %d\n", count);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, "shot.bmp") == 0) {
            found = 1;
            printf("shot.bmp: size=%u start=%u big=%d\n",
                   entries[i].size, entries[i].start_sector,
                   entries[i].big);
        }
    }
    if (!found) {
        fprintf(stderr, "FAIL: shot.bmp not on disk\n");
        return 1;
    }

    static uint8_t buf[SHOT_LEN + 16];
    size_t out_len = 0;
    int rc = diskfs_read_file(&dev, "shot.bmp", buf, sizeof(buf),
                              &out_len);
    if (rc != 0 || out_len != SHOT_LEN) {
        fprintf(stderr,
                "FAIL: shot.bmp read rc=%d len=%zu, want %d\n", rc,
                out_len, SHOT_LEN);
        return 1;
    }
    int n = (int)out_len;
    printf("ok: shot.bmp is %d bytes\n", n);

    int fail = 0;
#define CK(c, what)                                                     \
    do {                                                                \
        if (!(c)) {                                                     \
            fprintf(stderr, "FAIL: %s\n", what);                        \
            fail = 1;                                                   \
        }                                                               \
    } while (0)
    CK(buf[0] == 'B' && buf[1] == 'M', "BM magic");
    CK(rd32(buf + 2) == SHOT_LEN, "bfSize == file size");
    CK(rd32(buf + 10) == 54, "bfOffBits == 54");
    CK(rd32(buf + 14) == 40, "DIB header size == 40");
    CK(rd32(buf + 18) == 1280, "width == 1280");
    CK(rd32(buf + 22) == 800, "height == 800");
    CK(rd16(buf + 26) == 1, "planes == 1");
    CK(rd16(buf + 28) == 24, "bpp == 24");
    CK(rd32(buf + 30) == 0, "compression == BI_RGB");
    /* 画素のばらつき: 上・中・下の行を拾い、値の種類と
     * 行ごとの違いを見る (単色の塗りつぶしなら失敗)。 */
    {
        int seen[256];
        memset(seen, 0, sizeof(seen));
        int distinct = 0;
        uint8_t first_row[32], last_row[32];
        for (int row = 0; row < 800; row += 37) {
            const uint8_t *p = buf + 54 + (size_t)row * 3840;
            for (int i = 0; i < 3840; i += 7) {
                if (!seen[p[i]]) {
                    seen[p[i]] = 1;
                    distinct++;
                }
            }
            if (row == 0) {
                memcpy(first_row, p, sizeof(first_row));
            }
            if (row == 777) {
                memcpy(last_row, p, sizeof(last_row));
            }
        }
        printf("distinct pixel byte values sampled: %d\n", distinct);
        CK(distinct > 50, "pixel data varies (gradient)");
        CK(memcmp(first_row, last_row, sizeof(first_row)) != 0,
           "top and bottom rows differ");
    }
    fclose(img);
    if (fail) {
        return 1;
    }
    printf("SHOT CHECK PASSED\n");
    return 0;
}
