/* PrismOS — ARM 用汎用タイマ (物理タイマ、10ms 間隔)
 *
 * CNTP_TVAL_EL0 に間隔を入れ、CNTP_CTL_EL0 で有効化する。
 * 割り込みが来るたび arm_timer_tick() が刻みを進め、次の
 * 間隔を仕掛ける。刻みの数は arm_timer_ticks() で読める。
 */
#ifndef PRISM_ARM_TIMER_H
#define PRISM_ARM_TIMER_H

#include <stdint.h>

void arm_timer_init(void);
/* IRQ ハンドラから呼ばれる。刻みを 1 進め、次の間隔を仕掛ける。 */
void arm_timer_tick(void);
/* 起動からの刻み数 (10ms 単位)。 */
uint64_t arm_timer_ticks(void);
/* カウンタの周波数 (Hz)。 */
uint64_t arm_timer_freq_hz(void);
/* 今のカウンタ値。割り込みに頼らない時間測りに使う。 */
uint64_t arm_timer_now(void);

#endif /* PRISM_ARM_TIMER_H */
