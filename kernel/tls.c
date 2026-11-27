/* PrismOS - TLS 1.2 client, written from RFC 5246 and
 * RFC 5288 for the mail client.
 *
 * Scope (exactly this, no more):
 *   - TLS 1.2 only, one cipher suite:
 *     TLS_RSA_WITH_AES_128_GCM_SHA256 (0x009C) - RSA key
 *     exchange, AES-128-GCM records, SHA-256 PRF.
 *   - Client side only. No session resumption, no client
 *     certificates, no renegotiation (a HelloRequest is
 *     ignored, a re-handshake is refused).
 *
 * Certificate verification policy: PINNING. The caller
 * supplies the SHA-256 fingerprint (hex) of the one
 * server certificate it trusts, from mail.conf. The
 * handshake hashes the certificate the server presents
 * and compares in constant time; a mismatch, or a
 * missing pin, aborts the connection before any mail
 * password is sent. There is deliberately no CA chain
 * validation and no hostname check: the pin identifies
 * the exact certificate, which is stronger than a name
 * check for a configured mail server, and the operator
 * renews the pin when the server renews its certificate.
 * Self-signed server certificates are therefore usable,
 * which is what local and small-site mail servers have.
 *
 * The Finished messages are verified both ways, so the
 * peer must hold the private key matching the pinned
 * certificate.
 *
 * Limits: certificate messages up to 8KB, records up to
 * 16KB, RSA keys up to 4096 bits. One connection at a
 * time (the record scratch is shared), which matches
 * the mail client's single stream.
 */
#include "tls.h"

#include "crypto.h"
#include "security.h"
#include "serial.h"
#include "string.h"

/* ================= SHA-256 / HMAC / PRF ================= */

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len,
                        const uint8_t *data2, size_t data2_len,
                        uint8_t out[32]) {
    uint8_t k[64];
    uint8_t kopad[64];
    uint8_t kipad[64];
    for (int i = 0; i < 64; i++) {
        k[i] = 0;
    }
    if (key_len > 64) {
        crypto_sha256(key, key_len, k);
    } else {
        for (size_t i = 0; i < key_len; i++) {
            k[i] = key[i];
        }
    }
    for (int i = 0; i < 64; i++) {
        kopad[i] = (uint8_t)(k[i] ^ 0x5C);
        kipad[i] = (uint8_t)(k[i] ^ 0x36);
    }
    crypto_sha256_ctx ctx;
    uint8_t inner[32];
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, kipad, 64);
    if (data_len > 0) {
        crypto_sha256_update(&ctx, data, data_len);
    }
    if (data2_len > 0) {
        crypto_sha256_update(&ctx, data2, data2_len);
    }
    crypto_sha256_final(&ctx, inner);
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, kopad, 64);
    crypto_sha256_update(&ctx, inner, 32);
    crypto_sha256_final(&ctx, out);
}

void tls_prf(const uint8_t *secret, size_t secret_len,
             const char *label, const uint8_t *seed,
             size_t seed_len, uint8_t *out, size_t out_len) {
    /* A(1) = HMAC(secret, label || seed); each block is
     * HMAC(secret, A(i) || label || seed). */
    size_t label_len = 0;
    while (label[label_len] != '\0') {
        label_len++;
    }
    uint8_t a[32];
    /* A(1) = HMAC(secret, label || seed), via the two-part
     * HMAC helper. */
    hmac_sha256(secret, secret_len, (const uint8_t *)label,
                label_len, seed, seed_len, a);
    size_t done = 0;
    while (done < out_len) {
        uint8_t block[32];
        /* block = HMAC(secret, A || label || seed): the
         * two-part helper takes (A, label||seed) by using
         * a scratch concatenation for the second part. */
        uint8_t ls[128];
        size_t ls_len = 0;
        for (size_t i = 0; i < label_len && ls_len < sizeof(ls);
             i++) {
            ls[ls_len++] = (uint8_t)label[i];
        }
        for (size_t i = 0; i < seed_len && ls_len < sizeof(ls);
             i++) {
            ls[ls_len++] = seed[i];
        }
        hmac_sha256(secret, secret_len, a, 32, ls, ls_len, block);
        size_t take = out_len - done;
        if (take > 32) {
            take = 32;
        }
        for (size_t i = 0; i < take; i++) {
            out[done + i] = block[i];
        }
        done += take;
        if (done < out_len) {
            hmac_sha256(secret, secret_len, a, 32, NULL, 0, a);
        }
    }
}

/* ================= AES-128 (encrypt only) ================= */

/* The AES S-box (the standard constant table, FIPS-197). */
static const uint8_t AES_SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01,
    0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d,
    0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
    0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7,
    0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e,
    0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
    0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb,
    0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c,
    0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c,
    0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d,
    0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
    0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3,
    0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
    0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9,
    0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99,
    0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static uint8_t aes_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00));
}

typedef struct {
    uint8_t rk[176]; /* 11 round keys */
} aes128_ctx;

static void aes128_expand(aes128_ctx *c, const uint8_t key[16]) {
    static const uint8_t RCON[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                     0x20, 0x40, 0x80, 0x1B, 0x36};
    for (int i = 0; i < 16; i++) {
        c->rk[i] = key[i];
    }
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        for (int j = 0; j < 4; j++) {
            t[j] = c->rk[(i - 1) * 4 + j];
        }
        if (i % 4 == 0) {
            uint8_t r0 = t[0];
            t[0] = (uint8_t)(AES_SBOX[t[1]] ^ RCON[i / 4 - 1]);
            t[1] = AES_SBOX[t[2]];
            t[2] = AES_SBOX[t[3]];
            t[3] = AES_SBOX[r0];
        }
        for (int j = 0; j < 4; j++) {
            c->rk[i * 4 + j] =
                (uint8_t)(c->rk[(i - 4) * 4 + j] ^ t[j]);
        }
    }
}

static void aes128_encrypt(const aes128_ctx *c, const uint8_t in[16],
                           uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) {
        s[i] = (uint8_t)(in[i] ^ c->rk[i]);
    }
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) {
            s[i] = AES_SBOX[s[i]];
        }
        /* ShiftRows (state is column-major: s[col*4+row]). */
        uint8_t t;
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        t = s[2]; s[2] = s[10]; s[10] = t;
        t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3];
        s[3] = t;
        if (round < 10) {
            for (int col = 0; col < 4; col++) {
                uint8_t *v = &s[col * 4];
                uint8_t a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3];
                v[0] = (uint8_t)(aes_xtime(a0) ^
                                 (aes_xtime(a1) ^ a1) ^ a2 ^ a3);
                v[1] = (uint8_t)(a0 ^ aes_xtime(a1) ^
                                 (aes_xtime(a2) ^ a2) ^ a3);
                v[2] = (uint8_t)(a0 ^ a1 ^ aes_xtime(a2) ^
                                 (aes_xtime(a3) ^ a3));
                v[3] = (uint8_t)((aes_xtime(a0) ^ a0) ^ a1 ^ a2 ^
                                 aes_xtime(a3));
            }
        }
        for (int i = 0; i < 16; i++) {
            s[i] = (uint8_t)(s[i] ^ c->rk[round * 16 + i]);
        }
    }
    for (int i = 0; i < 16; i++) {
        out[i] = s[i];
    }
}

/* ================= GCM ================= */

static void ghash_mult(uint8_t x[16], const uint8_t h[16]) {
    /* x = x * h in GF(2^128), bit by bit (SP 800-38D). */
    uint8_t z[16];
    uint8_t v[16];
    for (int i = 0; i < 16; i++) {
        z[i] = 0;
        v[i] = h[i];
    }
    for (int i = 0; i < 128; i++) {
        if ((x[i / 8] >> (7 - (i % 8))) & 1) {
            for (int j = 0; j < 16; j++) {
                z[j] ^= v[j];
            }
        }
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) {
            v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
        }
        v[0] >>= 1;
        if (lsb) {
            v[0] ^= 0xE1;
        }
    }
    for (int i = 0; i < 16; i++) {
        x[i] = z[i];
    }
}

static void ghash_update(uint8_t y[16], const uint8_t h[16],
                         const uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > 16) {
            take = 16;
        }
        for (size_t i = 0; i < take; i++) {
            y[i] ^= data[off + i];
        }
        ghash_mult(y, h);
        off += take;
    }
}

static void gcm_crypt(const aes128_ctx *ctx, const uint8_t j0[16],
                      const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t ctr[16];
    for (int i = 0; i < 16; i++) {
        ctr[i] = j0[i];
    }
    size_t off = 0;
    while (off < len) {
        /* Increment the low 32 bits of the counter. */
        uint32_t v = ((uint32_t)ctr[12] << 24) |
                     ((uint32_t)ctr[13] << 16) |
                     ((uint32_t)ctr[14] << 8) | ctr[15];
        v++;
        ctr[12] = (uint8_t)(v >> 24);
        ctr[13] = (uint8_t)(v >> 16);
        ctr[14] = (uint8_t)(v >> 8);
        ctr[15] = (uint8_t)v;
        uint8_t ks[16];
        aes128_encrypt(ctx, ctr, ks);
        size_t take = len - off;
        if (take > 16) {
            take = 16;
        }
        for (size_t i = 0; i < take; i++) {
            out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        }
        off += take;
    }
}

static void gcm_tag(const aes128_ctx *ctx, const uint8_t h[16],
                    const uint8_t j0[16], const uint8_t *aad,
                    size_t aad_len, const uint8_t *ct, size_t len,
                    uint8_t tag[16]) {
    uint8_t y[16];
    for (int i = 0; i < 16; i++) {
        y[i] = 0;
    }
    if (aad_len > 0) {
        ghash_update(y, h, aad, aad_len);
    }
    if (len > 0) {
        ghash_update(y, h, ct, len);
    }
    uint8_t lens[16];
    uint64_t a_bits = (uint64_t)aad_len * 8;
    uint64_t c_bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(a_bits >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(c_bits >> (56 - 8 * i));
    }
    ghash_update(y, h, lens, 16);
    uint8_t ej0[16];
    aes128_encrypt(ctx, j0, ej0);
    for (int i = 0; i < 16; i++) {
        tag[i] = (uint8_t)(y[i] ^ ej0[i]);
    }
}

static void gcm_setup(const uint8_t key[16],
                      const uint8_t nonce[12], aes128_ctx *ctx,
                      uint8_t h[16], uint8_t j0[16]) {
    aes128_expand(ctx, key);
    uint8_t zero[16];
    for (int i = 0; i < 16; i++) {
        zero[i] = 0;
    }
    aes128_encrypt(ctx, zero, h);
    for (int i = 0; i < 12; i++) {
        j0[i] = nonce[i];
    }
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;
}

void tls_gcm_encrypt(const uint8_t key[16],
                     const uint8_t nonce[12], const uint8_t *pt,
                     size_t len, const uint8_t *aad,
                     size_t aad_len, uint8_t *ct,
                     uint8_t tag[16]) {
    aes128_ctx ctx;
    uint8_t h[16], j0[16];
    gcm_setup(key, nonce, &ctx, h, j0);
    if (len > 0) {
        gcm_crypt(&ctx, j0, pt, len, ct);
    }
    gcm_tag(&ctx, h, j0, aad, aad_len, ct, len, tag);
}

int tls_gcm_decrypt(const uint8_t key[16],
                    const uint8_t nonce[12], const uint8_t *ct,
                    size_t len, const uint8_t *aad,
                    size_t aad_len, const uint8_t tag[16],
                    uint8_t *pt) {
    aes128_ctx ctx;
    uint8_t h[16], j0[16];
    gcm_setup(key, nonce, &ctx, h, j0);
    uint8_t want[16];
    gcm_tag(&ctx, h, j0, aad, aad_len, ct, len, want);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) {
        diff |= (uint8_t)(want[i] ^ tag[i]);
    }
    if (diff != 0) {
        return -1;
    }
    if (len > 0) {
        gcm_crypt(&ctx, j0, ct, len, pt);
    }
    return 0;
}

/* ================= RSA public operation ================= */

/* Fixed-size bignum: up to 4096 bits, little-endian limbs. */
#define BN_LIMBS 128

typedef struct {
    uint32_t w[BN_LIMBS];
} bignum;

static void bn_from_bytes(bignum *b, const uint8_t *in, size_t len) {
    for (int i = 0; i < BN_LIMBS; i++) {
        b->w[i] = 0;
    }
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        b->w[bit / 32] |= (uint32_t)in[i] << (bit % 32);
    }
}

static void bn_to_bytes(const bignum *b, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        out[i] = (uint8_t)(b->w[bit / 32] >> (bit % 32));
    }
}

static int bn_cmp(const bignum *a, const bignum *b) {
    for (int i = BN_LIMBS - 1; i >= 0; i--) {
        if (a->w[i] != b->w[i]) {
            return a->w[i] > b->w[i] ? 1 : -1;
        }
    }
    return 0;
}

static void bn_sub(bignum *a, const bignum *b) { /* a -= b, a >= b */
    uint64_t borrow = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        uint64_t v = (uint64_t)a->w[i] - b->w[i] - borrow;
        a->w[i] = (uint32_t)v;
        borrow = (v >> 63) & 1;
    }
}

static void bn_shl1(bignum *a) {
    uint32_t carry = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        uint32_t next = a->w[i] >> 31;
        a->w[i] = (a->w[i] << 1) | carry;
        carry = next;
    }
}

/* r = a mod n by binary long division. a may use up to
 * 2*BN_LIMBS limbs worth of bits conceptually; here a is
 * given as a 2*BN_LIMBS-limb array. */
static void bn_mod_wide(uint32_t r_out[BN_LIMBS],
                        const uint32_t a[2 * BN_LIMBS],
                        const bignum *n) {
    bignum r;
    for (int i = 0; i < BN_LIMBS; i++) {
        r.w[i] = 0;
    }
    /* Start at the dividend's highest set bit: for a
     * 2048-bit key the product has at most 4096 bits,
     * which halves the work. */
    int top = 2 * BN_LIMBS * 32 - 1;
    while (top > 0 && ((a[top / 32] >> (top % 32)) & 1u) == 0) {
        top--;
    }
    for (int bit = top; bit >= 0; bit--) {
        bn_shl1(&r);
        r.w[0] |= (a[bit / 32] >> (bit % 32)) & 1u;
        if (bn_cmp(&r, n) >= 0) {
            bn_sub(&r, n);
        }
    }
    for (int i = 0; i < BN_LIMBS; i++) {
        r_out[i] = r.w[i];
    }
}

static void bn_mulmod(bignum *r, const bignum *a, const bignum *b,
                      const bignum *n) {
    uint32_t prod[2 * BN_LIMBS];
    for (int i = 0; i < 2 * BN_LIMBS; i++) {
        prod[i] = 0;
    }
    for (int i = 0; i < BN_LIMBS; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < BN_LIMBS; j++) {
            uint64_t v = (uint64_t)a->w[i] * b->w[j] + prod[i + j] +
                         carry;
            prod[i + j] = (uint32_t)v;
            carry = v >> 32;
        }
        prod[i + BN_LIMBS] = (uint32_t)carry;
    }
    bn_mod_wide(r->w, prod, n);
}

int tls_rsa_public(const uint8_t *mod, size_t mod_len,
                   const uint8_t *exp, size_t exp_len,
                   const uint8_t *in, uint8_t *out) {
    if (mod_len == 0 || mod_len > BN_LIMBS * 4 || exp_len == 0 ||
        exp_len > 8) {
        return -1;
    }
    bignum n, base, result;
    bn_from_bytes(&n, mod, mod_len);
    bn_from_bytes(&base, in, mod_len);
    if (bn_cmp(&base, &n) >= 0) {
        return -1;
    }
    for (int i = 0; i < BN_LIMBS; i++) {
        result.w[i] = 0;
    }
    result.w[0] = 1;
    /* Square and multiply over the exponent's bits. */
    uint64_t e = 0;
    for (size_t i = 0; i < exp_len; i++) {
        e = (e << 8) | exp[i];
    }
    int top = 0;
    for (int i = 63; i >= 0; i--) {
        if ((e >> i) & 1) {
            top = i;
            break;
        }
    }
    for (int i = top; i >= 0; i--) {
        bn_mulmod(&result, &result, &result, &n);
        if ((e >> i) & 1) {
            bn_mulmod(&result, &result, &base, &n);
        }
    }
    bn_to_bytes(&result, out, mod_len);
    return 0;
}

/* ================= X.509 (DER) parsing ================= */

typedef struct {
    const uint8_t *p;
    size_t len;
} der_cursor;

/* Read one TLV. On success *tag, *val, *val_len describe
 * the value and the cursor moves past the element. */
static int der_read(der_cursor *c, uint8_t *tag,
                    const uint8_t **val, size_t *val_len) {
    if (c->len < 2) {
        return -1;
    }
    *tag = c->p[0];
    size_t hdr = 2;
    size_t n;
    if ((c->p[1] & 0x80) == 0) {
        n = c->p[1];
    } else {
        int nb = c->p[1] & 0x7F;
        if (nb == 0 || nb > 2 || c->len < 2 + (size_t)nb) {
            return -1;
        }
        n = 0;
        for (int i = 0; i < nb; i++) {
            n = (n << 8) | c->p[2 + i];
        }
        hdr = 2 + (size_t)nb;
    }
    if (c->len < hdr + n) {
        return -1;
    }
    *val = c->p + hdr;
    *val_len = n;
    c->p += hdr + n;
    c->len -= hdr + n;
    return 0;
}

static int der_expect_seq(der_cursor *c, der_cursor *inner) {
    uint8_t tag;
    const uint8_t *val;
    size_t val_len;
    if (der_read(c, &tag, &val, &val_len) != 0 || tag != 0x30) {
        return -1;
    }
    inner->p = val;
    inner->len = val_len;
    return 0;
}

int tls_x509_rsa_key(const uint8_t *der, size_t len,
                     const uint8_t **mod, size_t *mod_len,
                     const uint8_t **exp, size_t *exp_len) {
    {
        der_cursor top2 = {der, len};
        der_cursor cert2, tbs2;
        if (der_expect_seq(&top2, &cert2) != 0 ||
            der_expect_seq(&cert2, &tbs2) != 0) {
            return -1;
        }
        uint8_t tag;
        const uint8_t *val;
        size_t val_len;
        /* Optional [0] version. */
        der_cursor peek = tbs2;
        if (der_read(&peek, &tag, &val, &val_len) == 0 &&
            tag == 0xA0) {
            tbs2 = peek;
        }
        /* serial, signature, issuer, validity, subject. */
        for (int i = 0; i < 5; i++) {
            if (der_read(&tbs2, &tag, &val, &val_len) != 0) {
                return -1;
            }
        }
        der_cursor spki2, alg2;
        if (der_expect_seq(&tbs2, &spki2) != 0) {
            return -1;
        }
        if (der_expect_seq(&spki2, &alg2) != 0) {
            return -1;
        }
        /* Algorithm OID must be rsaEncryption
         * (1.2.840.113549.1.1.1). */
        der_cursor algv = alg2;
        if (der_read(&algv, &tag, &val, &val_len) != 0 ||
            tag != 0x06 || val_len != 9 || val[0] != 0x2A ||
            val[1] != 0x86 || val[2] != 0x48 || val[3] != 0x86 ||
            val[4] != 0xF7 || val[5] != 0x0D || val[6] != 0x01 ||
            val[7] != 0x01 || val[8] != 0x01) {
            return -1;
        }
        /* BIT STRING: first byte is the unused-bit count. */
        if (der_read(&spki2, &tag, &val, &val_len) != 0 ||
            tag != 0x03 || val_len < 1 || val[0] != 0) {
            return -1;
        }
        der_cursor bits = {val + 1, val_len - 1};
        der_cursor rsa;
        if (der_expect_seq(&bits, &rsa) != 0) {
            return -1;
        }
        if (der_read(&rsa, &tag, &val, &val_len) != 0 ||
            tag != 0x02) {
            return -1;
        }
        /* INTEGERs may carry a leading zero byte. */
        if (val_len > 1 && val[0] == 0) {
            val++;
            val_len--;
        }
        *mod = val;
        *mod_len = val_len;
        if (der_read(&rsa, &tag, &val, &val_len) != 0 ||
            tag != 0x02) {
            return -1;
        }
        if (val_len > 1 && val[0] == 0) {
            val++;
            val_len--;
        }
        *exp = val;
        *exp_len = val_len;
        return 0;
    }
}

/* ================= Record layer ================= */

#define TLS_MAX_RECORD 16384
#define TLS_HS_BUF 8192
#define TLS_TIMEOUT 20 /* seconds per network wait */

static uint8_t tls_rec[TLS_MAX_RECORD + 32];
static uint8_t tls_hs[TLS_HS_BUF];
static size_t tls_hs_len; /* reassembly buffer fill */
static size_t tls_hs_pos; /* consumed prefix */
/* Decrypted application record not yet fully served. */
static size_t tls_app_len;
static size_t tls_app_pos;

static int tls_fail(tls_conn *c, const char *msg) {
    size_t i = 0;
    while (msg[i] != '\0' && i + 1 < sizeof(c->err)) {
        c->err[i] = msg[i];
        i++;
    }
    c->err[i] = '\0';
    return -1;
}

const char *tls_last_error(const tls_conn *c) {
    return c->err;
}

static int read_exact(tls_conn *c, uint8_t *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = net_tcp_recv(&c->tcp, out + got, n - got,
                             TLS_TIMEOUT);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

/* Read one record. Returns the payload length (plaintext
 * when the receive side is secure), or -1. */
static int read_record(tls_conn *c, uint8_t *type_out) {
    uint8_t hdr[5];
    if (read_exact(c, hdr, 5) != 0) {
        return tls_fail(c, "record read failed");
    }
    uint8_t type = hdr[0];
    size_t len = ((size_t)hdr[3] << 8) | hdr[4];
    if (len > sizeof(tls_rec)) {
        return tls_fail(c, "record too large");
    }
    if (len > 0 && read_exact(c, tls_rec, len) != 0) {
        return tls_fail(c, "record body read failed");
    }
    if (!c->recv_secure) {
        *type_out = type;
        return (int)len;
    }
    /* Encrypted: explicit nonce (8) || ciphertext || tag (16). */
    if (len < 8 + 16) {
        return tls_fail(c, "short encrypted record");
    }
    size_t ct_len = len - 8 - 16;
    if (ct_len > TLS_MAX_RECORD) {
        return tls_fail(c, "record too large");
    }
    uint8_t nonce[12];
    for (int i = 0; i < 4; i++) {
        nonce[i] = c->server_iv[i];
    }
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = tls_rec[i];
    }
    uint8_t aad[13];
    for (int i = 0; i < 8; i++) {
        aad[i] = (uint8_t)(c->recv_seq >> (56 - 8 * i));
    }
    aad[8] = type;
    aad[9] = 0x03;
    aad[10] = 0x03;
    aad[11] = (uint8_t)(ct_len >> 8);
    aad[12] = (uint8_t)ct_len;
    if (tls_gcm_decrypt(c->server_key, nonce, tls_rec + 8, ct_len,
                        aad, 13, tls_rec + 8 + ct_len, tls_rec) !=
        0) {
        return tls_fail(c, "record authentication failed");
    }
    c->recv_seq++;
    *type_out = type;
    return (int)ct_len;
}

static int write_record(tls_conn *c, uint8_t type,
                        const uint8_t *data, size_t len) {
    uint8_t hdr[5];
    hdr[0] = type;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    if (!c->send_secure) {
        hdr[3] = (uint8_t)(len >> 8);
        hdr[4] = (uint8_t)len;
        if (net_tcp_send(&c->tcp, (const char *)hdr, 5) != 0 ||
            (len > 0 &&
             net_tcp_send(&c->tcp, (const char *)data, len) != 0)) {
            return tls_fail(c, "record send failed");
        }
        return 0;
    }
    /* Encrypt into the scratch: explicit(8) || ct || tag. */
    if (len > TLS_MAX_RECORD) {
        return tls_fail(c, "record too large to send");
    }
    uint8_t nonce[12];
    for (int i = 0; i < 4; i++) {
        nonce[i] = c->client_iv[i];
        tls_rec[i] = 0; /* explicit part filled below */
    }
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(c->send_seq >> (56 - 8 * i));
        nonce[4 + i] = b;
        tls_rec[i] = b;
    }
    uint8_t aad[13];
    for (int i = 0; i < 8; i++) {
        aad[i] = nonce[4 + i];
    }
    aad[8] = type;
    aad[9] = 0x03;
    aad[10] = 0x03;
    aad[11] = (uint8_t)(len >> 8);
    aad[12] = (uint8_t)len;
    uint8_t tag[16];
    tls_gcm_encrypt(c->client_key, nonce, data, len, aad, 13,
                    tls_rec + 8, tag);
    for (int i = 0; i < 16; i++) {
        tls_rec[8 + len + i] = tag[i];
    }
    size_t total = 8 + len + 16;
    hdr[3] = (uint8_t)(total >> 8);
    hdr[4] = (uint8_t)total;
    if (net_tcp_send(&c->tcp, (const char *)hdr, 5) != 0 ||
        net_tcp_send(&c->tcp, (const char *)tls_rec, total) != 0) {
        return tls_fail(c, "record send failed");
    }
    c->send_seq++;
    return 0;
}

/* ================= Handshake ================= */

/* Send one handshake message (type + body) and hash it. */
static int hs_send(tls_conn *c, crypto_sha256_ctx *hash,
                   uint8_t type, const uint8_t *body,
                   size_t body_len) {
    uint8_t msg[4 + 1024];
    if (body_len + 4 > sizeof(msg)) {
        return tls_fail(c, "handshake message too large");
    }
    msg[0] = type;
    msg[1] = (uint8_t)(body_len >> 16);
    msg[2] = (uint8_t)(body_len >> 8);
    msg[3] = (uint8_t)body_len;
    for (size_t i = 0; i < body_len; i++) {
        msg[4 + i] = body[i];
    }
    crypto_sha256_update(hash, msg, 4 + body_len);
    return write_record(c, 22, msg, 4 + body_len);
}

/* Take the next handshake message from the reassembly
 * buffer, reading more records when needed. On success
 * returns the message type and sets msg/msg_len to the
 * full message (header + body); the caller hashes it. */
static int hs_next(tls_conn *c, uint8_t *type_out,
                   const uint8_t **msg, size_t *msg_len) {
    for (;;) {
        size_t avail = tls_hs_len - tls_hs_pos;
        if (avail >= 4) {
            size_t body = ((size_t)tls_hs[tls_hs_pos + 1] << 16) |
                          ((size_t)tls_hs[tls_hs_pos + 2] << 8) |
                          tls_hs[tls_hs_pos + 3];
            if (body > TLS_HS_BUF) {
                return tls_fail(c, "handshake message too large");
            }
            if (avail >= 4 + body) {
                *type_out = tls_hs[tls_hs_pos];
                *msg = tls_hs + tls_hs_pos;
                *msg_len = 4 + body;
                tls_hs_pos += 4 + body;
                if (tls_hs_pos == tls_hs_len) {
                    tls_hs_pos = 0;
                    tls_hs_len = 0;
                }
                return 0;
            }
        }
        /* Compact and read one more handshake record. */
        if (tls_hs_pos > 0) {
            size_t keep = tls_hs_len - tls_hs_pos;
            for (size_t i = 0; i < keep; i++) {
                tls_hs[i] = tls_hs[tls_hs_pos + i];
            }
            tls_hs_len = keep;
            tls_hs_pos = 0;
        }
        uint8_t rtype = 0;
        int n = read_record(c, &rtype);
        if (n < 0) {
            return -1;
        }
        if (rtype == 21) {
            return tls_fail(c, "alert from server in handshake");
        }
        if (rtype != 22) {
            return tls_fail(c, "unexpected record in handshake");
        }
        if (tls_hs_len + (size_t)n > TLS_HS_BUF) {
            return tls_fail(c, "handshake flight too large");
        }
        for (int i = 0; i < n; i++) {
            tls_hs[tls_hs_len + (size_t)i] = tls_rec[i];
        }
        tls_hs_len += (size_t)n;
    }
}

static int hex_val(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/* 秘密の消去 (volatile 経由で最適化に消させない)。 */
static void tls_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        v[i] = 0;
    }
}

static int tls_open_impl(tls_conn *c, const char *host,
                         uint16_t port, const char *pin_hex,
                         char *fp_hex_out) {
    for (size_t i = 0; i < sizeof(*c); i++) {
        ((uint8_t *)c)[i] = 0;
    }
    tls_hs_len = 0;
    tls_hs_pos = 0;
    tls_app_len = 0;
    tls_app_pos = 0;
    /* The pin is mandatory for tls_open: without one there
     * is nothing to verify the server against. The probe
     * variant (fp_hex_out != NULL, pin_hex == NULL) skips
     * the comparison but exports the fingerprint so the
     * caller can apply its own policy (the browser's
     * trust-on-first-use store) BEFORE sending any data. */
    uint8_t pin[32];
    int have_pin = pin_hex != NULL;
    if (!have_pin && fp_hex_out == NULL) {
        return tls_fail(c, "no certificate pin configured");
    }
    if (have_pin) {
        for (int i = 0; i < 32; i++) {
            int hi = hex_val(pin_hex[2 * i]);
            int lo = hex_val(pin_hex[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                return tls_fail(
                    c, "certificate pin is not 64 hex digits");
            }
            pin[i] = (uint8_t)((hi << 4) | lo);
        }
        if (pin_hex[64] != '\0') {
            return tls_fail(
                c, "certificate pin is not 64 hex digits");
        }
    }
    /* 接続ごとにエントロピープールを再種入れする
     * (起動時の種だけに全秘密をぶら下げない)。 */
    entropy_reseed();
    if (net_tcp_open(host, port, &c->tcp) != 0) {
        return tls_fail(c, "TCP connect failed");
    }

    crypto_sha256_ctx hash;
    crypto_sha256_init(&hash);

    /* ---- ClientHello ---- */
    uint8_t client_random[32];
    entropy_bytes(client_random, 32);
    uint8_t ch_body[512];
    size_t p = 0;
    ch_body[p++] = 0x03;
    ch_body[p++] = 0x03;
    for (int i = 0; i < 32; i++) {
        ch_body[p++] = client_random[i];
    }
    ch_body[p++] = 0; /* session id: empty */
    ch_body[p++] = 0;
    ch_body[p++] = 2; /* cipher suites length */
    ch_body[p++] = 0x00;
    ch_body[p++] = 0x9C; /* TLS_RSA_WITH_AES_128_GCM_SHA256 */
    ch_body[p++] = 1; /* compression methods */
    ch_body[p++] = 0;
    /* Extensions: server_name, signature_algorithms. */
    size_t host_len = 0;
    while (host[host_len] != '\0') {
        host_len++;
    }
    size_t ext_at = p;
    p += 2; /* extensions length, patched below */
    ch_body[p++] = 0x00;
    ch_body[p++] = 0x00; /* type: server_name */
    size_t sni_len = 2 + 1 + 2 + host_len;
    ch_body[p++] = (uint8_t)(sni_len >> 8);
    ch_body[p++] = (uint8_t)sni_len;
    ch_body[p++] = (uint8_t)((1 + 2 + host_len) >> 8);
    ch_body[p++] = (uint8_t)(1 + 2 + host_len);
    ch_body[p++] = 0; /* name type: host_name */
    ch_body[p++] = (uint8_t)(host_len >> 8);
    ch_body[p++] = (uint8_t)host_len;
    for (size_t i = 0; i < host_len; i++) {
        ch_body[p++] = (uint8_t)host[i];
    }
    /* Extension: signature_algorithms (13). Modern
     * servers (OpenSSL 3 at security level >= 1) refuse
     * a TLS 1.2 handshake without it ("no suitable
     * signature algorithm"): with no list they fall back
     * to legacy SHA-1 defaults that their policy rejects.
     * We offer RSA with SHA-256/384/512, matching the
     * RSA certificates this client pins. */
    ch_body[p++] = 0x00;
    ch_body[p++] = 0x0D; /* type: signature_algorithms */
    ch_body[p++] = 0x00;
    ch_body[p++] = 0x08; /* extension data length */
    ch_body[p++] = 0x00;
    ch_body[p++] = 0x06; /* list length */
    ch_body[p++] = 0x04;
    ch_body[p++] = 0x01; /* sha256 + rsa */
    ch_body[p++] = 0x05;
    ch_body[p++] = 0x01; /* sha384 + rsa */
    ch_body[p++] = 0x06;
    ch_body[p++] = 0x01; /* sha512 + rsa */
    size_t ext_total = p - ext_at - 2;
    ch_body[ext_at] = (uint8_t)(ext_total >> 8);
    ch_body[ext_at + 1] = (uint8_t)ext_total;
    if (hs_send(c, &hash, 1, ch_body, p) != 0) {
        return -1;
    }

    /* ---- ServerHello ---- */
    uint8_t server_random[32];
    {
        uint8_t mtype;
        const uint8_t *msg;
        size_t msg_len;
        if (hs_next(c, &mtype, &msg, &msg_len) != 0) {
            return -1;
        }
        if (mtype != 2 || msg_len < 4 + 2 + 32 + 1 + 2 + 1) {
            return tls_fail(c, "bad ServerHello");
        }
        crypto_sha256_update(&hash, msg, msg_len);
        const uint8_t *b = msg + 4;
        if (b[0] != 0x03 || b[1] != 0x03) {
            return tls_fail(c, "server is not TLS 1.2");
        }
        for (int i = 0; i < 32; i++) {
            server_random[i] = b[2 + i];
        }
        size_t q = 2 + 32;
        q += 1 + b[q]; /* session id */
        if (q + 3 > msg_len - 4) {
            return tls_fail(c, "bad ServerHello");
        }
        if (b[q] != 0x00 || b[q + 1] != 0x9C) {
            return tls_fail(c, "server chose another cipher suite");
        }
        if (b[q + 2] != 0) {
            return tls_fail(c, "server chose compression");
        }
    }

    /* ---- Certificate: pin check + RSA key ---- */
    const uint8_t *rsa_mod;
    size_t rsa_mod_len;
    const uint8_t *rsa_exp;
    size_t rsa_exp_len;
    {
        uint8_t mtype;
        const uint8_t *msg;
        size_t msg_len;
        if (hs_next(c, &mtype, &msg, &msg_len) != 0) {
            return -1;
        }
        if (mtype != 11 || msg_len < 4 + 3 + 3) {
            return tls_fail(c, "bad Certificate message");
        }
        crypto_sha256_update(&hash, msg, msg_len);
        const uint8_t *b = msg + 4;
        size_t total = ((size_t)b[0] << 16) | ((size_t)b[1] << 8) |
                       b[2];
        if (total + 3 > msg_len - 4) {
            return tls_fail(c, "bad Certificate message");
        }
        size_t cert_len = ((size_t)b[3] << 16) |
                          ((size_t)b[4] << 8) | b[5];
        const uint8_t *cert = b + 6;
        if (cert_len + 6 > msg_len - 4) {
            return tls_fail(c, "bad Certificate message");
        }
        uint8_t digest[32];
        crypto_sha256(cert, cert_len, digest);
        if (fp_hex_out != NULL) {
            static const char hexd[17] = "0123456789abcdef";
            for (int i = 0; i < 32; i++) {
                fp_hex_out[2 * i] = hexd[digest[i] >> 4];
                fp_hex_out[2 * i + 1] = hexd[digest[i] & 15];
            }
            fp_hex_out[64] = '\0';
        }
        if (have_pin) {
            uint8_t diff = 0;
            for (int i = 0; i < 32; i++) {
                diff |= (uint8_t)(digest[i] ^ pin[i]);
            }
            if (diff != 0) {
                return tls_fail(
                    c, "certificate does not match the pin");
            }
        }
        if (tls_x509_rsa_key(cert, cert_len, &rsa_mod,
                             &rsa_mod_len, &rsa_exp,
                             &rsa_exp_len) != 0) {
            return tls_fail(c, "no RSA key in certificate");
        }
    }

    /* ---- ServerHelloDone ---- */
    {
        uint8_t mtype;
        const uint8_t *msg;
        size_t msg_len;
        if (hs_next(c, &mtype, &msg, &msg_len) != 0) {
            return -1;
        }
        if (mtype != 14) {
            return tls_fail(c, "expected ServerHelloDone");
        }
        crypto_sha256_update(&hash, msg, msg_len);
    }

    /* サーバの乱数もプールへ混ぜてから premaster を引く
     * (この接続だけの新しさが秘密側にも入る)。 */
    for (int i = 0; i < 8; i++) {
        uint32_t w = ((uint32_t)server_random[i * 4] << 24) |
                     ((uint32_t)server_random[i * 4 + 1] << 16) |
                     ((uint32_t)server_random[i * 4 + 2] << 8) |
                     (uint32_t)server_random[i * 4 + 3];
        entropy_add(w);
    }

    /* ---- ClientKeyExchange ---- */
    uint8_t premaster[48];
    premaster[0] = 0x03;
    premaster[1] = 0x03;
    entropy_bytes(premaster + 2, 46);
    uint8_t enc[512];
    if (rsa_mod_len > sizeof(enc)) {
        return tls_fail(c, "RSA key too large");
    }
    if (rsa_mod_len < 256) {
        return tls_fail(c, "RSA key too small (need 2048 bits)");
    }
    {
        /* PKCS#1 v1.5 encryption padding:
         * 0x00 0x02 PS(>=8 nonzero random) 0x00 premaster. */
        size_t ps_len = rsa_mod_len - 48 - 3;
        enc[0] = 0x00;
        enc[1] = 0x02;
        entropy_bytes(enc + 2, ps_len);
        for (size_t i = 0; i < ps_len; i++) {
            while (enc[2 + i] == 0) {
                uint8_t r;
                entropy_bytes(&r, 1);
                enc[2 + i] = r;
            }
        }
        enc[2 + ps_len] = 0x00;
        for (int i = 0; i < 48; i++) {
            enc[3 + ps_len + i] = premaster[i];
        }
        uint8_t cke_body[514];
        cke_body[0] = (uint8_t)(rsa_mod_len >> 8);
        cke_body[1] = (uint8_t)rsa_mod_len;
        if (tls_rsa_public(rsa_mod, rsa_mod_len, rsa_exp,
                           rsa_exp_len, enc, cke_body + 2) != 0) {
            return tls_fail(c, "RSA encryption failed");
        }
        if (hs_send(c, &hash, 16, cke_body, 2 + rsa_mod_len) !=
            0) {
            return -1;
        }
    }

    /* ---- Keys ---- */
    uint8_t master[48];
    {
        uint8_t seed[64];
        for (int i = 0; i < 32; i++) {
            seed[i] = client_random[i];
            seed[32 + i] = server_random[i];
        }
        tls_prf(premaster, 48, "master secret", seed, 64,
                master, 48);
        for (int i = 0; i < 32; i++) {
            seed[i] = server_random[i];
            seed[32 + i] = client_random[i];
        }
        uint8_t key_block[40];
        tls_prf(master, 48, "key expansion", seed, 64,
                key_block, 40);
        for (int i = 0; i < 16; i++) {
            c->client_key[i] = key_block[i];
            c->server_key[i] = key_block[16 + i];
        }
        for (int i = 0; i < 4; i++) {
            c->client_iv[i] = key_block[32 + i];
            c->server_iv[i] = key_block[36 + i];
        }
        tls_wipe(key_block, sizeof(key_block));
        tls_wipe(seed, sizeof(seed));
    }
    /* premaster は役目を終えたので消す。master は
     * Finished の計算でまだ使うので、握手の最後で消す。 */
    tls_wipe(premaster, sizeof(premaster));

    /* ---- ChangeCipherSpec + client Finished ---- */
    {
        uint8_t one = 1;
        if (write_record(c, 20, &one, 1) != 0) {
            return -1;
        }
    }
    c->send_secure = 1;
    c->send_seq = 0;
    {
        crypto_sha256_ctx snapshot = hash;
        uint8_t digest[32];
        crypto_sha256_final(&snapshot, digest);
        uint8_t verify[12];
        tls_prf(master, 48, "client finished", digest, 32,
                verify, 12);
        if (hs_send(c, &hash, 20, verify, 12) != 0) {
            return -1;
        }
    }

    /* ---- Server ChangeCipherSpec + Finished ---- */
    {
        uint8_t rtype = 0;
        int n = read_record(c, &rtype);
        if (n < 0) {
            return -1;
        }
        if (rtype != 20 || n != 1 || tls_rec[0] != 1) {
            return tls_fail(c, "expected server ChangeCipherSpec");
        }
    }
    c->recv_secure = 1;
    c->recv_seq = 0;
    {
        uint8_t mtype;
        const uint8_t *msg;
        size_t msg_len;
        if (hs_next(c, &mtype, &msg, &msg_len) != 0) {
            return -1;
        }
        if (mtype != 20 || msg_len != 4 + 12) {
            return tls_fail(c, "bad server Finished");
        }
        crypto_sha256_ctx snapshot = hash;
        uint8_t digest[32];
        crypto_sha256_final(&snapshot, digest);
        uint8_t want[12];
        tls_prf(master, 48, "server finished", digest, 32,
                want, 12);
        uint8_t diff = 0;
        for (int i = 0; i < 12; i++) {
            diff |= (uint8_t)(want[i] ^ msg[4 + i]);
        }
        if (diff != 0) {
            return tls_fail(c,
                            "server Finished check failed");
        }
    }
    tls_wipe(master, sizeof(master));
    c->secure = 1;
    serial_write_line(have_pin
                          ? "[tls] handshake ok (TLS 1.2, "
                            "TLS_RSA_WITH_AES_128_GCM_SHA256, "
                            "certificate pin matched)."
                          : "[tls] handshake ok (TLS 1.2, probe: "
                            "fingerprint exported for TOFU).");
    return 0;
}

int tls_open(tls_conn *c, const char *host, uint16_t port,
             const char *pin_hex) {
    return tls_open_impl(c, host, port, pin_hex, NULL);
}

int tls_open_probe(tls_conn *c, const char *host, uint16_t port,
                   char *fp_hex_out) {
    if (fp_hex_out == NULL) {
        return tls_fail(c, "probe needs a fingerprint buffer");
    }
    return tls_open_impl(c, host, port, NULL, fp_hex_out);
}

/* ================= Application data ================= */

int tls_send(tls_conn *c, const char *data, size_t len) {
    if (!c->secure) {
        return tls_fail(c, "send before handshake");
    }
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 4096) {
            chunk = 4096;
        }
        if (write_record(c, 23, (const uint8_t *)data + off,
                         chunk) != 0) {
            return -1;
        }
        off += chunk;
    }
    return 0;
}

/* Refill the plaintext buffer from one application
 * record. Returns 1 when bytes are available, 0 on a
 * clean close, -1 on error. */
static int tls_fill(tls_conn *c) {
    if (tls_app_pos < tls_app_len) {
        /* Serve the rest of the previous record first. */
        size_t avail = tls_app_len - tls_app_pos;
        size_t take = avail;
        if (take > sizeof(c->plain)) {
            take = sizeof(c->plain);
        }
        for (size_t i = 0; i < take; i++) {
            c->plain[i] = tls_rec[tls_app_pos + i];
        }
        tls_app_pos += take;
        c->plain_len = take;
        c->plain_pos = 0;
        return 1;
    }
    for (;;) {
        uint8_t rtype = 0;
        int n = read_record(c, &rtype);
        if (n < 0) {
            return -1;
        }
        if (rtype == 21) {
            /* Alert: close_notify (0) ends the stream;
             * anything else is fatal. */
            if (n >= 2 && tls_rec[1] == 0) {
                c->peer_closed = 1;
                return 0;
            }
            return tls_fail(c, "fatal alert from server");
        }
        if (rtype != 23) {
            return tls_fail(c, "unexpected record after handshake");
        }
        if (n == 0) {
            continue;
        }
        tls_app_len = (size_t)n;
        tls_app_pos = 0;
        size_t take = tls_app_len;
        if (take > sizeof(c->plain)) {
            take = sizeof(c->plain);
        }
        for (size_t i = 0; i < take; i++) {
            c->plain[i] = tls_rec[i];
        }
        tls_app_pos = take;
        c->plain_len = take;
        c->plain_pos = 0;
        return 1;
    }
}

int tls_recv(tls_conn *c, uint8_t *out, size_t cap) {
    if (c->plain_pos >= c->plain_len) {
        int f = tls_fill(c);
        if (f == 0) {
            return 0; /* clean close */
        }
        if (f < 0) {
            return -1;
        }
    }
    size_t n = c->plain_len - c->plain_pos;
    if (n > cap) {
        n = cap;
    }
    memcpy(out, c->plain + c->plain_pos, n);
    c->plain_pos += n;
    if (c->plain_pos >= c->plain_len) {
        c->plain_pos = 0;
        c->plain_len = 0;
    }
    return (int)n;
}

int tls_recv_line(tls_conn *c, char *out, size_t cap,
                  uint32_t timeout_sec) {
    (void)timeout_sec; /* record reads carry their own wait */
    if (c == NULL || out == NULL || cap < 2 || !c->secure) {
        return -1;
    }
    size_t n = 0;
    for (;;) {
        while (c->plain_pos < c->plain_len) {
            uint8_t ch = c->plain[c->plain_pos++];
            if (ch == '\n') {
                if (n > 0 && out[n - 1] == '\r') {
                    n--;
                }
                out[n] = '\0';
                return (int)n;
            }
            if (n + 1 < cap) {
                out[n++] = (char)ch;
            }
        }
        c->plain_pos = 0;
        c->plain_len = 0;
        if (c->peer_closed) {
            return -1;
        }
        int r = tls_fill(c);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return -1;
        }
    }
}

void tls_close(tls_conn *c) {
    if (c == NULL) {
        return;
    }
    if (c->secure) {
        uint8_t alert[2] = {1, 0}; /* warning, close_notify */
        (void)write_record(c, 21, alert, 2);
    }
    net_tcp_close(&c->tcp);
    /* セッション鍵と IV は接続が終わったら消す。 */
    tls_wipe(c->client_key, sizeof(c->client_key));
    tls_wipe(c->server_key, sizeof(c->server_key));
    tls_wipe(c->client_iv, sizeof(c->client_iv));
    tls_wipe(c->server_iv, sizeof(c->server_iv));
    c->secure = 0;
}
