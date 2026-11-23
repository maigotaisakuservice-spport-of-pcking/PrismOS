/* PrismOS — SHA-3 / SHAKE (Keccak-f1600) の実装。
 *
 * Keccak-f1600 は Keccak の参照実装と同じ 24 ラウンド
 * (theta / rho / pi / chi / iota) で書く。バイトの並びは
 * リトルエンディアン (FIPS 202 の流儀)。
 */
#include "sha3.h"

#include "string.h"

static uint64_t rol64(uint64_t x, unsigned n) {
    return (x << n) | (x >> (64 - n));
}

static void keccak_f1600(uint64_t s[25]) {
    static const uint64_t RC[24] = {
        0x0000000000000001ULL, 0x0000000000008082ULL,
        0x800000000000808aULL, 0x8000000080008000ULL,
        0x000000000000808bULL, 0x0000000080000001ULL,
        0x8000000080008081ULL, 0x8000000000008009ULL,
        0x000000000000008aULL, 0x0000000000000088ULL,
        0x0000000080008009ULL, 0x000000008000000aULL,
        0x000000008000808bULL, 0x800000000000008bULL,
        0x8000000000008089ULL, 0x8000000000008003ULL,
        0x8000000000008002ULL, 0x8000000000000080ULL,
        0x000000000000800aULL, 0x800000008000000aULL,
        0x8000000080008081ULL, 0x8000000000008080ULL,
        0x0000000080000001ULL, 0x8000000080008008ULL
    };
    static const int ROTC[24] = {
        1, 3, 6, 10, 15, 21, 28, 36, 45, 55,
        2, 14, 27, 41, 56, 8, 25, 43, 62, 18,
        39, 61, 20, 44
    };
    static const int PILN[24] = {
        10, 7, 11, 17, 18, 3, 5, 16, 8, 21,
        24, 4, 15, 23, 19, 13, 12, 2, 20, 14,
        22, 9, 6, 1
    };
    for (int round = 0; round < 24; round++) {
        uint64_t bc[5];
        for (int i = 0; i < 5; i++) {
            bc[i] = s[i] ^ s[i + 5] ^ s[i + 10] ^ s[i + 15] ^ s[i + 20];
        }
        for (int i = 0; i < 5; i++) {
            uint64_t t = bc[(i + 4) % 5] ^ rol64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) {
                s[j + i] ^= t;
            }
        }
        uint64_t t = s[1];
        for (int i = 0; i < 24; i++) {
            int j = PILN[i];
            bc[0] = s[j];
            s[j] = rol64(t, (unsigned)ROTC[i]);
            t = bc[0];
        }
        /* Chi は行を先にコピーしてから (その場で書くと
         * 既に更新したレーンを使ってしまう)。 */
        for (int j = 0; j < 25; j += 5) {
            uint64_t row[5];
            for (int i = 0; i < 5; i++) {
                row[i] = s[j + i];
            }
            for (int i = 0; i < 5; i++) {
                s[j + i] ^= (~row[(i + 1) % 5]) & row[(i + 2) % 5];
            }
        }
        s[0] ^= RC[round];
    }
}

static void shake_init(shake_ctx *c, size_t rate) {
    memset(c->st, 0, sizeof(c->st));
    c->rate = rate;
    c->pos = 0;
}

void shake128_init(shake_ctx *c) {
    shake_init(c, 168);
}

void shake256_init(shake_ctx *c) {
    shake_init(c, 136);
}

void shake_absorb(shake_ctx *c, const uint8_t *in, size_t len) {
    for (size_t i = 0; i < len; i++) {
        size_t p = c->pos;
        uint8_t *lane =
            (uint8_t *)&c->st[p / 8];
        lane[p % 8] ^= in[i];
        c->pos++;
        if (c->pos == c->rate) {
            keccak_f1600(c->st);
            c->pos = 0;
        }
    }
}

void shake_finalize(shake_ctx *c, uint8_t delim) {
    /* pad10*1: 位置 pos に delim、ブロックの最後のバイトに 0x80。 */
    uint8_t *bytes = (uint8_t *)c->st;
    bytes[c->pos] ^= delim;
    bytes[c->rate - 1] ^= 0x80;
    keccak_f1600(c->st);
    c->pos = 0;
}

void shake_squeeze(shake_ctx *c, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (c->pos == c->rate) {
            keccak_f1600(c->st);
            c->pos = 0;
        }
        size_t p = c->pos;
        out[i] = ((const uint8_t *)c->st)[p];
        c->pos++;
    }
}

void sha3_256(const uint8_t *in, size_t len, uint8_t out[32]) {
    shake_ctx c;
    shake_init(&c, 136);
    shake_absorb(&c, in, len);
    shake_finalize(&c, 0x06);
    shake_squeeze(&c, out, 32);
}

void sha3_512(const uint8_t *in, size_t len, uint8_t out[64]) {
    shake_ctx c;
    shake_init(&c, 72);
    shake_absorb(&c, in, len);
    shake_finalize(&c, 0x06);
    shake_squeeze(&c, out, 64);
}

void shake128_once(const uint8_t *in, size_t len, uint8_t *out,
                   size_t out_len) {
    shake_ctx c;
    shake128_init(&c);
    shake_absorb(&c, in, len);
    shake_finalize(&c, 0x1f);
    shake_squeeze(&c, out, out_len);
}

void shake256_once(const uint8_t *in, size_t len, uint8_t *out,
                   size_t out_len) {
    shake_ctx c;
    shake256_init(&c);
    shake_absorb(&c, in, len);
    shake_finalize(&c, 0x1f);
    shake_squeeze(&c, out, out_len);
}
