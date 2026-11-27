/* PrismOS — ARM PL031 RTC (QEMU virt) の窓口。
 *
 * rtc_read() (共有の rtc.h) の実体は pl031.c にある。
 * ここは TOTP の門など Unix 秒そのものが要る場所のため。 */
#ifndef PRISM_ARM_PL031_H
#define PRISM_ARM_PL031_H

#include <stdint.h>

/* PL031 のカウンタ値 = 現在の Unix 秒 (UTC)。 */
uint32_t pl031_unix_seconds(void);

#endif /* PRISM_ARM_PL031_H */
