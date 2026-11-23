/* PrismOS — ML-DSA-44 (FIPS 204) の実装。
 *
 * パラメータ (ML-DSA-44): q = 8380417、d = 13、tau = 39、
 * gamma1 = 2^17、gamma2 = 95232、k = 4、l = 4、eta = 2、
 * beta = 78、omega = 80。
 * 多項式は係数 256 個 (int32_t)。NTT の蝶形は 64 ビット
 * 整数で計算する (q^2 が 32 ビットに収まらないため)。
 */
#include "mldsa.h"

#include "sha3.h"
#include "string.h"

#define DSA_Q 8380417
#define DSA_N 256
#define DSA_D 13
#define DSA_TAU 39
#define DSA_GAMMA1 131072
#define DSA_GAMMA2 95232
#define DSA_K 4
#define DSA_L 4
#define DSA_ETA 2
#define DSA_BETA 78
#define DSA_OMEGA 80
#define DSA_ALPHA 190464 /* 2 * gamma2 */
#define DSA_F 8347681    /* 256^-1 mod q (invntt の最後) */

/* ---- 作業域 (静的。スタックを食わないため) ---- */
typedef struct {
    int32_t a_hat[16][DSA_N];
    int32_t s1[4][DSA_N];
    int32_t s2[4][DSA_N];
    int32_t t0[4][DSA_N];
    int32_t t1[4][DSA_N];
    int32_t y[4][DSA_N];
    int32_t z[4][DSA_N];
    int32_t w[4][DSA_N];
    int32_t w1[4][DSA_N];
    int32_t c[DSA_N];
    int32_t tmp[DSA_N];
    int32_t tmp2[DSA_N];
} mldsa_ws;

static mldsa_ws WS;
static int32_t ZETAS[256];
static int zetas_ready = 0;

/* ---- 小さな道具 ---- */

static int32_t modq(int64_t x) {
    x %= DSA_Q;
    if (x < 0) {
        x += DSA_Q;
    }
    return (int32_t)x;
}

static int32_t mulmod(int32_t a, int32_t b) {
    return modq((int64_t)a * b);
}

static int powmod(int base, int exp) {
    int64_t r = 1;
    int64_t b = base;
    while (exp > 0) {
        if (exp & 1) {
            r = (r * b) % DSA_Q;
        }
        b = (b * b) % DSA_Q;
        exp >>= 1;
    }
    return (int)r;
}

static int bitrev8(int x) {
    int r = 0;
    for (int i = 0; i < 8; i++) {
        r = (r << 1) | ((x >> i) & 1);
    }
    return r;
}

static void init_zetas(void) {
    if (zetas_ready) {
        return;
    }
    /* ML-DSA の NTT は 8 段の完全 NTT。zeta は 1753 (512 乗根)
     * を使い、表は 1753^bitrev8(i)。ZETAS[0] は使わない。 */
    ZETAS[0] = 0;
    for (int i = 1; i < 256; i++) {
        ZETAS[i] = powmod(1753, bitrev8(i));
    }
    zetas_ready = 1;
}

/* ---- NTT (8 段の完全 NTT。出力はビット反転順) ---- */

static void ntt(int32_t r[DSA_N]) {
    int k = 0;
    for (int len = 128; len > 0; len >>= 1) {
        for (int start = 0; start < DSA_N; start += 2 * len) {
            int32_t zeta = ZETAS[++k];
            for (int j = start; j < start + len; j++) {
                int32_t t = mulmod(zeta, r[j + len]);
                int32_t a = r[j];
                r[j] = modq((int64_t)a + t);
                r[j + len] = modq((int64_t)a - t);
            }
        }
    }
}

static void invntt(int32_t r[DSA_N]) {
    int k = 256;
    for (int len = 1; len < DSA_N; len <<= 1) {
        for (int start = 0; start < DSA_N; start += 2 * len) {
            int32_t zeta = modq(-(int64_t)ZETAS[--k]);
            for (int j = start; j < start + len; j++) {
                int32_t t = r[j];
                int32_t a = modq((int64_t)t + r[j + len]);
                int32_t b = mulmod(zeta, modq((int64_t)t - r[j + len]));
                r[j] = a;
                r[j + len] = b;
            }
        }
    }
    for (int j = 0; j < DSA_N; j++) {
        r[j] = mulmod(r[j], DSA_F);
    }
}

/* NTT 領域の積は要素ごと (完全 NTT)。 */
static void poly_basemul(int32_t r[DSA_N], const int32_t a[DSA_N],
                         const int32_t b[DSA_N]) {
    for (int i = 0; i < DSA_N; i++) {
        r[i] = mulmod(a[i], b[i]);
    }
}

/* r += basemul(a, b) (mod q)。 */
static void poly_basemul_add(int32_t r[DSA_N], const int32_t a[DSA_N],
                             const int32_t b[DSA_N]) {
    poly_basemul(WS.tmp, a, b);
    for (int i = 0; i < DSA_N; i++) {
        r[i] = modq((int64_t)r[i] + WS.tmp[i]);
    }
}

static void poly_add(int32_t r[DSA_N], const int32_t a[DSA_N],
                     const int32_t b[DSA_N]) {
    for (int i = 0; i < DSA_N; i++) {
        r[i] = modq((int64_t)a[i] + b[i]);
    }
}

static void poly_sub(int32_t r[DSA_N], const int32_t a[DSA_N],
                     const int32_t b[DSA_N]) {
    for (int i = 0; i < DSA_N; i++) {
        r[i] = modq((int64_t)a[i] - b[i]);
    }
}

static void poly_neg(int32_t r[DSA_N], const int32_t a[DSA_N]) {
    for (int i = 0; i < DSA_N; i++) {
        r[i] = a[i] == 0 ? 0 : DSA_Q - a[i];
    }
}

/* [0, q) の係数を (-q/2, q/2] の中心表現に直す。
 * z = y + c*s1 のような小さな値 (絶対値が q/2 未満) に使う。 */
static void poly_center(int32_t r[DSA_N]) {
    for (int i = 0; i < DSA_N; i++) {
        if (r[i] > (DSA_Q - 1) / 2) {
            r[i] -= DSA_Q;
        }
    }
}

/* ---- サンプリング ---- */

/* SampleNTT: SHAKE128(rho || j || i)。3 バイトで 1 候補。 */
static void sample_ntt(int32_t r[DSA_N], const uint8_t rho[32],
                       uint8_t j, uint8_t i) {
    shake_ctx ctx;
    shake128_init(&ctx);
    shake_absorb(&ctx, rho, 32);
    uint8_t jj[2] = {j, i};
    shake_absorb(&ctx, jj, 2);
    shake_finalize(&ctx, 0x1f);
    int ctr = 0;
    while (ctr < DSA_N) {
        uint8_t buf[3];
        shake_squeeze(&ctx, buf, 3);
        uint32_t t = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                     ((uint32_t)buf[2] << 16);
        t &= 0x7FFFFFu;
        if ((int32_t)t < DSA_Q) {
            r[ctr++] = (int32_t)t;
        }
    }
}

/* FIPS 204 の eta 抽出 (CBD ではない)。ニブルごとに棄却し、
 * 係数 = 2 - (t mod 5)。下位ニブルが先。buf は十分に長いこと。 */
static void sample_eta(int32_t r[DSA_N], const uint8_t *buf,
                       size_t buflen) {
    int ctr = 0;
    for (size_t p = 0; p < buflen && ctr < DSA_N; p++) {
        uint8_t b = buf[p];
        for (int half = 0; half < 2 && ctr < DSA_N; half++) {
            int t = (half == 0) ? (b & 0x0f) : (b >> 4);
            if (t < 15) {
                int v = t - ((205 * t) >> 10) * 5;
                r[ctr++] = 2 - v;
            }
        }
    }
}

/* ExpandA: A_hat[i][j] = SampleNTT(rho, j, i)。 */
static void expand_a(const uint8_t rho[32]) {
    init_zetas();
    for (int i = 0; i < DSA_K; i++) {
        for (int j = 0; j < DSA_L; j++) {
            sample_ntt(WS.a_hat[i * DSA_L + j], rho, (uint8_t)j,
                       (uint8_t)i);
        }
    }
}

/* ExpandS: rho' (64 バイト) から s1, s2 を作る。 */
static void expand_s(const uint8_t rhoprime[64]) {
    uint8_t buf[512];
    for (int i = 0; i < DSA_L; i++) {
        shake_ctx ctx;
        shake256_init(&ctx);
        shake_absorb(&ctx, rhoprime, 64);
        uint8_t nonce[2] = {(uint8_t)i, 0};
        shake_absorb(&ctx, nonce, 2);
        shake_finalize(&ctx, 0x1f);
        shake_squeeze(&ctx, buf, sizeof(buf));
        sample_eta(WS.s1[i], buf, sizeof(buf));
    }
    for (int i = 0; i < DSA_K; i++) {
        shake_ctx ctx;
        shake256_init(&ctx);
        shake_absorb(&ctx, rhoprime, 64);
        uint8_t nonce[2] = {(uint8_t)(DSA_L + i), 0};
        shake_absorb(&ctx, nonce, 2);
        shake_finalize(&ctx, 0x1f);
        shake_squeeze(&ctx, buf, sizeof(buf));
        sample_eta(WS.s2[i], buf, sizeof(buf));
    }
}

/* ExpandMask: rho' (64 バイト) と試行カウンタから y を作る。
 * 各 y[i] は別々の SHAKE256 流 (nonce = L*kappa + i)。
 * 係数は polyz_unpack 式: 18 ビットずつ取り、y = GAMMA1 - v。
 * 棄却はしない。 */
static void expand_mask(const uint8_t rhoprime[64], uint16_t kappa) {
    static uint8_t buf[576];
    for (int i = 0; i < DSA_L; i++) {
        shake_ctx ctx;
        shake256_init(&ctx);
        shake_absorb(&ctx, rhoprime, 64);
        uint16_t n = (uint16_t)(DSA_L * kappa + i);
        uint8_t nonce[2] = {(uint8_t)n, (uint8_t)(n >> 8)};
        shake_absorb(&ctx, nonce, 2);
        shake_finalize(&ctx, 0x1f);
        shake_squeeze(&ctx, buf, sizeof(buf));
        uint32_t acc = 0;
        int nbits = 0;
        size_t p = 0;
        for (int j = 0; j < DSA_N; j++) {
            while (nbits < 18) {
                acc |= (uint32_t)buf[p++] << nbits;
                nbits += 8;
            }
            uint32_t v = acc & 0x3FFFFu;
            WS.y[i][j] = (int32_t)DSA_GAMMA1 - (int32_t)v;
            acc >>= 18;
            nbits -= 18;
        }
    }
}

/* SampleInBall: c_tilde (32 バイト) から tau 個の +-1 を作る。
 * 符号は先頭 8 バイト (64 ビット、下位ビットから順)、
 * 位置バイトはその後ろから。 */
static void sample_in_ball(int32_t c[DSA_N],
                           const uint8_t seed[32]) {
    shake_ctx ctx;
    shake256_init(&ctx);
    shake_absorb(&ctx, seed, 32);
    shake_finalize(&ctx, 0x1f);
    uint8_t sbuf[8];
    shake_squeeze(&ctx, sbuf, 8);
    uint64_t signs = 0;
    for (int i = 0; i < 8; i++) {
        signs |= (uint64_t)sbuf[i] << (8 * i);
    }
    memset(c, 0, sizeof(int32_t) * DSA_N);
    for (int i = DSA_N - DSA_TAU; i < DSA_N; i++) {
        uint8_t jb;
        int j;
        do {
            shake_squeeze(&ctx, &jb, 1);
            j = jb;
        } while (j > i);
        c[i] = c[j];
        c[j] = (signs & 1) ? -1 : 1;
        signs >>= 1;
    }
}

/* ---- 丸めとヒント ---- */

static int32_t power2round(int32_t *r0, int32_t r) {
    /* r0 は (-4096, 4096] になる (4096 - r0 が 13 ビットに
     * 収まるため。-1 が無いと r0 = -4096 になり符号化が
     * 壊れる)。 */
    int32_t r1 = (r + (1 << (DSA_D - 1)) - 1) >> DSA_D;
    *r0 = r - (r1 << DSA_D);
    return r1;
}

/* Decompose: (r1, r0) を返す。r は [0, q) であること。 */
static int32_t decompose(int32_t *r0, int32_t r) {
    int32_t t = r % DSA_ALPHA;
    if (t > DSA_GAMMA2) {
        t -= DSA_ALPHA;
    }
    *r0 = t;
    if (r - t == DSA_Q - 1) {
        *r0 = t - 1;
        return 0;
    }
    return (r - t) / DSA_ALPHA;
}

static int32_t highbits(int32_t r) {
    int32_t r0;
    return decompose(&r0, r);
}

static int32_t lowbits(int32_t r) {
    int32_t r0;
    (void)decompose(&r0, r);
    return r0;
}

static int make_hint(int32_t r, int32_t z) {
    int32_t r0, r0z;
    int32_t r1 = decompose(&r0, r);
    int32_t r1z = decompose(&r0z, modq((int64_t)r + z));
    (void)r0z;
    return (r1 != r1z) ? 1 : 0;
}

static int32_t use_hint(int32_t h, int32_t r) {
    int32_t r0;
    int32_t r1 = decompose(&r0, r);
    const int32_t m = (DSA_Q - 1) / DSA_ALPHA; /* 44 */
    if (h == 1) {
        if (r0 > 0) {
            int32_t v = r1 + 1;
            return (v == m) ? 0 : v;
        }
        int32_t v = r1 - 1;
        return (v < 0) ? m - 1 : v;
    }
    return r1;
}

/* ---- 符号化 (ビット詰めは LSB ファースト) ---- */

static void pack_bits(uint8_t *out, const int32_t *f, int bits,
                      int32_t sub) {
    /* 係数 x を (sub - x) の bits ビットで詰める。
     * sub = 0 ならそのまま。 */
    uint32_t acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (int i = 0; i < DSA_N; i++) {
        uint32_t v = (uint32_t)(sub - f[i]) & ((1u << bits) - 1u);
        acc |= v << nbits;
        nbits += bits;
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

static void unpack_bits(int32_t *f, const uint8_t *in, int bits,
                        int32_t sub) {
    uint32_t acc = 0;
    int nbits = 0;
    size_t p = 0;
    for (int i = 0; i < DSA_N; i++) {
        while (nbits < bits) {
            acc |= (uint32_t)in[p++] << nbits;
            nbits += 8;
        }
        uint32_t v = acc & ((1u << bits) - 1u);
        f[i] = sub - (int32_t)v;
        acc >>= bits;
        nbits -= bits;
    }
}

/* 係数をそのまま bits ビットで詰める (t1 用)。 */
static void pack_direct(uint8_t *out, const int32_t *f, int bits) {
    uint32_t acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (int i = 0; i < DSA_N; i++) {
        acc |= ((uint32_t)f[i] & ((1u << bits) - 1u)) << nbits;
        nbits += bits;
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

static void unpack_direct(int32_t *f, const uint8_t *in, int bits) {
    uint32_t acc = 0;
    int nbits = 0;
    size_t p = 0;
    for (int i = 0; i < DSA_N; i++) {
        while (nbits < bits) {
            acc |= (uint32_t)in[p++] << nbits;
            nbits += 8;
        }
        f[i] = (int32_t)(acc & ((1u << bits) - 1u));
        acc >>= bits;
        nbits -= bits;
    }
}

/* w1 (6 ビット、0..43) の詰め。sub は使わない。 */
static void pack_w1(uint8_t *out, const int32_t *f) {
    uint32_t acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (int i = 0; i < DSA_N; i++) {
        acc |= ((uint32_t)f[i] & 0x3fu) << nbits;
        nbits += 6;
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

/* ヒントの詰め: 1 の位置を並べ (最大 80 バイト)、
 * 末尾 4 バイトに多項式ごとの累積数を置く。 */
static void pack_hint(uint8_t *out, const int32_t h[4][DSA_N]) {
    size_t o = 0;
    size_t counts[DSA_K];
    for (int i = 0; i < DSA_K; i++) {
        for (int j = 0; j < DSA_N; j++) {
            if (h[i][j] != 0) {
                out[o++] = (uint8_t)j;
            }
        }
        counts[i] = o;
    }
    /* 残りの位置バイトは 0 で埋める (強力な偽造不可能性のため)。 */
    while (o < DSA_OMEGA) {
        out[o++] = 0;
    }
    for (int i = 0; i < DSA_K; i++) {
        out[DSA_OMEGA + i] = (uint8_t)counts[i];
    }
}

static int unpack_hint(int32_t h[4][DSA_N], const uint8_t *in) {
    /* in は 84 バイト。戻り値 0 = OK。 */
    memset(h, 0, sizeof(int32_t) * DSA_K * DSA_N);
    size_t prev = 0;
    for (int i = 0; i < DSA_K; i++) {
        size_t end = in[DSA_OMEGA + i]; /* この多項式までの累積数 */
        if (end < prev || end > DSA_OMEGA) {
            return -1;
        }
        for (size_t j = prev; j < end; j++) {
            uint8_t pos = in[j];
            if (j > prev && in[j] <= in[j - 1]) {
                return -1; /* 位置は昇順でなければならない */
            }
            h[i][pos] = 1;
        }
        prev = end;
    }
    /* 余分な位置バイトは 0 でなければならない。 */
    for (size_t j = prev; j < DSA_OMEGA; j++) {
        if (in[j] != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- ノルムの検査 ---- */

static int norm_exceeds(const int32_t *f, int32_t bound) {
    for (int i = 0; i < DSA_N; i++) {
        int32_t v = f[i];
        if (v < 0) {
            v = -v;
        }
        if (v >= bound) {
            return 1;
        }
    }
    return 0;
}

/* [0, q) の係数を中心表現の絶対値で検査する。
 * ct0 = c*t0 のような mod-q 値に使う。 */
static int norm_exceeds_centered(const int32_t *f, int32_t bound) {
    for (int i = 0; i < DSA_N; i++) {
        int32_t v = f[i];
        if (v > (DSA_Q - 1) / 2) {
            v -= DSA_Q;
        }
        if (v < 0) {
            v = -v;
        }
        if (v >= bound) {
            return 1;
        }
    }
    return 0;
}

static int count_hint_ones(const int32_t h[4][DSA_N]) {
    int n = 0;
    for (int i = 0; i < DSA_K; i++) {
        for (int j = 0; j < DSA_N; j++) {
            if (h[i][j] != 0) {
                n++;
            }
        }
    }
    return n;
}

/* ---- mu の計算 (tr || 0x00 || 0x00 || msg) ---- */

static void compute_mu(uint8_t mu[64], const uint8_t tr[64],
                       const uint8_t *msg, size_t msg_len) {
    shake_ctx ctx;
    shake256_init(&ctx);
    shake_absorb(&ctx, tr, 64);
    uint8_t zeros[2] = {0, 0};
    shake_absorb(&ctx, zeros, 2);
    shake_absorb(&ctx, msg, msg_len);
    shake_finalize(&ctx, 0x1f);
    shake_squeeze(&ctx, mu, 64);
}

/* ---- 公開 API ---- */

int mldsa44_keygen_from_seed(const uint8_t seed[MLDSA44_SEED_LEN],
                             uint8_t pk[MLDSA44_PK_LEN],
                             uint8_t sk[MLDSA44_SK_LEN]) {
    init_zetas();
    /* (rho, rho', K) = SHAKE256(seed || k || l)、128 バイト。 */
    uint8_t in34[34];
    memcpy(in34, seed, 32);
    in34[32] = DSA_K;
    in34[33] = DSA_L;
    uint8_t exp[128];
    shake256_once(in34, 34, exp, 128);
    const uint8_t *rho = exp;
    const uint8_t *rhoprime = exp + 32;
    const uint8_t *key = exp + 96;

    expand_a(rho);
    expand_s(rhoprime);
    /* t = invntt(A * s1) + s2 */
    int32_t t[4][DSA_N];
    for (int i = 0; i < DSA_K; i++) {
        memset(t[i], 0, sizeof(t[i]));
        for (int j = 0; j < DSA_L; j++) {
            int32_t sh[DSA_N];
            memcpy(sh, WS.s1[j], sizeof(sh));
            ntt(sh);
            poly_basemul_add(t[i], WS.a_hat[i * DSA_L + j], sh);
        }
        invntt(t[i]);
        poly_add(t[i], t[i], WS.s2[i]);
    }
    /* (t1, t0) = Power2Round(t) */
    for (int i = 0; i < DSA_K; i++) {
        for (int j = 0; j < DSA_N; j++) {
            int32_t r0;
            WS.t1[i][j] = power2round(&r0, t[i][j]);
            WS.t0[i][j] = r0;
        }
    }
    /* pk = rho || Encode(t1) */
    memcpy(pk, rho, 32);
    for (int i = 0; i < DSA_K; i++) {
        pack_direct(pk + 32 + i * 320, WS.t1[i], 10);
    }
    /* tr = SHAKE256(pk)、64 バイト。 */
    uint8_t tr[64];
    shake256_once(pk, MLDSA44_PK_LEN, tr, 64);
    /* sk = rho || K || tr || s1 || s2 || t0 */
    memcpy(sk, rho, 32);
    memcpy(sk + 32, key, 32);
    memcpy(sk + 64, tr, 64);
    for (int i = 0; i < DSA_L; i++) {
        pack_bits(sk + 128 + i * 96, WS.s1[i], 3, DSA_ETA);
    }
    for (int i = 0; i < DSA_K; i++) {
        pack_bits(sk + 128 + DSA_L * 96 + i * 96, WS.s2[i], 3,
                  DSA_ETA);
    }
    for (int i = 0; i < DSA_K; i++) {
        pack_bits(sk + 128 + (DSA_L + DSA_K) * 96 + i * 416,
                  WS.t0[i], 13, 1 << (DSA_D - 1));
    }
    return 0;
}

int mldsa44_sign(const uint8_t sk[MLDSA44_SK_LEN],
                 const uint8_t *msg, size_t msg_len,
                 uint8_t sig[MLDSA44_SIG_LEN]) {
    init_zetas();
    const uint8_t *rho = sk;
    const uint8_t *key = sk + 32;
    const uint8_t *tr = sk + 64;
    /* s1, s2, t0 を復号する。 */
    for (int i = 0; i < DSA_L; i++) {
        unpack_bits(WS.s1[i], sk + 128 + i * 96, 3, DSA_ETA);
    }
    for (int i = 0; i < DSA_K; i++) {
        unpack_bits(WS.s2[i], sk + 128 + DSA_L * 96 + i * 96, 3,
                    DSA_ETA);
    }
    for (int i = 0; i < DSA_K; i++) {
        unpack_bits(WS.t0[i],
                    sk + 128 + (DSA_L + DSA_K) * 96 + i * 416, 13,
                    1 << (DSA_D - 1));
    }
    /* s1, s2, t0 の NTT 形を作る。 */
    int32_t s1h[4][DSA_N], s2h[4][DSA_N], t0h[4][DSA_N];
    for (int i = 0; i < DSA_L; i++) {
        memcpy(s1h[i], WS.s1[i], sizeof(s1h[i]));
        ntt(s1h[i]);
    }
    for (int i = 0; i < DSA_K; i++) {
        memcpy(s2h[i], WS.s2[i], sizeof(s2h[i]));
        ntt(s2h[i]);
        memcpy(t0h[i], WS.t0[i], sizeof(t0h[i]));
        ntt(t0h[i]);
    }
    expand_a(rho);

    uint8_t mu[64];
    compute_mu(mu, tr, msg, msg_len);

    uint8_t c_tilde[32];
    uint16_t kappa = 0;
    int32_t hint[4][DSA_N];
    for (;;) {
        /* rho' = SHAKE256(K || rnd(0) || mu)、64 バイト。 */
        uint8_t in128[128];
        memcpy(in128, key, 32);
        memset(in128 + 32, 0, 32);
        memcpy(in128 + 64, mu, 64);
        uint8_t rhoprime[64];
        shake256_once(in128, 128, rhoprime, 64);
        expand_mask(rhoprime, kappa);
        kappa++;
        /* w = A * y */
        for (int i = 0; i < DSA_K; i++) {
            int32_t yh[DSA_N];
            memset(WS.w[i], 0, sizeof(WS.w[i]));
            for (int j = 0; j < DSA_L; j++) {
                memcpy(yh, WS.y[j], sizeof(yh));
                ntt(yh);
                poly_basemul_add(WS.w[i], WS.a_hat[i * DSA_L + j],
                                 yh);
            }
            invntt(WS.w[i]);
        }
        /* w1 = HighBits(w) */
        for (int i = 0; i < DSA_K; i++) {
            for (int j = 0; j < DSA_N; j++) {
                WS.w1[i][j] = highbits(WS.w[i][j]);
            }
        }
        /* c_tilde = SHAKE256(mu || Encode(w1))、32 バイト。 */
        uint8_t w1enc[DSA_K * 192];
        for (int i = 0; i < DSA_K; i++) {
            pack_w1(w1enc + i * 192, WS.w1[i]);
        }
        shake_ctx ctx;
        shake256_init(&ctx);
        shake_absorb(&ctx, mu, 64);
        shake_absorb(&ctx, w1enc, sizeof(w1enc));
        shake_finalize(&ctx, 0x1f);
        shake_squeeze(&ctx, c_tilde, 32);
        /* c = SampleInBall(c_tilde) */
        sample_in_ball(WS.c, c_tilde);
        int32_t ch[DSA_N];
        memcpy(ch, WS.c, sizeof(ch));
        ntt(ch);
        /* z = y + c*s1 (中心表現に直す。ノルム検査と符号化のため) */
        for (int i = 0; i < DSA_L; i++) {
            poly_basemul(WS.tmp, ch, s1h[i]);
            invntt(WS.tmp);
            poly_add(WS.z[i], WS.y[i], WS.tmp);
            poly_center(WS.z[i]);
        }
        /* r0 = LowBits(w - c*s2) */
        int32_t r0v[4][DSA_N];
        for (int i = 0; i < DSA_K; i++) {
            poly_basemul(WS.tmp, ch, s2h[i]);
            invntt(WS.tmp);
            poly_sub(WS.tmp2, WS.w[i], WS.tmp);
            for (int j = 0; j < DSA_N; j++) {
                r0v[i][j] = lowbits(WS.tmp2[j]);
            }
        }
        int bad = 0;
        for (int i = 0; i < DSA_L; i++) {
            if (norm_exceeds(WS.z[i], DSA_GAMMA1 - DSA_BETA)) {
                bad = 1;
            }
        }
        for (int i = 0; i < DSA_K; i++) {
            if (norm_exceeds(r0v[i], DSA_GAMMA2 - DSA_BETA)) {
                bad = 1;
            }
        }
        if (bad) {
            continue;
        }
        /* ct0 = c*t0、h = MakeHint(-ct0, w - c*s2 + ct0) */
        memset(hint, 0, sizeof(hint));
        int ct0_bad = 0;
        for (int i = 0; i < DSA_K; i++) {
            poly_basemul(WS.tmp, ch, t0h[i]);
            invntt(WS.tmp);
            /* ct0 を WS.tmp2 に置く。 */
            memcpy(WS.tmp2, WS.tmp, sizeof(WS.tmp2));
            if (norm_exceeds_centered(WS.tmp2, DSA_GAMMA2)) {
                ct0_bad = 1;
            }
            /* w - c*s2 + ct0 を作る。 */
            poly_basemul(WS.tmp, ch, s2h[i]);
            invntt(WS.tmp);
            int32_t v[DSA_N];
            poly_sub(v, WS.w[i], WS.tmp);
            poly_add(v, v, WS.tmp2);
            int32_t neg_ct0[DSA_N];
            poly_neg(neg_ct0, WS.tmp2);
            for (int j = 0; j < DSA_N; j++) {
                hint[i][j] = make_hint(v[j], neg_ct0[j]);
            }
        }
        if (ct0_bad) {
            continue;
        }
        if (count_hint_ones(hint) > DSA_OMEGA) {
            continue;
        }
        break;
    }
    /* sig = c_tilde || Encode(z) || Encode(h)。
     * z の符号化は GAMMA1 - z (FIPS 204 の polyz_pack)。 */
    memcpy(sig, c_tilde, 32);
    for (int i = 0; i < DSA_L; i++) {
        pack_bits(sig + 32 + i * 576, WS.z[i], 18,
                  DSA_GAMMA1);
    }
    pack_hint(sig + 32 + DSA_L * 576, hint);
    return 0;
}

int mldsa44_verify(const uint8_t pk[MLDSA44_PK_LEN],
                   const uint8_t *msg, size_t msg_len,
                   const uint8_t sig[MLDSA44_SIG_LEN]) {
    init_zetas();
    const uint8_t *rho = pk;
    for (int i = 0; i < DSA_K; i++) {
        unpack_direct(WS.t1[i], pk + 32 + i * 320, 10);
    }
    const uint8_t *c_tilde = sig;
    for (int i = 0; i < DSA_L; i++) {
        unpack_bits(WS.z[i], sig + 32 + i * 576, 18,
                    DSA_GAMMA1);
        if (norm_exceeds(WS.z[i], DSA_GAMMA1 - DSA_BETA)) {
            return -1;
        }
    }
    int32_t hint[4][DSA_N];
    if (unpack_hint(hint, sig + 32 + DSA_L * 576) != 0) {
        return -1;
    }
    if (count_hint_ones(hint) > DSA_OMEGA) {
        return -1;
    }
    /* tr = SHAKE256(pk)、mu = SHAKE256(tr || 0 || 0 || msg) */
    uint8_t tr[64];
    shake256_once(pk, MLDSA44_PK_LEN, tr, 64);
    uint8_t mu[64];
    compute_mu(mu, tr, msg, msg_len);
    /* c = SampleInBall(c_tilde) */
    sample_in_ball(WS.c, c_tilde);
    int32_t ch[DSA_N];
    memcpy(ch, WS.c, sizeof(ch));
    ntt(ch);
    expand_a(rho);
    /* A*z - c*t1*2^d */
    for (int i = 0; i < DSA_K; i++) {
        memset(WS.w[i], 0, sizeof(WS.w[i]));
        for (int j = 0; j < DSA_L; j++) {
            int32_t zh[DSA_N];
            memcpy(zh, WS.z[j], sizeof(zh));
            ntt(zh);
            poly_basemul_add(WS.w[i], WS.a_hat[i * DSA_L + j], zh);
        }
        invntt(WS.w[i]);
        /* c*t1*2^d を引く。 */
        int32_t t1s[DSA_N];
        for (int j = 0; j < DSA_N; j++) {
            t1s[j] = modq((int64_t)WS.t1[i][j] * (1 << DSA_D));
        }
        ntt(t1s);
        poly_basemul(WS.tmp, ch, t1s);
        invntt(WS.tmp);
        poly_sub(WS.w[i], WS.w[i], WS.tmp);
    }
    /* w1' = UseHint(h, ...) */
    for (int i = 0; i < DSA_K; i++) {
        for (int j = 0; j < DSA_N; j++) {
            WS.w1[i][j] = use_hint(hint[i][j], WS.w[i][j]);
        }
    }
    uint8_t w1enc[DSA_K * 192];
    for (int i = 0; i < DSA_K; i++) {
        pack_w1(w1enc + i * 192, WS.w1[i]);
    }
    uint8_t c2[32];
    shake_ctx ctx;
    shake256_init(&ctx);
    shake_absorb(&ctx, mu, 64);
    shake_absorb(&ctx, w1enc, sizeof(w1enc));
    shake_finalize(&ctx, 0x1f);
    shake_squeeze(&ctx, c2, 32);
    if (memcmp(c_tilde, c2, 32) != 0) {
        return -1;
    }
    return 0;
}
