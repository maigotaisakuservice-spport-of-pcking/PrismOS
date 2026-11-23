/* PrismOS — ARM 用汎用タイマ (物理タイマ、10ms 間隔)
 *
 * CNTP_TVAL_EL0 に間隔を入れ、CNTP_CTL_EL0 で有効化する。
 * 割り込みが来るたび arm_timer_tick() が刻みを進め、次の
 * 間隔を仕掛ける。刻みの数は arm_timer_ticks() で読める。
 */
#include "timer.h"

#define TIMER_INTERVAL_MS 10

static volatile uint64_t ticks;

static uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void write_cntp_tval(uint64_t v) {
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(v));
    __asm__ volatile("isb" ::: "memory");
}

static void write_cntp_ctl(uint64_t v) {
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(v));
    __asm__ volatile("isb" ::: "memory");
}

uint64_t arm_timer_freq_hz(void) {
    return read_cntfrq();
}

uint64_t arm_timer_now(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

uint64_t arm_timer_ticks(void) {
    return ticks;
}

void arm_timer_tick(void) {
    ticks++;
    /* 次の間隔を仕掛ける。ここで TVAL を書き直すと割り込みの
     * 条件が消え、10ms 後にまた割り込みが来る。 */
    write_cntp_tval(read_cntfrq() / (1000 / TIMER_INTERVAL_MS));
}

void arm_timer_init(void) {
    ticks = 0;
    /* 最初の間隔を仕掛けてから有効化する。CTL の bit 0 が
     * 有効化、bit 1 がマスク (0 = 割り込みを出す)。 */
    write_cntp_tval(read_cntfrq() / (1000 / TIMER_INTERVAL_MS));
    write_cntp_ctl(1);
}
