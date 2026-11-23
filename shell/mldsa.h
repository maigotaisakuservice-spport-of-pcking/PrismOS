/* PrismOS — ML-DSA-44 (FIPS 204) の純粋 C 実装
 *
 * 格子ベースの署名。更新ファイルの署名検証に使う。
 * 浮動小数点も動的確保も使わない。カーネルとホストテストで
 * 同じソースを使う。
 *
 * 使い方:
 *   mldsa44_keygen_from_seed(seed32, pk, sk)
 *     seed は 32 バイトの乱数。pk 1312 バイト、sk 2560 バイト。
 *   mldsa44_sign(sk, msg, msg_len, sig)
 *     決定的な署名 (rnd = 0)。sig 2420 バイト。
 *   mldsa44_verify(pk, msg, msg_len, sig)
 *     0 = 検証 OK、0 以外 = 拒否。
 *
 * 作業域: 32KB ほどの静的な作業域を使う (カーネルの
 * スタックは 32KB しかないため)。シングルスレッド前提で、
 * 再入はしないこと。
 */
#ifndef PRISM_MLDSA_H
#define PRISM_MLDSA_H

#include <stddef.h>
#include <stdint.h>

#define MLDSA44_PK_LEN 1312
#define MLDSA44_SK_LEN 2560
#define MLDSA44_SIG_LEN 2420
#define MLDSA44_SEED_LEN 32

int mldsa44_keygen_from_seed(const uint8_t seed[MLDSA44_SEED_LEN],
                             uint8_t pk[MLDSA44_PK_LEN],
                             uint8_t sk[MLDSA44_SK_LEN]);
int mldsa44_sign(const uint8_t sk[MLDSA44_SK_LEN],
                 const uint8_t *msg, size_t msg_len,
                 uint8_t sig[MLDSA44_SIG_LEN]);
int mldsa44_verify(const uint8_t pk[MLDSA44_PK_LEN],
                   const uint8_t *msg, size_t msg_len,
                   const uint8_t sig[MLDSA44_SIG_LEN]);

#endif /* PRISM_MLDSA_H */
