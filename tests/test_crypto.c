/* PrismOS — 暗号のホストテスト (SHA-256 と AES-256-CTR)。
 * 期待値は OpenSSL で確かめたもの (FIPS-197 の AES-256 例を含む)。 */
#include <stdio.h>
#include <stdint.h>

#include "crypto.h"

/* -I kernel があると <string.h> がカーネルのものになるため、
 * memcmp は使わず自前で比べる。 */
static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

static int hex_eq(const uint8_t *got, const char *want_hex, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned int hi = 0;
        char c = want_hex[i * 2];
        if (c >= '0' && c <= '9') {
            hi = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            hi = (unsigned int)(c - 'a' + 10);
        } else {
            return 0;
        }
        unsigned int lo = 0;
        c = want_hex[i * 2 + 1];
        if (c >= '0' && c <= '9') {
            lo = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            lo = (unsigned int)(c - 'a' + 10);
        } else {
            return 0;
        }
        if (got[i] != (uint8_t)((hi << 4) | lo)) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    uint8_t out[64];
    uint8_t key[32];
    uint8_t iv[16];
    uint8_t pt[32];
    uint8_t ct[32];

    /* SHA-256: 空文字、"abc"、56 文字の例。 */
    crypto_sha256((const uint8_t *)"", 0, out);
    check(hex_eq(out,
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 32),
          "sha256 empty");
    crypto_sha256((const uint8_t *)"abc", 3, out);
    check(hex_eq(out,
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 32),
          "sha256 abc");
    crypto_sha256((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljkl"
                  "mklmnlmnomnopnopq",
                  56, out);
    check(hex_eq(out,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                 32),
          "sha256 56 chars");

    /* SHA-256 ストリーミング: 分割して渡しても一括と同じ値になる。 */
    {
        uint8_t one[32];
        uint8_t streamed[32];
        crypto_sha256_ctx ctx;
        /* 空文字。 */
        crypto_sha256((const uint8_t *)"", 0, one);
        crypto_sha256_init(&ctx);
        crypto_sha256_final(&ctx, streamed);
        check(bytes_eq(one, streamed, 32), "sha256 stream empty");
        /* "abc" を 1 バイトずつ。 */
        crypto_sha256((const uint8_t *)"abc", 3, one);
        crypto_sha256_init(&ctx);
        crypto_sha256_update(&ctx, (const uint8_t *)"a", 1);
        crypto_sha256_update(&ctx, (const uint8_t *)"b", 1);
        crypto_sha256_update(&ctx, (const uint8_t *)"c", 1);
        crypto_sha256_final(&ctx, streamed);
        check(bytes_eq(one, streamed, 32), "sha256 stream abc 1 byte each");
        /* 56 文字を 7 バイトずつ (ブロック境界の手前で切れる)。 */
        static const uint8_t msg56[] =
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        crypto_sha256(msg56, 56, one);
        crypto_sha256_init(&ctx);
        for (int i = 0; i < 56; i += 7) {
            crypto_sha256_update(&ctx, msg56 + i, 7);
        }
        crypto_sha256_final(&ctx, streamed);
        check(bytes_eq(one, streamed, 32), "sha256 stream 56 in 7s");
        /* 200 バイトを 13 バイトずつ (ブロックをまたぐ)。 */
        static uint8_t msg200[200];
        for (int i = 0; i < 200; i++) {
            msg200[i] = (uint8_t)(i * 5 + 1);
        }
        crypto_sha256(msg200, 200, one);
        crypto_sha256_init(&ctx);
        size_t off = 0;
        while (off < 200) {
            size_t n = 200 - off;
            if (n > 13) {
                n = 13;
            }
            crypto_sha256_update(&ctx, msg200 + off, n);
            off += n;
        }
        crypto_sha256_final(&ctx, streamed);
        check(bytes_eq(one, streamed, 32), "sha256 stream 200 in 13s");
        /* 64 バイトちょうどを 1 回で (ブロック境界ぴったり)。 */
        crypto_sha256(msg200, 64, one);
        crypto_sha256_init(&ctx);
        crypto_sha256_update(&ctx, msg200, 64);
        crypto_sha256_final(&ctx, streamed);
        check(bytes_eq(one, streamed, 32), "sha256 stream exact block");
    }

    /* 鍵の導出: SHA-256(PIN || salt)。 */
    uint8_t salt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    crypto_derive_key("1234", salt, key);
    check(hex_eq(key,
                 "38f89fdfe2f6bd21933b69f07e50f9aa6ca03577226be90d052e161a105a8df9",
                 32),
          "derive key 1234");
    crypto_derive_key("9999", salt, key);
    check(hex_eq(key,
                 "52b28e1424213b87b42a6176f663903b23083adb1630ce51ca2b597ddf032991",
                 32),
          "derive key 9999 differs");

    /* KDF: 反復ハッシュ。1 回は SHA-256(salt || PIN) と同じ。 */
    {
        uint8_t salt16[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                               9, 10, 11, 12, 13, 14, 15, 16 };
        uint8_t k1[32];
        uint8_t k2[32];
        uint8_t k3[32];
        crypto_kdf("1234", salt16, 16, 10000, k1);
        crypto_kdf("1234", salt16, 16, 10000, k2);
        check(bytes_eq(k1, k2, 32), "kdf deterministic");
        crypto_kdf("1235", salt16, 16, 10000, k2);
        check(!bytes_eq(k1, k2, 32), "kdf PIN differs");
        crypto_kdf("1234", salt16, 16, 1, k2);
        check(!bytes_eq(k1, k2, 32), "kdf iterations differ");
        uint8_t buf[20];
        for (int i = 0; i < 16; i++) {
            buf[i] = salt16[i];
        }
        buf[16] = '1';
        buf[17] = '2';
        buf[18] = '3';
        buf[19] = '4';
        crypto_sha256(buf, 20, k3);
        check(bytes_eq(k2, k3, 32), "kdf 1 round = sha256(salt||pin)");
        crypto_kdf("1234", salt16, 16, 0, k3);
        check(bytes_eq(k2, k3, 32), "kdf 0 rounds = 1 round");
        /* 既知答 (Python hashlib で独立計算): ストリーミング
         * 化しても既存 PIN の導出結果が変わらないこと。 */
        check(hex_eq(k1,
                     "823dd6d512b552d2f8e6ce810bca48dc670ae43417"
                     "c8a0aee0ad0e59af53c084",
                     32),
              "kdf known answer (10000 rounds)");
        /* 長いパスフレーズ (29 文字): 旧実装は固定バッファを
         * 溢れてスタックを壊していた (FDE リカバリキーで
         * 発現)。既知答と一致し、決定的であること。 */
        {
            uint8_t salt_a5[16];
            for (int i = 0; i < 16; i++) {
                salt_a5[i] = 0xA5;
            }
            uint8_t lk1[32], lk2[32];
            crypto_kdf("correct horse battery staple", salt_a5,
                       16, 100, lk1);
            crypto_kdf("correct horse battery staple", salt_a5,
                       16, 100, lk2);
            check(bytes_eq(lk1, lk2, 32), "kdf long passphrase stable");
            check(hex_eq(lk1,
                         "826d3c9a1782efd2d304addd5bac751a89e4cab2"
                         "6c31b8a65f57bbfa37f9568d",
                         32),
                  "kdf long passphrase known answer");
        }
    }

    /* AES-256-CTR: 1 ブロック (FIPS-197 の鍵と平文)。 */
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }
    for (int i = 0; i < 16; i++) {
        iv[i] = (uint8_t)i;
        pt[i] = (uint8_t)(i * 0x11);
    }
    crypto_aes256_ctr(key, iv, pt, ct, 16);
    check(hex_eq(ct, "5a7f26644cae17e178b7ff86ce1e486d", 16),
          "aes-256-ctr 1 block");
    /* 復号は同じ関数で戻る。 */
    crypto_aes256_ctr(key, iv, ct, out, 16);
    check(bytes_eq(out, pt, 16), "aes-256-ctr decrypt round-trip");

    /* 2 ブロック (カウンタが進むことの確認)。 */
    for (int i = 0; i < 32; i++) {
        pt[i] = (uint8_t)i;
    }
    crypto_aes256_ctr(key, iv, pt, ct, 32);
    check(hex_eq(ct,
                 "5a6f06540cfe7791f8275f360ecea89d70e202c6d7904e4a4d0fe14a6ef83ed0",
                 32),
          "aes-256-ctr 2 blocks");
    crypto_aes256_ctr(key, iv, ct, out, 32);
    check(bytes_eq(out, pt, 32), "aes-256-ctr 2-block round-trip");

    /* ブロック境界でない長さ (5 バイト) も往復する。 */
    const char *msg = "hello";
    crypto_aes256_ctr(key, iv, (const uint8_t *)msg, ct, 5);
    crypto_aes256_ctr(key, iv, ct, out, 5);
    check(bytes_eq(out, (const uint8_t *)msg, 5), "aes-256-ctr short round-trip");

    /* 長さ 0 は何もしない (壊れない)。 */
    crypto_aes256_ctr(key, iv, pt, ct, 0);
    check(1, "aes-256-ctr zero length");

    printf("crypto tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
