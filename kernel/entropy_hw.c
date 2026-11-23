/* PrismOS — ハードウェア乱数とタイマ刻み (x86 専用)
 *
 *   * RDRAND: CPUID.1:ECX bit 30 で有無を確認し、あれば
 *     64 ビット乱数を取る。QEMU の既定 CPU にはある。
 *     無ければ -1 を返し、呼び出し側はイベントプールだけ
 *     を使う (嘘の乱数は返さない)。
 *   * RDTSC: タイムスタンプカウンタの下位 32 ビット。
 *     エントロピーの種としてだけ使う (単独では乱数にしない)。
 *
 * カーネル専用 (ホストテストや ARM には入れない)。
 * SSE/MMX/x87 は使わない。
 */
#include "entropy_hw.h"

#include <stddef.h>
#include <stdint.h>

int entropy_hw_random(uint64_t *out) {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    (void)eax;
    (void)ebx;
    (void)edx;
    /* ECX bit 30 = RDRAND。 */
    if ((ecx & (1u << 30)) == 0) {
        return -1;
    }
    for (int i = 0; i < 10; i++) {
        unsigned char ok = 0;
        uint64_t v = 0;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            if (out != NULL) {
                *out = v;
            }
            return 0;
        }
    }
    return -1;
}

uint32_t entropy_hw_tick(void) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}
