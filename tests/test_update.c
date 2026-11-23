/* PrismOS — 更新マニフェストの解析とバージョン比較のホストテスト
 *
 * カーネルと同じ shell/update.c と kernel/crypto.c をホストで
 * 動かす。ネットワークは不要。`make test` で走る。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update.h"
#include "pq_update_sig.h"

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

static size_t my_strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(void) {
    /* SHA-256("abc") の既知の値。 */
    char hex[65];
    static const uint8_t abc[] = "abc";
    update_sha256_hex(abc, 3, hex);
    CHECK(str_eq(hex,
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* SHA-256("") の既知の値。 */
    update_sha256_hex(abc, 0, hex);
    CHECK(str_eq(hex,
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    /* 16 進とバイトの往復。 */
    uint8_t bytes[32];
    CHECK(update_hex_to_bytes(
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              bytes) == 0);
    CHECK(bytes[0] == 0xBA && bytes[1] == 0x78 && bytes[31] == 0xAD);
    update_bytes_to_hex(bytes, hex);
    CHECK(str_eq(hex,
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* 壊れた 16 進は断る。 */
    CHECK(update_hex_to_bytes("zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                              bytes) != 0);
    CHECK(update_hex_to_bytes("ba78", bytes) != 0);

    /* バージョンの比較。 */
    CHECK(update_version_cmp("0.2.0", "0.3.0") < 0);
    CHECK(update_version_cmp("0.3.0", "0.2.0") > 0);
    CHECK(update_version_cmp("0.2.0", "0.2.0") == 0);
    CHECK(update_version_cmp("0.10.0", "0.9.0") > 0);
    CHECK(update_version_cmp("0.9.0", "0.10.0") < 0);
    CHECK(update_version_cmp("0.3", "0.3.0") == 0);
    CHECK(update_version_cmp("1.0.0", "0.9.9") > 0);
    /* 接尾辞があるものは、数字が同じなら古い。 */
    CHECK(update_version_cmp("0.3.0-beta", "0.3.0") < 0);
    CHECK(update_version_cmp("0.3.0", "0.3.0-beta") > 0);
    CHECK(update_version_cmp("0.3.1-beta", "0.3.0") > 0);

    /* マニフェストの解析 (正常系)。 */
    static const char manifest[] =
        "version=0.3.0\n"
        "channel=stable\n"
        "type=security\n"
        "base=/updates\n"
        "file README.txt ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
        "file VERSION e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n";
    update_manifest m;
    CHECK(update_parse_manifest(manifest, my_strlen(manifest), &m) == 0);
    CHECK(str_eq(m.version, "0.3.0"));
    CHECK(str_eq(m.channel, "stable"));
    CHECK(str_eq(m.type, "security"));
    CHECK(str_eq(m.base, "/updates"));
    CHECK(m.file_count == 2);
    CHECK(str_eq(m.files[0].name, "README.txt"));
    CHECK(m.files[0].sha256[0] == 0xBA);
    CHECK(str_eq(m.files[1].name, "VERSION"));
    CHECK(m.files[1].sha256[0] == 0xE3);

    /* 既定値: channel / type / base が無ければ既定になる。 */
    static const char manifest_min[] = "version=0.3.1\n";
    CHECK(update_parse_manifest(manifest_min, my_strlen(manifest_min), &m) == 0);
    CHECK(str_eq(m.version, "0.3.1"));
    CHECK(str_eq(m.channel, "stable"));
    CHECK(str_eq(m.type, "feature"));
    CHECK(str_eq(m.base, ""));
    CHECK(m.file_count == 0);

    /* 知らない行は無視する。 */
    static const char manifest_extra[] =
        "version=0.3.2\n"
        "note=this line is ignored\n"
        "file README.txt ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
    CHECK(update_parse_manifest(manifest_extra, my_strlen(manifest_extra), &m) == 0);
    CHECK(str_eq(m.version, "0.3.2"));
    CHECK(m.file_count == 1);

    /* version が無ければ失敗する。 */
    static const char manifest_noversion[] =
        "type=security\n"
        "file README.txt ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
    CHECK(update_parse_manifest(manifest_noversion,
                                my_strlen(manifest_noversion), &m) != 0);

    /* 壊れた file 行は失敗する。 */
    static const char manifest_badfile[] =
        "version=0.3.0\n"
        "file README.txt not-a-hash\n";
    CHECK(update_parse_manifest(manifest_badfile,
                                my_strlen(manifest_badfile), &m) != 0);

    /* カーネル行: ある場合だけ has_kernel になり、ハッシュが入る。 */
    static const char manifest_kernel[] =
        "version=0.3.0\n"
        "channel=stable\n"
        "type=feature\n"
        "base=/updates\n"
        "file VERSION e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
        "kernel ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
    CHECK(update_parse_manifest(manifest_kernel, my_strlen(manifest_kernel),
                                &m) == 0);
    CHECK(m.has_kernel == 1);
    CHECK(m.kernel_sha256[0] == 0xBA && m.kernel_sha256[31] == 0xAD);
    CHECK(m.file_count == 1);
    /* カーネル行が無ければ has_kernel は 0 のまま。 */
    CHECK(update_parse_manifest(manifest, my_strlen(manifest), &m) == 0);
    CHECK(m.has_kernel == 0);
    CHECK(update_parse_manifest(manifest_min, my_strlen(manifest_min),
                                &m) == 0);
    CHECK(m.has_kernel == 0);
    /* 壊れた kernel 行は失敗する。 */
    static const char manifest_badkernel[] =
        "version=0.3.0\n"
        "kernel not-a-hash\n";
    CHECK(update_parse_manifest(manifest_badkernel,
                                my_strlen(manifest_badkernel), &m) != 0);
    static const char manifest_shortkernel[] =
        "version=0.3.0\n"
        "kernel ba78\n";
    CHECK(update_parse_manifest(manifest_shortkernel,
                                my_strlen(manifest_shortkernel),
                                &m) != 0);

    /* 空のマニフェストは失敗する。 */
    CHECK(update_parse_manifest("", 0, &m) != 0);

    /* ---- ML-DSA-44 署名検証 (更新署名用の鍵で署名したもの) ---- */
    {
        size_t mlen = my_strlen(PQ_TEST_MANIFEST);
        CHECK(update_verify_manifest_sig(
                  (const uint8_t *)PQ_TEST_MANIFEST, mlen,
                  PQ_TEST_MANIFEST_SIG,
                  sizeof(PQ_TEST_MANIFEST_SIG)) == 0);
        /* 署名を 1 バイト変えると拒否される。 */
        uint8_t bad_sig[2420];
        for (size_t i = 0; i < sizeof(bad_sig); i++) {
            bad_sig[i] = PQ_TEST_MANIFEST_SIG[i];
        }
        bad_sig[100] ^= 1;
        CHECK(update_verify_manifest_sig(
                  (const uint8_t *)PQ_TEST_MANIFEST, mlen, bad_sig,
                  sizeof(bad_sig)) != 0);
        /* マニフェストを 1 文字変えると拒否される。 */
        char bad_manifest[200];
        size_t bi = 0;
        while (PQ_TEST_MANIFEST[bi] != '\0' && bi + 1 < sizeof(bad_manifest)) {
            bad_manifest[bi] = PQ_TEST_MANIFEST[bi];
            bi++;
        }
        bad_manifest[bi] = '\0';
        bad_manifest[8] = '1'; /* "version=0.3.0" -> "version=1.3.0" の改ざん */
        CHECK(update_verify_manifest_sig(
                  (const uint8_t *)bad_manifest, bi,
                  PQ_TEST_MANIFEST_SIG,
                  sizeof(PQ_TEST_MANIFEST_SIG)) != 0);
        /* 署名がない (長さ 0) と拒否される。 */
        CHECK(update_verify_manifest_sig(
                  (const uint8_t *)PQ_TEST_MANIFEST, mlen,
                  PQ_TEST_MANIFEST_SIG, 0) != 0);
        /* 署名が短いと拒否される。 */
        CHECK(update_verify_manifest_sig(
                  (const uint8_t *)PQ_TEST_MANIFEST, mlen,
                  PQ_TEST_MANIFEST_SIG, 100) != 0);
    }

    if (failures == 0) {
        printf("update tests: %d checks, 0 failures\n", checks);
        printf("PASS\n");
        return 0;
    }
    printf("update tests: %d checks, %d failures\n", checks, failures);
    printf("FAIL\n");
    return 1;
}
