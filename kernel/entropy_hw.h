/* PrismOS — ハードウェア乱数とタイマ刻み (x86 専用、カーネルのみ) */
#ifndef PRISM_ENTROPY_HW_H
#define PRISM_ENTROPY_HW_H

#include <stdint.h>

/* RDRAND で 64 ビット乱数を 1 つ取る。成功したら 0、
 * CPU に RDRAND が無い・10 回とも失敗したら -1。 */
int entropy_hw_random(uint64_t *out);

/* タイムスタンプカウンタの下位 32 ビット。 */
uint32_t entropy_hw_tick(void);

#endif /* PRISM_ENTROPY_HW_H */
