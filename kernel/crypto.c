/* PrismOS — SHA-256 と AES-256-CTR (純粋関数) */
#include "crypto.h"

/* ================= SHA-256 ================= */

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static const uint32_t SHA_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/* 1 ブロック (64 バイト) を圧縮して h に混ぜる。 */
static void sha256_compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + s1 + ch + SHA_K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void crypto_sha256_init(crypto_sha256_ctx *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->block_len = 0;
    ctx->total_len = 0;
}

void crypto_sha256_update(crypto_sha256_ctx *ctx, const uint8_t *data,
                          size_t len) {
    if (ctx == NULL || (data == NULL && len != 0)) {
        return;
    }
    ctx->total_len += (uint64_t)len;
    while (len > 0) {
        size_t room = 64 - ctx->block_len;
        size_t n = len < room ? len : room;
        for (size_t i = 0; i < n; i++) {
            ctx->block[ctx->block_len + i] = data[i];
        }
        ctx->block_len += n;
        data += n;
        len -= n;
        if (ctx->block_len == 64) {
            sha256_compress(ctx->h, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void crypto_sha256_final(crypto_sha256_ctx *ctx, uint8_t out[32]) {
    if (ctx == NULL || out == NULL) {
        return;
    }
    uint64_t bit_len = ctx->total_len * 8;
    /* パディング: 0x80、0 を詰めて長さ (ビット数、64 ビット) を
     * 最後の 8 バイトに入れる。全体が 64 の倍数になる。 */
    uint8_t pad = 0x80;
    crypto_sha256_update(ctx, &pad, 1);
    uint8_t zero = 0;
    while (ctx->block_len != 56) {
        crypto_sha256_update(ctx, &zero, 1);
    }
    /* 長さの 8 バイトは total_len を使わず、ここで直接入れる
     * (update に渡すと total_len が増えてしまうため)。
     * block_len は 56 なので、8 バイト足すとちょうど 64 になる。 */
    for (int i = 0; i < 8; i++) {
        ctx->block[56 + i] =
            (uint8_t)((bit_len >> ((7 - i) * 8)) & 0xFF);
    }
    sha256_compress(ctx->h, ctx->block);
    ctx->block_len = 0;
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)((ctx->h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = (uint8_t)((ctx->h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((ctx->h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i] & 0xFF);
    }
}

void crypto_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    crypto_sha256_ctx ctx;
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, data, len);
    crypto_sha256_final(&ctx, out);
}

void crypto_derive_key(const char *pin, const uint8_t *salt,
                       uint8_t key_out[32]) {
    /* SHA-256(PIN || salt)。PIN の長さは 4〜16 文字。 */
    size_t pin_len = 0;
    while (pin[pin_len] != '\0') {
        pin_len++;
    }
    uint8_t buf[16 + 8];
    for (size_t i = 0; i < pin_len; i++) {
        buf[i] = (uint8_t)pin[i];
    }
    for (int i = 0; i < 8; i++) {
        buf[pin_len + (size_t)i] = salt[i];
    }
    crypto_sha256(buf, pin_len + 8, key_out);
}

void crypto_kdf(const char *pin, const uint8_t *salt, size_t salt_len,
                uint32_t iterations, uint8_t key_out[32]) {
    if (pin == NULL || salt == NULL || key_out == NULL) {
        return;
    }
    if (iterations == 0) {
        iterations = 1;
    }
    size_t pin_len = 0;
    while (pin[pin_len] != '\0') {
        pin_len++;
    }
    /* 1 回目: SHA-256(salt || PIN)。
     * 2 回目以降: SHA-256(前回 || salt || PIN)。
     * 以前は 64 バイトの固定バッファに組み立てていたため、
     * ソルト 16 バイトで PIN が 17 文字以上だとバッファを
     * 溢れてスタックを壊していた (アカウント PIN は最大
     * 16 文字で発現しなかったが、FDE の 24 文字リカバリ
     * キーで発現した)。ストリーミングで同じバイト列を
     * ハッシュするので、既存の入力に対する結果は不変。 */
    crypto_sha256_ctx cx;
    crypto_sha256_init(&cx);
    crypto_sha256_update(&cx, salt, salt_len);
    crypto_sha256_update(&cx, (const uint8_t *)pin, pin_len);
    crypto_sha256_final(&cx, key_out);
    for (uint32_t round = 1; round < iterations; round++) {
        uint8_t prev[32];
        for (int i = 0; i < 32; i++) {
            prev[i] = key_out[i];
        }
        crypto_sha256_init(&cx);
        crypto_sha256_update(&cx, prev, sizeof(prev));
        crypto_sha256_update(&cx, salt, salt_len);
        crypto_sha256_update(&cx, (const uint8_t *)pin, pin_len);
        crypto_sha256_final(&cx, key_out);
        for (int i = 0; i < 32; i++) {
            prev[i] = 0;
        }
    }
}

/* ================= AES-256 ================= */

static const uint8_t AES_SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t AES_RCON[8] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
};

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00));
}

/* 32 バイトの鍵から 60 ワード (240 バイト) のラウンド鍵を作る。 */
static void aes256_expand_key(const uint8_t *key, uint8_t *rk) {
    for (int i = 0; i < 32; i++) {
        rk[i] = key[i];
    }
    int bytes = 32;
    int rcon_i = 0;
    while (bytes < 240) {
        uint8_t temp[4];
        for (int i = 0; i < 4; i++) {
            temp[i] = rk[bytes - 4 + i];
        }
        if (bytes % 32 == 0) {
            uint8_t t = temp[0];
            temp[0] = AES_SBOX[temp[1]] ^ AES_RCON[rcon_i];
            temp[1] = AES_SBOX[temp[2]];
            temp[2] = AES_SBOX[temp[3]];
            temp[3] = AES_SBOX[t];
            rcon_i++;
        } else if (bytes % 32 == 16) {
            for (int i = 0; i < 4; i++) {
                temp[i] = AES_SBOX[temp[i]];
            }
        }
        for (int i = 0; i < 4; i++) {
            rk[bytes + i] = rk[bytes - 32 + i] ^ temp[i];
        }
        bytes += 4;
    }
}

static void aes_add_round_key(uint8_t *state, const uint8_t *rk) {
    for (int i = 0; i < 16; i++) {
        state[i] ^= rk[i];
    }
}

static void aes_sub_bytes(uint8_t *state) {
    for (int i = 0; i < 16; i++) {
        state[i] = AES_SBOX[state[i]];
    }
}

static void aes_shift_rows(uint8_t *state) {
    uint8_t t;
    /* state は列優先: state[4*c + r] が r 行 c 列。 */
    t = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = t;
    t = state[2];
    state[2] = state[10];
    state[10] = t;
    t = state[6];
    state[6] = state[14];
    state[14] = t;
    t = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = state[3];
    state[3] = t;
}

static void aes_mix_columns(uint8_t *state) {
    for (int c = 0; c < 4; c++) {
        uint8_t *col = state + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        col[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        col[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        col[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
}

static void aes256_encrypt_block(const uint8_t *rk, const uint8_t *in,
                                 uint8_t *out) {
    uint8_t state[16];
    for (int i = 0; i < 16; i++) {
        state[i] = in[i];
    }
    aes_add_round_key(state, rk);
    for (int round = 1; round < 14; round++) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, rk + round * 16);
    }
    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_add_round_key(state, rk + 14 * 16);
    for (int i = 0; i < 16; i++) {
        out[i] = state[i];
    }
}

void crypto_aes256_ctr(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *in, uint8_t *out, size_t len) {
    if (len == 0 || key == NULL || iv == NULL || in == NULL ||
        out == NULL) {
        return;
    }
    uint8_t rk[240];
    aes256_expand_key(key, rk);
    uint8_t counter[16];
    for (int i = 0; i < 16; i++) {
        counter[i] = iv[i];
    }
    size_t pos = 0;
    while (pos < len) {
        uint8_t keystream[16];
        aes256_encrypt_block(rk, counter, keystream);
        size_t take = len - pos;
        if (take > 16) {
            take = 16;
        }
        for (size_t i = 0; i < take; i++) {
            out[pos + i] = (uint8_t)(in[pos + i] ^ keystream[i]);
        }
        pos += take;
        /* カウンタを 128 ビットのビッグエンディアンで +1。 */
        for (int i = 15; i >= 0; i--) {
            counter[i]++;
            if (counter[i] != 0) {
                break;
            }
        }
    }
}
