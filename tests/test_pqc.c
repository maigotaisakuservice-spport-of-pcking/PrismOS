/* PrismOS — 耐量子暗号のホストテスト (SHA3/SHAKE、ML-KEM、ML-DSA)。
 * 期待値は公開の参照実装で確かめたもの: ML-KEM-768/1024 と
 * ML-DSA は Python の cryptography ライブラリ (FIPS 203/204)、
 * ML-KEM-512 は同ライブラリに無いため PQClean (pqcrypto) を
 * 参照実装にして双方向のベクトルで照合する
 * (scripts/pq-kat512.py が生成時に相互検証済み)。
 * メッセージは 16 進文字列で入っているため、復号してから使う。 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "sha3.h"
#include "mlkem.h"
#include "mldsa.h"
#include "pq_vectors.h"
#include "pq_vectors_512.h"

/* -I shell があると <string.h> がカーネルのものになるため、
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

static int hexval(int c) {
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

static int hex_eq(const uint8_t *got, const char *want_hex, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int hi = hexval((unsigned char)want_hex[i * 2]);
        int lo = hexval((unsigned char)want_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        if (got[i] != (uint8_t)(hi * 16 + lo)) {
            return 0;
        }
    }
    return 1;
}

static size_t unhex(const char *h, uint8_t *out) {
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)(hexval((unsigned char)h[i * 2]) * 16 +
                           hexval((unsigned char)h[i * 2 + 1]));
    }
    return n;
}

int main(void) {
    uint8_t msg[64];
    uint8_t out32[32];
    uint8_t out64[64];
    size_t msglen;

    /* ---- SHA3/SHAKE (3 メッセージ) ---- */
    msglen = unhex(PQ_SHA_MSG0, msg);
    sha3_256(msg, msglen, out32);
    check(hex_eq(out32, PQ_SHA3_256_0, 32), "sha3-256 empty");
    sha3_512(msg, msglen, out64);
    check(hex_eq(out64, PQ_SHA3_512_0, 64), "sha3-512 empty");
    shake128_once(msg, msglen, out64, 64);
    check(hex_eq(out64, PQ_SHAKE128_0, 64), "shake128 empty");
    shake256_once(msg, msglen, out64, 64);
    check(hex_eq(out64, PQ_SHAKE256_0, 64), "shake256 empty");

    msglen = unhex(PQ_SHA_MSG1, msg);
    sha3_256(msg, msglen, out32);
    check(hex_eq(out32, PQ_SHA3_256_1, 32), "sha3-256 abc");
    sha3_512(msg, msglen, out64);
    check(hex_eq(out64, PQ_SHA3_512_1, 64), "sha3-512 abc");
    shake128_once(msg, msglen, out64, 64);
    check(hex_eq(out64, PQ_SHAKE128_1, 64), "shake128 abc");
    shake256_once(msg, msglen, out64, 64);
    check(hex_eq(out64, PQ_SHAKE256_1, 64), "shake256 abc");

    msglen = unhex(PQ_SHA_MSG2, msg);
    sha3_256(msg, msglen, out32);
    check(hex_eq(out32, PQ_SHA3_256_2, 32), "sha3-256 50 bytes");
    sha3_512(msg, msglen, out64);
    check(hex_eq(out64, PQ_SHA3_512_2, 64), "sha3-512 50 bytes");
    shake128_once(msg, msglen, out64, 64);
    check(hex_eq(out64, PQ_SHAKE128_2, 64), "shake128 50 bytes");
    shake256_once(msg, msglen, out64, 64);
    check(hex_eq(out64, PQ_SHAKE256_2, 64), "shake256 50 bytes");

    /* ---- ML-KEM-768 (公開ベクトルと一致) ---- */
    {
        uint8_t ek[MLKEM768_EK_LEN];
        uint8_t dk[MLKEM768_DK_LEN];
        uint8_t ss[MLKEM_SS_LEN];
        mlkem768_keygen(PQ_KEM768_SEED, PQ_KEM768_SEED + 32, ek, dk);
        check(bytes_eq(ek, PQ_KEM768_PK, MLKEM768_EK_LEN),
              "mlkem768 keygen ek");
        mlkem768_decaps(dk, PQ_KEM768_CT, ss);
        check(bytes_eq(ss, PQ_KEM768_SS, MLKEM_SS_LEN),
              "mlkem768 decaps known ct");
        /* 自分でカプセル化したものを復号すると同じ共有鍵になる。 */
        uint8_t ct[MLKEM768_CT_LEN];
        uint8_t ss2[MLKEM_SS_LEN];
        uint8_t m[32];
        memset(m, 0x37, 32);
        mlkem768_encaps(ek, m, ct, ss);
        mlkem768_decaps(dk, ct, ss2);
        check(bytes_eq(ss, ss2, MLKEM_SS_LEN),
              "mlkem768 encaps/decaps round-trip");
        /* 改ざんしたカプセル化は別の共有鍵になる (暗黙の拒否)。 */
        ct[0] ^= 1;
        mlkem768_decaps(dk, ct, ss2);
        check(!bytes_eq(ss, ss2, MLKEM_SS_LEN),
              "mlkem768 tampered ct changes ss");
    }

    /* ---- ML-KEM-1024 (公開ベクトルと一致) ---- */
    {
        uint8_t ek[MLKEM1024_EK_LEN];
        uint8_t dk[MLKEM1024_DK_LEN];
        uint8_t ss[MLKEM_SS_LEN];
        mlkem1024_keygen(PQ_KEM1024_SEED, PQ_KEM1024_SEED + 32, ek, dk);
        check(bytes_eq(ek, PQ_KEM1024_PK, MLKEM1024_EK_LEN),
              "mlkem1024 keygen ek");
        mlkem1024_decaps(dk, PQ_KEM1024_CT, ss);
        check(bytes_eq(ss, PQ_KEM1024_SS, MLKEM_SS_LEN),
              "mlkem1024 decaps known ct");
        uint8_t ct[MLKEM1024_CT_LEN];
        uint8_t ss2[MLKEM_SS_LEN];
        uint8_t m[32];
        memset(m, 0x5b, 32);
        mlkem1024_encaps(ek, m, ct, ss);
        mlkem1024_decaps(dk, ct, ss2);
        check(bytes_eq(ss, ss2, MLKEM_SS_LEN),
              "mlkem1024 encaps/decaps round-trip");
        ct[100] ^= 0x40;
        mlkem1024_decaps(dk, ct, ss2);
        check(!bytes_eq(ss, ss2, MLKEM_SS_LEN),
              "mlkem1024 tampered ct changes ss");
    }

    /* ---- ML-KEM-512 (PQClean 参照ベクトルで双方向照合) ---- */
    {
        uint8_t ss[MLKEM_SS_LEN];
        uint8_t ct[MLKEM512_CT_LEN];
        /* 参照実装の鍵対・暗号文で decaps が一致する。 */
        mlkem512_decaps(PQ_KEM512_SK, PQ_KEM512_CT_REF, ss);
        check(bytes_eq(ss, PQ_KEM512_SS_REF, MLKEM_SS_LEN),
              "mlkem512 decaps matches PQClean vector");
        /* 固定 m での encaps が、参照実装と相互検証済みの
         * 暗号文・共有鍵と一致する。 */
        mlkem512_encaps(PQ_KEM512_PK, PQ_KEM512_M, ct, ss);
        check(bytes_eq(ct, PQ_KEM512_CT_C, MLKEM512_CT_LEN),
              "mlkem512 encaps matches cross-checked vector");
        check(bytes_eq(ss, PQ_KEM512_SS_C, MLKEM_SS_LEN),
              "mlkem512 encaps ss matches");
    }

    /* ---- ML-KEM-512 (一貫性と改ざん検出) ---- */
    {
        uint8_t d[32];
        uint8_t z[32];
        uint8_t ek[MLKEM512_EK_LEN];
        uint8_t dk[MLKEM512_DK_LEN];
        uint8_t ct[MLKEM512_CT_LEN];
        uint8_t ss[MLKEM_SS_LEN];
        uint8_t ss2[MLKEM_SS_LEN];
        uint8_t m[32];
        memset(d, 0x11, 32);
        memset(z, 0x22, 32);
        memset(m, 0x33, 32);
        mlkem512_keygen(d, z, ek, dk);
        mlkem512_encaps(ek, m, ct, ss);
        mlkem512_decaps(dk, ct, ss2);
        check(bytes_eq(ss, ss2, MLKEM_SS_LEN),
              "mlkem512 encaps/decaps round-trip");
        ct[0] ^= 1;
        mlkem512_decaps(dk, ct, ss2);
        check(!bytes_eq(ss, ss2, MLKEM_SS_LEN),
              "mlkem512 tampered ct changes ss");
        /* 同じ種からは同じ鍵ができる (決定的)。 */
        uint8_t ek2[MLKEM512_EK_LEN];
        uint8_t dk2[MLKEM512_DK_LEN];
        mlkem512_keygen(d, z, ek2, dk2);
        check(bytes_eq(ek, ek2, MLKEM512_EK_LEN),
              "mlkem512 keygen deterministic");
    }

    /* ---- ML-DSA-44 (公開ベクトル 3 件) ---- */
    {
        const uint8_t *seeds[3] = {PQ_DSA0_SK, PQ_DSA1_SK, PQ_DSA2_SK};
        const uint8_t *pks[3] = {PQ_DSA0_PK, PQ_DSA1_PK, PQ_DSA2_PK};
        const uint8_t *sigs[3] = {PQ_DSA0_SIG, PQ_DSA1_SIG, PQ_DSA2_SIG};
        const char *msgs[3] = {PQ_DSA0_MSG, PQ_DSA1_MSG, PQ_DSA2_MSG};
        for (int v = 0; v < 3; v++) {
            uint8_t pk[MLDSA44_PK_LEN];
            uint8_t sk[MLDSA44_SK_LEN];
            uint8_t mbuf[64];
            size_t mlen = unhex(msgs[v], mbuf);
            mldsa44_keygen_from_seed(seeds[v], pk, sk);
            check(bytes_eq(pk, pks[v], MLDSA44_PK_LEN),
                  "mldsa44 keygen pk");
            check(mldsa44_verify(pks[v], mbuf, mlen, sigs[v]) == 0,
                  "mldsa44 verify library sig");
            uint8_t bad[MLDSA44_SIG_LEN];
            memcpy(bad, sigs[v], MLDSA44_SIG_LEN);
            bad[100] ^= 1;
            check(mldsa44_verify(pks[v], mbuf, mlen, bad) != 0,
                  "mldsa44 tampered sig rejected");
            if (mlen > 0) {
                uint8_t badm[64];
                memcpy(badm, mbuf, mlen);
                badm[0] ^= 1;
                check(mldsa44_verify(pks[v], badm, mlen, sigs[v]) != 0,
                      "mldsa44 tampered msg rejected");
            }
        }
    }

    /* ---- ML-DSA-44 (自分で署名して検証) ---- */
    {
        uint8_t seed[32];
        uint8_t pk[MLDSA44_PK_LEN];
        uint8_t sk[MLDSA44_SK_LEN];
        uint8_t sig[MLDSA44_SIG_LEN];
        memset(seed, 0x5a, 32);
        mldsa44_keygen_from_seed(seed, pk, sk);
        const char *m = "PrismOS update 0.3.0";
        size_t mlen = strlen(m);
        mldsa44_sign(sk, (const uint8_t *)m, mlen, sig);
        check(mldsa44_verify(pk, (const uint8_t *)m, mlen, sig) == 0,
              "mldsa44 self sign/verify");
        uint8_t bad[MLDSA44_SIG_LEN];
        memcpy(bad, sig, MLDSA44_SIG_LEN);
        bad[200] ^= 1;
        check(mldsa44_verify(pk, (const uint8_t *)m, mlen, bad) != 0,
              "mldsa44 self tampered sig rejected");
        /* 空メッセージも署名・検証できる。 */
        mldsa44_sign(sk, (const uint8_t *)"", 0, sig);
        check(mldsa44_verify(pk, (const uint8_t *)"", 0, sig) == 0,
              "mldsa44 empty msg sign/verify");
        /* 同じ種からは同じ署名になる (決定的)。 */
        uint8_t sig2[MLDSA44_SIG_LEN];
        mldsa44_sign(sk, (const uint8_t *)m, mlen, sig2);
        mldsa44_sign(sk, (const uint8_t *)m, mlen, sig);
        check(bytes_eq(sig, sig2, MLDSA44_SIG_LEN),
              "mldsa44 sign deterministic");
    }

    printf("pqc tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
