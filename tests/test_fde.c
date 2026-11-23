/* PrismOS — FDE 鍵ラップ (kernel/fde.c) のホストテスト。
 * 包む→開ける往復、違うパスフレーズの拒否、改ざん検出、
 * ヘッダのセクタ往復を確認する。 */
#include <stdio.h>
#include <string.h>

#include "fde.h"

static int checks;
static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            failures++;                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                              \
    } while (0)

int main(void) {
    uint8_t master[32];
    uint8_t salt[16];
    uint8_t iv[16];
    for (int i = 0; i < 32; i++) {
        master[i] = (uint8_t)(i * 7 + 3);
    }
    for (int i = 0; i < 16; i++) {
        salt[i] = (uint8_t)(0xA0 + i);
        iv[i] = (uint8_t)(0x50 + i);
    }

    fde_header hdr;
    CHECK(fde_wrap_key("correct horse", salt, 1000, iv, master,
                       &hdr) == 0);
    /* 包んだ鍵は平文と違う。 */
    CHECK(memcmp(hdr.wrapped, master, 32) != 0);

    uint8_t out[32];
    memset(out, 0, sizeof(out));
    CHECK(fde_unwrap_key("correct horse", &hdr, out) == 0);
    CHECK(memcmp(out, master, 32) == 0);

    /* 違うパスフレーズでは開けない。 */
    memset(out, 0xEE, sizeof(out));
    CHECK(fde_unwrap_key("wrong pass", &hdr, out) == -1);

    /* 改ざん (wrapped の 1 バイト) は MAC で検出する。 */
    {
        fde_header bad = hdr;
        bad.wrapped[5] ^= 0x01;
        CHECK(fde_unwrap_key("correct horse", &bad, out) == -1);
    }
    /* salt の改ざんも検出する (KEK が変わる)。 */
    {
        fde_header bad = hdr;
        bad.salt[0] ^= 0x01;
        CHECK(fde_unwrap_key("correct horse", &bad, out) == -1);
    }

    /* セクタへの往復。 */
    {
        uint8_t sector[512];
        CHECK(fde_header_serialize(&hdr, sector) == 0);
        CHECK(memcmp(sector, "PRISMFK1", 8) == 0);
        fde_header back;
        CHECK(fde_header_parse(sector, &back) == 0);
        CHECK(memcmp(back.wrapped, hdr.wrapped, 32) == 0);
        CHECK(back.iterations == hdr.iterations);
        memset(out, 0, sizeof(out));
        CHECK(fde_unwrap_key("correct horse", &back, out) == 0);
        CHECK(memcmp(out, master, 32) == 0);
        /* magic が無ければ読めない。 */
        sector[0] = 'X';
        CHECK(fde_header_parse(sector, &back) == -1);
    }

    /* 引数の検証。 */
    CHECK(fde_wrap_key(NULL, salt, 1000, iv, master, &hdr) == -1);
    CHECK(fde_wrap_key("x", salt, 0, iv, master, &hdr) == -1);

    /* KDF 反復回数の上限 (監査 F4): 細工・破損ヘッダの
     * 巨大値を parse / unwrap / wrap の全経路で弾く。 */
    CHECK(fde_wrap_key("x", salt, FDE_MAX_ITERATIONS + 1, iv,
                       master, &hdr) == -1);
    {
        fde_header big = hdr;
        big.iterations = 0xFFFFFFFFu;
        CHECK(fde_unwrap_key("correct horse", &big, out) == -1);
        uint8_t sector[512];
        CHECK(fde_header_serialize(&hdr, sector) == 0);
        /* 反復回数フィールド (オフセット 24、LE) を書き換え。 */
        sector[24] = 0xFF;
        sector[25] = 0xFF;
        sector[26] = 0xFF;
        sector[27] = 0xFF;
        fde_header back;
        CHECK(fde_header_parse(sector, &back) == -1);
        sector[24] = 0;
        sector[25] = 0;
        sector[26] = 0;
        sector[27] = 0;
        CHECK(fde_header_parse(sector, &back) == -1);
    }

    /* 長いパスフレーズ (48 文字) と 24 文字のリカバリキー
     * 形式でも往復できる。旧 crypto_kdf はこの長さで
     * スタックバッファを溢れていた (QEMU で #GP を確認)。 */
    {
        fde_header lh;
        uint8_t lout[32];
        const char *longpass =
            "a very long passphrase that exceeds sixteen chars";
        CHECK(fde_wrap_key(longpass, salt, 1000, iv, master,
                           &lh) == 0);
        CHECK(fde_unwrap_key(longpass, &lh, lout) == 0);
        CHECK(memcmp(lout, master, 32) == 0);
        CHECK(fde_unwrap_key("short", &lh, lout) == -1);
        const char *rkey = "ABCDEFGHJKMNPQRSTUVWXY"; /* 24 */
        CHECK(fde_wrap_key(rkey, salt, 1000, iv, master, &lh) == 0);
        CHECK(fde_unwrap_key(rkey, &lh, lout) == 0);
        CHECK(memcmp(lout, master, 32) == 0);
    }

    /* セクタ暗号 (fde_sector_crypt): 往復、LBA で IV が
     * 変わること、鍵違いでは戻らないこと、インプレース。 */
    {
        uint8_t key[32];
        uint8_t plain[512], enc[512], back[512];
        for (int i = 0; i < 32; i++) {
            key[i] = (uint8_t)(0xA0 + i);
        }
        for (int i = 0; i < 512; i++) {
            plain[i] = (uint8_t)(i * 7 + 3);
        }
        fde_sector_crypt(key, 100, plain, enc);
        CHECK(memcmp(enc, plain, 512) != 0);
        fde_sector_crypt(key, 100, enc, back);
        CHECK(memcmp(back, plain, 512) == 0);
        /* 同じ平文でも LBA が違えば暗号文が違う。 */
        uint8_t enc2[512];
        fde_sector_crypt(key, 101, plain, enc2);
        CHECK(memcmp(enc, enc2, 512) != 0);
        /* 同じ LBA・同じ平文は決定的 (IV を保存しない方式)。 */
        uint8_t enc3[512];
        fde_sector_crypt(key, 100, plain, enc3);
        CHECK(memcmp(enc, enc3, 512) == 0);
        /* 鍵が違えば復号できない。 */
        uint8_t key2[32];
        for (int i = 0; i < 32; i++) {
            key2[i] = (uint8_t)(0x50 + i);
        }
        fde_sector_crypt(key2, 100, enc, back);
        CHECK(memcmp(back, plain, 512) != 0);
        /* インプレース (in == out) でも往復できる。 */
        uint8_t buf[512];
        for (int i = 0; i < 512; i++) {
            buf[i] = plain[i];
        }
        fde_sector_crypt(key, 7, buf, buf);
        CHECK(memcmp(buf, plain, 512) != 0);
        fde_sector_crypt(key, 7, buf, buf);
        CHECK(memcmp(buf, plain, 512) == 0);
        /* ゼロ埋めセクタでも鍵ストリームが漏れない
         * (暗号文が全部 0 にはならない)。 */
        uint8_t zeros[512];
        uint8_t zenc[512];
        for (int i = 0; i < 512; i++) {
            zeros[i] = 0;
        }
        fde_sector_crypt(key, 0, zeros, zenc);
        int all_zero = 1;
        for (int i = 0; i < 512; i++) {
            if (zenc[i] != 0) {
                all_zero = 0;
            }
        }
        CHECK(!all_zero);
    }

    printf("test_fde: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
