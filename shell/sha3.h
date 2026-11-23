/* PrismOS — SHA-3 / SHAKE (Keccak-f1600)
 *
 * ML-KEM (FIPS 203) と ML-DSA (FIPS 204) の両方が使う土台。
 * 純粋関数で、浮動小数点も動的確保も使わない。カーネルと
 * ホストテストで同じソースを使う。
 *
 *   * SHA3-256 / SHA3-512: 一発関数。
 *   * SHAKE128 / SHAKE256: 吸収と絞り出しを分けた使い方
 *     (shake128_init -> absorb -> finalize -> squeeze) と
 *     一発関数 (shake128_once 等) の両方を用意する。
 *
 * 注意: カーネルはシングルスレッドなので、呼び出し側が
 * コンテキストをスタックに置いて使う。大きな作業域は
 * ここには無い (状態は 25 ワードだけ)。
 */
#ifndef PRISM_SHA3_H
#define PRISM_SHA3_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t st[25];
    size_t rate; /* 吸収・絞り出しのブロック長 (バイト) */
    size_t pos;  /* 現在のブロック内の位置 (バイト) */
} shake_ctx;

/* SHAKE128 / SHAKE256 の開始。 */
void shake128_init(shake_ctx *c);
void shake256_init(shake_ctx *c);

/* 入力を吸収する。finalize の前なら何度でも呼べる。 */
void shake_absorb(shake_ctx *c, const uint8_t *in, size_t len);

/* 吸収を終える。delim は SHAKE なら 0x1f、SHA-3 なら 0x06。 */
void shake_finalize(shake_ctx *c, uint8_t delim);

/* 出力を絞り出す。finalize の後なら何度でも呼べる。 */
void shake_squeeze(shake_ctx *c, uint8_t *out, size_t len);

/* SHA3-256 / SHA3-512 の一発関数。out は 32 / 64 バイト。 */
void sha3_256(const uint8_t *in, size_t len, uint8_t out[32]);
void sha3_512(const uint8_t *in, size_t len, uint8_t out[64]);

/* SHAKE128 / SHAKE256 の一発関数。out_len バイト絞り出す。 */
void shake128_once(const uint8_t *in, size_t len, uint8_t *out,
                   size_t out_len);
void shake256_once(const uint8_t *in, size_t len, uint8_t *out,
                   size_t out_len);

#endif /* PRISM_SHA3_H */
