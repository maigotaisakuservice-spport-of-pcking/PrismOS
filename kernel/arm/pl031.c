/* PrismOS — ARM PL031 RTC ドライバ (QEMU virt)
 *
 * virt 構成の PL031 (ベース 0x09010000) は Unix 秒の
 * カウンタを持ち、QEMU がホストの時刻で初期化する。
 * 以前のスタブは汎用タイマの経過秒から作った絶対時刻で
 * ない値を返していたため、時刻ベース認証 (TOTP) の前提を
 * 満たさなかった。このドライバは実時刻を読む。
 *
 * レジスタ: DR (0x00) = 現在値 (秒)、CR (0x0C) の bit0 =
 * カウント開始。読み出しだけなら初期化は要らないが、
 * 実機相当の作法として初回に開始ビットを立てておく。
 * rtc_read() は Unix 秒を UTC の年月日時分秒へ分解して
 * 返す (曜日は 1970-01-01 が木曜日であることから出す)。 */
#include "rtc.h"

#include <stddef.h>

#include "pl031.h"

#define PL031_BASE ((volatile uint32_t *)0x09010000u)
#define PL031_DR 0 /* 0x00: 現在値 (Unix 秒) */
#define PL031_CR 3 /* 0x0C: 制御 (bit0 = 開始) */

static int pl031_started;

uint32_t pl031_unix_seconds(void) {
    if (!pl031_started) {
        PL031_BASE[PL031_CR] = 1;
        pl031_started = 1;
    }
    return PL031_BASE[PL031_DR];
}

int rtc_read(prism_rtc_time *out) {
    if (out == NULL) {
        return -1;
    }
    uint32_t secs = pl031_unix_seconds();
    uint32_t days = secs / 86400u;
    uint32_t tod = secs % 86400u;
    out->hour = (uint8_t)(tod / 3600u);
    out->minute = (uint8_t)((tod / 60u) % 60u);
    out->second = (uint8_t)(tod % 60u);
    out->weekday = (uint8_t)((days + 4u) % 7u); /* 1970-01-01 = 木 */
    /* 日数 → 年月日 (civil-from-days、Howard Hinnant の式)。 */
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u -
                    doe / 146096u) /
                   365u;
    int64_t year = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t day = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t month = mp < 10u ? mp + 3u : mp - 9u;
    if (month <= 2u) {
        year += 1;
    }
    out->day = (uint8_t)day;
    out->month = (uint8_t)month;
    out->year = (uint8_t)(year - 2000); /* 2000+year の約束 */
    return 0;
}
