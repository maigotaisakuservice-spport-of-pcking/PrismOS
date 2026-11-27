/* PrismOS — 32bit 用 64bit 除算ヘルパー
 *
 * -m32 では 64bit の割り算・剰余はランタイム関数呼び出しになる
 * (__udivdi3 など)。32bit 版 libgcc が無い環境でもリンクできる
 * よう、符号なしはビット単位の長除算で、符号つきは符号なしに
 * 帰着させて実装する。本物の計算であり、近似ではない。
 */
#include <stddef.h>
#include <stdint.h>

uint64_t __udivdi3(uint64_t n, uint64_t d) {
    uint64_t q = 0;
    uint64_t r = 0;
    int i;
    if (d == 0) {
        return 0; /* 0 除算は未定義。暴走せず 0 を返す */
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    return q;
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t q = __udivdi3(n, d);
    return n - q * d;
}

int64_t __divdi3(int64_t n, int64_t d) {
    int neg = 0;
    uint64_t un, ud, uq;
    if (n < 0) {
        neg = !neg;
        un = (uint64_t)(-(n + 1)) + 1;
    } else {
        un = (uint64_t)n;
    }
    if (d < 0) {
        neg = !neg;
        ud = (uint64_t)(-(d + 1)) + 1;
    } else {
        ud = (uint64_t)d;
    }
    uq = __udivdi3(un, ud);
    return neg ? -(int64_t)uq : (int64_t)uq;
}

int64_t __moddi3(int64_t n, int64_t d) {
    int neg = 0;
    uint64_t un, ud, ur;
    if (n < 0) {
        neg = 1;
        un = (uint64_t)(-(n + 1)) + 1;
    } else {
        un = (uint64_t)n;
    }
    if (d < 0) {
        ud = (uint64_t)(-(d + 1)) + 1;
    } else {
        ud = (uint64_t)d;
    }
    ur = __umoddi3(un, ud);
    return neg ? -(int64_t)ur : (int64_t)ur;
}

/* 商と剰余を一緒に返す版 (rtc.c の 64bit 除算が使う)。 */
int64_t __divmoddi4(int64_t n, int64_t d, int64_t *rem) {
    int64_t q = __divdi3(n, d);
    if (rem != NULL) {
        *rem = __moddi3(n, d);
    }
    return q;
}

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = __udivdi3(n, d);
    if (rem != NULL) {
        *rem = __umoddi3(n, d);
    }
    return q;
}
