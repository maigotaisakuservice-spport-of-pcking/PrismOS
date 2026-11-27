/* PrismOS — ML-KEM (FIPS 203) の純粋 C 実装
 *
 * 格子ベースの鍵交換 (Kyber)。ML-KEM-512 / 768 / 1024 の
 * 3 つを、パラメータ k だけが違う同じコードで扱う。
 * 浮動小数点も動的確保も使わない。カーネルとホストテストで
 * 同じソースを使う。
 *
 * 使い方 (呼び出し側が乱数を渡す。決定的でテストしやすい):
 *   mlkem768_keygen(d, z, ek, dk)   d, z は 32 バイトの乱数
 *   mlkem768_encaps(ek, m, ct, ss)  m は 32 バイトの乱数
 *   mlkem768_decaps(dk, ct, ss)
 * 共有鍵 ss は 32 バイト。encaps と decaps で同じ ss になる。
 * 壊れた暗号文には、暗黙の拒否で別の共有鍵を返す
 * (FIPS 203 の流儀。失敗を教えない)。
 *
 * 作業域: 18KB ほどの静的な作業域を使う (カーネルの
 * スタックは 32KB しかないため)。シングルスレッド前提で、
 * 再入はしないこと。
 */
#ifndef PRISM_MLKEM_H
#define PRISM_MLKEM_H

#include <stddef.h>
#include <stdint.h>

#define MLKEM512_EK_LEN 800
#define MLKEM512_DK_LEN 1632
#define MLKEM512_CT_LEN 768
#define MLKEM768_EK_LEN 1184
#define MLKEM768_DK_LEN 2400
#define MLKEM768_CT_LEN 1088
#define MLKEM1024_EK_LEN 1568
#define MLKEM1024_DK_LEN 3168
#define MLKEM1024_CT_LEN 1568
#define MLKEM_SS_LEN 32

int mlkem512_keygen(const uint8_t d[32], const uint8_t z[32],
                    uint8_t ek[MLKEM512_EK_LEN],
                    uint8_t dk[MLKEM512_DK_LEN]);
int mlkem512_encaps(const uint8_t ek[MLKEM512_EK_LEN],
                    const uint8_t m[32],
                    uint8_t ct[MLKEM512_CT_LEN],
                    uint8_t ss[MLKEM_SS_LEN]);
int mlkem512_decaps(const uint8_t dk[MLKEM512_DK_LEN],
                    const uint8_t ct[MLKEM512_CT_LEN],
                    uint8_t ss[MLKEM_SS_LEN]);

int mlkem768_keygen(const uint8_t d[32], const uint8_t z[32],
                    uint8_t ek[MLKEM768_EK_LEN],
                    uint8_t dk[MLKEM768_DK_LEN]);
int mlkem768_encaps(const uint8_t ek[MLKEM768_EK_LEN],
                    const uint8_t m[32],
                    uint8_t ct[MLKEM768_CT_LEN],
                    uint8_t ss[MLKEM_SS_LEN]);
int mlkem768_decaps(const uint8_t dk[MLKEM768_DK_LEN],
                    const uint8_t ct[MLKEM768_CT_LEN],
                    uint8_t ss[MLKEM_SS_LEN]);

int mlkem1024_keygen(const uint8_t d[32], const uint8_t z[32],
                     uint8_t ek[MLKEM1024_EK_LEN],
                     uint8_t dk[MLKEM1024_DK_LEN]);
int mlkem1024_encaps(const uint8_t ek[MLKEM1024_EK_LEN],
                     const uint8_t m[32],
                     uint8_t ct[MLKEM1024_CT_LEN],
                     uint8_t ss[MLKEM_SS_LEN]);
int mlkem1024_decaps(const uint8_t dk[MLKEM1024_DK_LEN],
                     const uint8_t ct[MLKEM1024_CT_LEN],
                     uint8_t ss[MLKEM_SS_LEN]);

#endif /* PRISM_MLKEM_H */
