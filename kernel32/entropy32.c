/* PrismOS (32-bit) — ハードウェア乱数の 32 ビット版。
 * 64 ビット版 (kernel/entropy_hw.c) の rdrand は 64 ビット
 * レジスタ前提なので、32 ビットでは 32 ビット rdrand を
 * 2 回読んで 64 ビットに組む。RDTSC は両モード共通。 */
#include "../kernel/entropy_hw.h"

#include <stddef.h>

int entropy_hw_random(uint64_t *out) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    (void)eax;
    (void)ebx;
    (void)edx;
    if ((ecx & (1u << 30)) == 0) {
        return -1;
    }
    uint32_t parts[2];
    for (int half = 0; half < 2; half++) {
        int got = 0;
        for (int i = 0; i < 10 && !got; i++) {
            unsigned char ok = 0;
            uint32_t v = 0;
            __asm__ volatile("rdrand %0; setc %1"
                             : "=r"(v), "=qm"(ok));
            if (ok) {
                parts[half] = v;
                got = 1;
            }
        }
        if (!got) {
            return -1;
        }
    }
    if (out != NULL) {
        *out = ((uint64_t)parts[1] << 32) | parts[0];
    }
    return 0;
}

uint32_t entropy_hw_tick(void) {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}
