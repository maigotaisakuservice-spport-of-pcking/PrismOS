/* PrismOS — ARM 用のハードウェア乱数スタブ。
 * ARM には RDRAND 相当の命令をこの実装では使わないので、
 * entropy_hw_random は -1 を返す (呼び出し側はイベント
 * プールとタイマ刻みにフォールバックする。嘘の乱数は
 * 返さない)。ティックは汎用タイマのカウンタを使う。 */
#include "../entropy_hw.h"

#include "timer.h"

int entropy_hw_random(uint64_t *out) {
    (void)out;
    return -1;
}

uint32_t entropy_hw_tick(void) {
    return (uint32_t)arm_timer_ticks();
}
