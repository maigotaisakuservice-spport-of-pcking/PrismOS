/* PrismOS — ML-KEM (FIPS 203) の実装。
 *
 * 多項式は係数 256 個 (int16_t) で、NTT 領域でも同じ形。
 * 剰余 q = 3329 の計算は 32 ビット整数で足りる
 * (NTT の蝶形は zeta * 係数 < q^2、基底積だけ 64 ビット)。
 * NTT は参照実装と同じ 7 段 (len 128 → 2) で、基底積は
 * 4 係数ずつ +zeta / -zeta (zetas[64+i]) を使う。
 */
#include "mlkem.h"

#include "sha3.h"
#include "string.h"

#define KEM_Q 3329
#define KEM_N 256
#define KEM_F 3303 /* 128^-1 mod q (invntt の最後の掛け算) */

/* ---- 作業域 (静的。スタックを食わないため) ---- */
typedef struct {
    int16_t a_hat[16][KEM_N];
    int16_t t_hat[4][KEM_N];
    int16_t s[4][KEM_N];
    int16_t e[4][KEM_N];
    int16_t y[4][KEM_N];
    int16_t u[4][KEM_N];
    int16_t v[KEM_N];
    int16_t w[KEM_N];
    int16_t tmp[KEM_N];
} mlkem_ws;

static mlkem_ws WS;
static int16_t ZETAS[128];
static int zetas_ready = 0;

typedef struct {
    int k, eta1, eta2, du, dv;
    int ek_len, dk_len, ct_len;
} mlkem_params;

static const mlkem_params P512 = {2, 3, 2, 10, 4, 800, 1632, 768};
static const mlkem_params P768 = {3, 2, 2, 10, 4, 1184, 2400, 1088};
static const mlkem_params P1024 = {4, 2, 2, 11, 5, 1568, 3168, 1568};

/* ---- 小さな道具 ---- */

static int16_t modq(int32_t x) {
    x %= KEM_Q;
    if (x < 0) {
        x += KEM_Q;
    }
    return (int16_t)x;
}

static int16_t modq64(int64_t x) {
    x %= KEM_Q;
    if (x < 0) {
        x += KEM_Q;
    }
    return (int16_t)x;
}

static int powmod(int base, int exp) {
    int64_t r = 1;
    int64_t b = base;
    while (exp > 0) {
        if (exp & 1) {
            r = (r * b) % KEM_Q;
        }
        b = (b * b) % KEM_Q;
        exp >>= 1;
    }
    return (int)r;
}

static int bitrev7(int x) {
    int r = 0;
    for (int i = 0; i < 7; i++) {
        r = (r << 1) | ((x >> i) & 1);
    }
    return r;
}

static void init_zetas(void) {
    if (zetas_ready) {
        return;
    }
    for (int i = 0; i < 128; i++) {
        ZETAS[i] = (int16_t)powmod(17, bitrev7(i));
    }
    zetas_ready = 1;
}

/* ---- NTT ---- */

static void ntt(int16_t r[KEM_N]) {
    int k = 1;
    for (int len = 128; len >= 2; len >>= 1) {
        for (int start = 0; start < KEM_N; start += 2 * len) {
            int16_t zeta = ZETAS[k++];
            for (int j = start; j < start + len; j++) {
                int32_t t = (int32_t)zeta * r[j + len] % KEM_Q;
                int32_t a = r[j];
                r[j] = modq(a + t);
                r[j + len] = modq(a - t);
            }
        }
    }
}

static void invntt(int16_t r[KEM_N]) {
    int k = 127;
    for (int len = 2; len <= 128; len <<= 1) {
        for (int start = 0; start < KEM_N; start += 2 * len) {
            int16_t zeta = ZETAS[k--];
            for (int j = start; j < start + len; j++) {
                int32_t t = r[j];
                int32_t a = modq(t + r[j + len]);
                int32_t b = modq((int32_t)zeta * modq(r[j + len] - t));
                r[j] = (int16_t)a;
                r[j + len] = (int16_t)b;
            }
        }
    }
    for (int j = 0; j < KEM_N; j++) {
        r[j] = modq((int32_t)r[j] * KEM_F);
    }
}

/* NTT 領域の多項式積。4 係数ずつ、+zeta / -zeta。 */
static void poly_basemul(int16_t r[KEM_N], const int16_t a[KEM_N],
                         const int16_t b[KEM_N]) {
    for (int i = 0; i < 64; i++) {
        int16_t zeta = ZETAS[64 + i];
        int j = 4 * i;
        r[j] = modq64((int64_t)a[j] * b[j] +
                      (int64_t)a[j + 1] * b[j + 1] * zeta);
        r[j + 1] = modq64((int64_t)a[j] * b[j + 1] +
                          (int64_t)a[j + 1] * b[j]);
        r[j + 2] = modq64((int64_t)a[j + 2] * b[j + 2] -
                          (int64_t)a[j + 3] * b[j + 3] * zeta);
        r[j + 3] = modq64((int64_t)a[j + 2] * b[j + 3] +
                          (int64_t)a[j + 3] * b[j + 2]);
    }
}

/* r += basemul(a, b) (mod q)。 */
static void poly_basemul_add(int16_t r[KEM_N], const int16_t a[KEM_N],
                             const int16_t b[KEM_N]) {
    poly_basemul(WS.tmp, a, b);
    for (int i = 0; i < KEM_N; i++) {
        r[i] = modq((int32_t)r[i] + WS.tmp[i]);
    }
}

/* ---- サンプリング ---- */

/* SampleNTT: SHAKE128(rho || j || i) を 3 バイトずつ読む。 */
static void sample_ntt(int16_t r[KEM_N], const uint8_t rho[32],
                       uint8_t j, uint8_t i) {
    shake_ctx ctx;
    shake128_init(&ctx);
    shake_absorb(&ctx, rho, 32);
    uint8_t jj[2] = {j, i};
    shake_absorb(&ctx, jj, 2);
    shake_finalize(&ctx, 0x1f);
    int ctr = 0;
    while (ctr < KEM_N) {
        uint8_t buf[3];
        shake_squeeze(&ctx, buf, 3);
        int d1 = buf[0] + 256 * (buf[1] % 16);
        int d2 = buf[1] / 16 + 16 * buf[2];
        if (d1 < KEM_Q) {
            r[ctr++] = (int16_t)d1;
        }
        if (d2 < KEM_Q && ctr < KEM_N) {
            r[ctr++] = (int16_t)d2;
        }
    }
}

static int bit_at(const uint8_t *buf, size_t bit) {
    return (buf[bit / 8] >> (bit % 8)) & 1;
}

/* SamplePolyCBD: 仕様のビット定義どおりに素直に読む。 */
static void sample_cbd(int16_t r[KEM_N], const uint8_t *buf, int eta) {
    for (int i = 0; i < KEM_N; i++) {
        int x = 0, y = 0;
        for (int j = 0; j < eta; j++) {
            x += bit_at(buf, (size_t)(2 * i) * (size_t)eta + (size_t)j);
            y += bit_at(buf, (size_t)(2 * i + 1) * (size_t)eta +
                              (size_t)j);
        }
        r[i] = (int16_t)(x - y);
    }
}

/* PRF_eta(s, b) = SHAKE256(s || b)。out は 64*eta バイト。 */
static void prf_eta(uint8_t *out, size_t out_len, const uint8_t s[32],
                    uint8_t nonce) {
    shake_ctx ctx;
    shake256_init(&ctx);
    shake_absorb(&ctx, s, 32);
    shake_absorb(&ctx, &nonce, 1);
    shake_finalize(&ctx, 0x1f);
    shake_squeeze(&ctx, out, out_len);
}

/* ---- 圧縮と符号化 ---- */

static int16_t compress_coef(int16_t x, int d) {
    uint32_t t = (uint32_t)x;
    uint32_t v = ((t << d) + (uint32_t)(KEM_Q / 2)) / (uint32_t)KEM_Q;
    return (int16_t)(v & ((1u << d) - 1u));
}

static int16_t decompress_coef(uint16_t y, int d) {
    return (int16_t)(((uint32_t)y * (uint32_t)KEM_Q +
                      (1u << (d - 1))) >>
                     d);
}

/* d ビットずつ LSB ファーストで詰める。out はゼロで渡すこと。 */
static void byte_encode(uint8_t *out, const int16_t *f, int d) {
    uint32_t acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (int i = 0; i < KEM_N; i++) {
        acc |= ((uint32_t)f[i] & ((1u << d) - 1u)) << nbits;
        nbits += d;
        while (nbits >= 8) {
            out[o++] = (uint8_t)(acc & 0xffu);
            acc >>= 8;
            nbits -= 8;
        }
    }
    if (nbits > 0) {
        out[o++] = (uint8_t)(acc & 0xffu);
    }
}

static void byte_decode(int16_t *f, const uint8_t *in, int d) {
    uint32_t acc = 0;
    int nbits = 0;
    size_t p = 0;
    for (int i = 0; i < KEM_N; i++) {
        while (nbits < d) {
            acc |= (uint32_t)in[p++] << nbits;
            nbits += 8;
        }
        f[i] = (int16_t)(acc & ((1u << d) - 1u));
        acc >>= d;
        nbits -= d;
    }
}

static void poly_add(int16_t r[KEM_N], const int16_t a[KEM_N],
                     const int16_t b[KEM_N]) {
    for (int i = 0; i < KEM_N; i++) {
        r[i] = modq((int32_t)a[i] + b[i]);
    }
}

static void poly_sub(int16_t r[KEM_N], const int16_t a[KEM_N],
                     const int16_t b[KEM_N]) {
    for (int i = 0; i < KEM_N; i++) {
        r[i] = modq((int32_t)a[i] - b[i]);
    }
}

/* ---- K-PKE ---- */

/* ek = Encode12(t_hat) || rho (rho は末尾 32 バイト)。
 * dk_pke = Encode12(s_hat)。 */
static void kpke_keygen(const mlkem_params *p, const uint8_t d[32],
                        uint8_t *ek, uint8_t *dk_pke) {
    init_zetas();
    /* (rho, sigma) = G(d || k)。k は 1 バイト。 */
    uint8_t in33[33];
    memcpy(in33, d, 32);
    in33[32] = (uint8_t)p->k;
    uint8_t g[64];
    sha3_512(in33, 33, g);
    uint8_t rho[32], sigma[32];
    memcpy(rho, g, 32);
    memcpy(sigma, g + 32, 32);

    int n = 0;
    for (int i = 0; i < p->k; i++) {
        for (int j = 0; j < p->k; j++) {
            sample_ntt(WS.a_hat[i * p->k + j], rho, (uint8_t)j,
                       (uint8_t)i);
        }
    }
    uint8_t buf[192]; /* 64 * eta1 (最大 3) */
    for (int i = 0; i < p->k; i++) {
        prf_eta(buf, (size_t)(64 * p->eta1), sigma, (uint8_t)n++);
        sample_cbd(WS.s[i], buf, p->eta1);
    }
    for (int i = 0; i < p->k; i++) {
        prf_eta(buf, (size_t)(64 * p->eta1), sigma, (uint8_t)n++);
        sample_cbd(WS.e[i], buf, p->eta1);
    }
    for (int i = 0; i < p->k; i++) {
        ntt(WS.s[i]);
        ntt(WS.e[i]);
    }
    for (int i = 0; i < p->k; i++) {
        memset(WS.t_hat[i], 0, sizeof(WS.t_hat[i]));
        for (int j = 0; j < p->k; j++) {
            poly_basemul_add(WS.t_hat[i], WS.a_hat[i * p->k + j],
                             WS.s[j]);
        }
        poly_add(WS.t_hat[i], WS.t_hat[i], WS.e[i]);
    }
    memcpy(ek + 384 * p->k, rho, 32);
    for (int i = 0; i < p->k; i++) {
        byte_encode(ek + i * 384, WS.t_hat[i], 12);
    }
    for (int i = 0; i < p->k; i++) {
        byte_encode(dk_pke + i * 384, WS.s[i], 12);
    }
}

static void kpke_encrypt(const mlkem_params *p, const uint8_t *ek,
                         const uint8_t m[32], const uint8_t r[32],
                         uint8_t *ct) {
    init_zetas();
    /* rho は ek の末尾 32 バイト、t_hat は先頭。 */
    const uint8_t *rho = ek + 384 * p->k;
    /* t_hat を復号する。 */
    for (int i = 0; i < p->k; i++) {
        byte_decode(WS.t_hat[i], ek + i * 384, 12);
    }
    for (int i = 0; i < p->k; i++) {
        for (int j = 0; j < p->k; j++) {
            sample_ntt(WS.a_hat[i * p->k + j], rho, (uint8_t)j,
                       (uint8_t)i);
        }
    }
    int n = 0;
    uint8_t buf[192];
    for (int i = 0; i < p->k; i++) {
        prf_eta(buf, (size_t)(64 * p->eta1), r, (uint8_t)n++);
        sample_cbd(WS.y[i], buf, p->eta1);
    }
    for (int i = 0; i < p->k; i++) {
        prf_eta(buf, (size_t)(64 * p->eta2), r, (uint8_t)n++);
        sample_cbd(WS.e[i], buf, p->eta2);
    }
    prf_eta(buf, (size_t)(64 * p->eta2), r, (uint8_t)n++);
    sample_cbd(WS.v, buf, p->eta2); /* e2 を v に借り置きする */
    for (int i = 0; i < p->k; i++) {
        ntt(WS.y[i]);
    }
    /* u = invntt(A^T * y_hat) + e1 */
    for (int i = 0; i < p->k; i++) {
        memset(WS.u[i], 0, sizeof(WS.u[i]));
        for (int j = 0; j < p->k; j++) {
            poly_basemul_add(WS.u[i], WS.a_hat[j * p->k + i],
                             WS.y[j]);
        }
        invntt(WS.u[i]);
        poly_add(WS.u[i], WS.u[i], WS.e[i]);
    }
    /* mu = Decompress1(Decode1(m)) */
    int16_t mu[KEM_N];
    for (int i = 0; i < KEM_N; i++) {
        int bit = (m[i / 8] >> (i % 8)) & 1;
        mu[i] = decompress_coef((uint16_t)bit, 1);
    }
    /* v = invntt(t_hat^T * y_hat) + e2 + mu */
    memset(WS.w, 0, sizeof(WS.w));
    for (int i = 0; i < p->k; i++) {
        poly_basemul_add(WS.w, WS.t_hat[i], WS.y[i]);
    }
    invntt(WS.w);
    poly_add(WS.w, WS.w, WS.v); /* WS.v は e2 */
    poly_add(WS.w, WS.w, mu);
    /* c1 = Encode_du(Compress_du(u))、c2 = Encode_dv(Compress_dv(v)) */
    int16_t cu[KEM_N];
    for (int i = 0; i < p->k; i++) {
        for (int j = 0; j < KEM_N; j++) {
            cu[j] = compress_coef(WS.u[i][j], p->du);
        }
        byte_encode(ct + i * (32 * p->du), cu, p->du);
    }
    int16_t cv[KEM_N];
    for (int j = 0; j < KEM_N; j++) {
        cv[j] = compress_coef(WS.w[j], p->dv);
    }
    byte_encode(ct + p->k * (32 * p->du), cv, p->dv);
}

static void kpke_decrypt(const mlkem_params *p, const uint8_t *dk_pke,
                         const uint8_t *ct, uint8_t m[32]) {
    init_zetas();
    for (int i = 0; i < p->k; i++) {
        byte_decode(WS.s[i], dk_pke + i * 384, 12); /* s_hat */
    }
    for (int i = 0; i < p->k; i++) {
        int16_t cu[KEM_N];
        byte_decode(cu, ct + i * (32 * p->du), p->du);
        for (int j = 0; j < KEM_N; j++) {
            WS.u[i][j] = decompress_coef((uint16_t)cu[j], p->du);
        }
    }
    int16_t cv[KEM_N];
    byte_decode(cv, ct + p->k * (32 * p->du), p->dv);
    for (int j = 0; j < KEM_N; j++) {
        WS.v[j] = decompress_coef((uint16_t)cv[j], p->dv);
    }
    /* w = v - invntt(s_hat^T * ntt(u)) */
    memset(WS.w, 0, sizeof(WS.w));
    for (int i = 0; i < p->k; i++) {
        ntt(WS.u[i]);
        poly_basemul_add(WS.w, WS.s[i], WS.u[i]);
    }
    invntt(WS.w);
    poly_sub(WS.w, WS.v, WS.w);
    /* m = Encode1(Compress1(w)) */
    memset(m, 0, 32);
    for (int i = 0; i < KEM_N; i++) {
        int bit = compress_coef(WS.w[i], 1);
        if (bit) {
            m[i / 8] |= (uint8_t)(1u << (i % 8));
        }
    }
}

/* ---- ML-KEM ---- */

static int kem_keygen(const mlkem_params *p, const uint8_t d[32],
                      const uint8_t z[32], uint8_t *ek, uint8_t *dk) {
    uint8_t *dk_pke = dk;
    kpke_keygen(p, d, ek, dk_pke);
    /* dk = dk_pke || ek || H(ek) || z */
    uint8_t *ek_copy = dk + 384 * p->k;
    memcpy(ek_copy, ek, (size_t)p->ek_len);
    uint8_t hek[32];
    sha3_256(ek, (size_t)p->ek_len, hek);
    memcpy(dk + 384 * p->k + p->ek_len, hek, 32);
    memcpy(dk + 384 * p->k + p->ek_len + 32, z, 32);
    return 0;
}

static int kem_encaps(const mlkem_params *p, const uint8_t *ek,
                      const uint8_t m[32], uint8_t *ct, uint8_t *ss) {
    uint8_t hek[32];
    sha3_256(ek, (size_t)p->ek_len, hek);
    /* (K, r) = G(m || H(ek)) */
    uint8_t in64[64];
    memcpy(in64, m, 32);
    memcpy(in64 + 32, hek, 32);
    uint8_t g[64];
    sha3_512(in64, 64, g);
    uint8_t k0[32], r[32];
    memcpy(k0, g, 32);
    memcpy(r, g + 32, 32);
    kpke_encrypt(p, ek, m, r, ct);
    /* 共有鍵は G の前半そのもの (FIPS 203: 最終段の J は無い)。 */
    memcpy(ss, k0, 32);
    return 0;
}

static int kem_decaps(const mlkem_params *p, const uint8_t *dk,
                      const uint8_t *ct, uint8_t *ss) {
    const uint8_t *dk_pke = dk;
    const uint8_t *ek = dk + 384 * p->k;
    const uint8_t *hek = dk + 384 * p->k + p->ek_len;
    const uint8_t *z = dk + 384 * p->k + p->ek_len + 32;
    uint8_t m2[32];
    kpke_decrypt(p, dk_pke, ct, m2);
    /* (K', r') = G(m' || H(ek)) */
    uint8_t in64[64];
    memcpy(in64, m2, 32);
    memcpy(in64 + 32, hek, 32);
    uint8_t g[64];
    sha3_512(in64, 64, g);
    uint8_t k2[32], r2[32];
    memcpy(k2, g, 32);
    memcpy(r2, g + 32, 32);
    /* Kbar = J(z || c) (暗黙の拒否用。c 全体を吸収する)。 */
    uint8_t ct2[1568];
    kpke_encrypt(p, ek, m2, r2, ct2);
    /* Kbar = J(z || c) */
    shake_ctx ctx;
    shake256_init(&ctx);
    shake_absorb(&ctx, z, 32);
    shake_absorb(&ctx, ct, (size_t)p->ct_len);
    shake_finalize(&ctx, 0x1f);
    uint8_t kbar[32];
    shake_squeeze(&ctx, kbar, 32);
    /* c == c' なら K'、違えば Kbar。比較は定数時間で行い、
     * 選択も分岐なしにする (FO 変換の暗黙の拒否で memcmp の
     * 早期終了を使うとタイミングチャネルになる)。 */
    uint32_t diff = 0;
    for (int i = 0; i < p->ct_len; i++) {
        diff |= (uint32_t)(ct[i] ^ ct2[i]);
    }
    /* diff == 0 のときだけ全ビット 1 になるマスク。 */
    uint8_t mask = (uint8_t)(0u - (uint8_t)(diff == 0));
    for (int i = 0; i < 32; i++) {
        ss[i] = (uint8_t)(kbar[i] ^ (mask & (kbar[i] ^ k2[i])));
    }
    return 0;
}

int mlkem512_keygen(const uint8_t d[32], const uint8_t z[32],
                    uint8_t ek[MLKEM512_EK_LEN],
                    uint8_t dk[MLKEM512_DK_LEN]) {
    return kem_keygen(&P512, d, z, ek, dk);
}

int mlkem512_encaps(const uint8_t ek[MLKEM512_EK_LEN],
                    const uint8_t m[32],
                    uint8_t ct[MLKEM512_CT_LEN],
                    uint8_t ss[MLKEM_SS_LEN]) {
    return kem_encaps(&P512, ek, m, ct, ss);
}

int mlkem512_decaps(const uint8_t dk[MLKEM512_DK_LEN],
                    const uint8_t ct[MLKEM512_CT_LEN],
                    uint8_t ss[MLKEM_SS_LEN]) {
    return kem_decaps(&P512, dk, ct, ss);
}

int mlkem768_keygen(const uint8_t d[32], const uint8_t z[32],
                    uint8_t ek[MLKEM768_EK_LEN],
                    uint8_t dk[MLKEM768_DK_LEN]) {
    return kem_keygen(&P768, d, z, ek, dk);
}

int mlkem768_encaps(const uint8_t ek[MLKEM768_EK_LEN],
                    const uint8_t m[32],
                    uint8_t ct[MLKEM768_CT_LEN],
                    uint8_t ss[MLKEM_SS_LEN]) {
    return kem_encaps(&P768, ek, m, ct, ss);
}

int mlkem768_decaps(const uint8_t dk[MLKEM768_DK_LEN],
                    const uint8_t ct[MLKEM768_CT_LEN],
                    uint8_t ss[MLKEM_SS_LEN]) {
    return kem_decaps(&P768, dk, ct, ss);
}

int mlkem1024_keygen(const uint8_t d[32], const uint8_t z[32],
                     uint8_t ek[MLKEM1024_EK_LEN],
                     uint8_t dk[MLKEM1024_DK_LEN]) {
    return kem_keygen(&P1024, d, z, ek, dk);
}

int mlkem1024_encaps(const uint8_t ek[MLKEM1024_EK_LEN],
                     const uint8_t m[32],
                     uint8_t ct[MLKEM1024_CT_LEN],
                     uint8_t ss[MLKEM_SS_LEN]) {
    return kem_encaps(&P1024, ek, m, ct, ss);
}

int mlkem1024_decaps(const uint8_t dk[MLKEM1024_DK_LEN],
                     const uint8_t ct[MLKEM1024_CT_LEN],
                     uint8_t ss[MLKEM_SS_LEN]) {
    return kem_decaps(&P1024, dk, ct, ss);
}
