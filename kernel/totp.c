/* PrismOS — TOTP / HOTP の実装 (totp.h 参照)。 */
#include "totp.h"

#include "string.h"

/* ---- SHA-1 ---- */

static uint32_t rol32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(prism_sha1 *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) |
               (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3],
             e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol32(b, 30);
        b = a;
        a = tmp;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

void sha1_init(prism_sha1 *c) {
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->len = 0;
    c->buflen = 0;
}

void sha1_update(prism_sha1 *c, const uint8_t *data, size_t len) {
    c->len += len;
    while (len > 0) {
        size_t take = 64 - c->buflen;
        if (take > len) {
            take = len;
        }
        memcpy(c->buf + c->buflen, data, take);
        c->buflen += take;
        data += take;
        len -= take;
        if (c->buflen == 64) {
            sha1_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

void sha1_final(prism_sha1 *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) {
        sha1_update(c, &zero, 1);
    }
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) {
        lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
    }
    /* buflen 経由でなく直接ブロック処理する (len を
     * 増やさないため)。buflen は 56 で、8 バイト足すと
     * ちょうど 64 になる。 */
    memcpy(c->buf + 56, lenbuf, 8);
    sha1_block(c, c->buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

/* ---- HMAC-SHA1 ---- */

static void hmac_sha1(const uint8_t *key, size_t key_len,
                      const uint8_t *msg, size_t msg_len,
                      uint8_t out[20]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        prism_sha1 c;
        sha1_init(&c);
        sha1_update(&c, key, key_len);
        sha1_final(&c, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5C);
    }
    prism_sha1 c;
    uint8_t inner[20];
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, msg, msg_len);
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
    memset(k, 0, sizeof(k));
    memset(inner, 0, sizeof(inner));
}

uint32_t hotp_code(const uint8_t *secret, size_t secret_len,
                   uint64_t counter, int digits) {
    uint8_t msg[8];
    for (int i = 0; i < 8; i++) {
        msg[i] = (uint8_t)(counter >> (56 - i * 8));
    }
    uint8_t mac[20];
    hmac_sha1(secret, secret_len, msg, 8, mac);
    int off = mac[19] & 0x0F;
    uint32_t bin = ((uint32_t)(mac[off] & 0x7F) << 24) |
                   ((uint32_t)mac[off + 1] << 16) |
                   ((uint32_t)mac[off + 2] << 8) |
                   (uint32_t)mac[off + 3];
    uint32_t mod = 1;
    for (int i = 0; i < digits; i++) {
        mod *= 10;
    }
    memset(mac, 0, sizeof(mac));
    return bin % mod;
}

uint32_t totp_code(const uint8_t *secret, size_t secret_len,
                   uint64_t unix_time, uint32_t period, int digits) {
    if (period == 0) {
        period = 30;
    }
    return hotp_code(secret, secret_len, unix_time / period, digits);
}

int totp_verify(const uint8_t *secret, size_t secret_len,
                uint64_t unix_time, uint32_t code, int window) {
    if (secret == NULL || secret_len == 0) {
        return 0;
    }
    uint64_t step = unix_time / 30;
    for (int w = -window; w <= window; w++) {
        uint64_t ctr = (uint64_t)((int64_t)step + w);
        if (hotp_code(secret, secret_len, ctr, 6) == code) {
            return 1;
        }
    }
    return 0;
}

/* ---- Base32 ---- */

static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

int base32_encode(const uint8_t *in, size_t len, char *out,
                  size_t cap) {
    size_t o = 0;
    uint32_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | in[i];
        nbits += 8;
        while (nbits >= 5) {
            if (o + 1 >= cap) {
                return -1;
            }
            out[o++] = B32[(acc >> (nbits - 5)) & 31];
            nbits -= 5;
        }
    }
    if (nbits > 0) {
        if (o + 1 >= cap) {
            return -1;
        }
        out[o++] = B32[(acc << (5 - nbits)) & 31];
    }
    if (o >= cap) {
        return -1;
    }
    out[o] = '\0';
    return (int)o;
}

static int b32_val(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a';
    }
    if (ch >= '2' && ch <= '7') {
        return ch - '2' + 26;
    }
    return -1;
}

int base32_decode(const char *in, uint8_t *out, size_t cap) {
    size_t o = 0;
    uint32_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        if (in[i] == '=' || in[i] == ' ' || in[i] == '-') {
            continue;
        }
        int v = b32_val(in[i]);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 5) | (uint32_t)v;
        nbits += 5;
        if (nbits >= 8) {
            if (o >= cap) {
                return -1;
            }
            out[o++] = (uint8_t)(acc >> (nbits - 8));
            nbits -= 8;
        }
    }
    return (int)o;
}
