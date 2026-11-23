/* PrismOS - CMOS real-time clock. */
#include "rtc.h"
#include "io.h"

#include <stddef.h>

#define RTC_INDEX 0x70
#define RTC_DATA 0x71

uint8_t rtc_bcd_to_bin(uint8_t v) {
    uint8_t hi = (uint8_t)((v >> 4) & 0x0F);
    uint8_t lo = (uint8_t)(v & 0x0F);
    if (hi > 9) {
        hi = 9;
    }
    if (lo > 9) {
        lo = 9;
    }
    return (uint8_t)(hi * 10 + lo);
}

uint8_t rtc_bin_to_bcd(uint8_t v) {
    if (v > 99) {
        v = 99;
    }
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t rtc_read_reg(uint8_t reg) {
    /* Bit 7 of the index disables the non-maskable interrupt
     * while the clock is read. */
    prism_outb(RTC_INDEX, (uint8_t)(reg | 0x80));
    return prism_inb(RTC_DATA);
}

static int rtc_updating(void) {
    return (rtc_read_reg(0x0A) & 0x80) != 0;
}

int rtc_read(prism_rtc_time *out) {
    if (out == NULL) {
        return -1;
    }
    /* Wait out an update in progress so the three fields come
     * from the same second. Give up after a bounded spin. */
    for (int i = 0; i < 100000 && rtc_updating(); i++) {
        /* spin */
    }
    uint8_t reg_b = rtc_read_reg(0x0B);
    int binary_mode = (reg_b & 0x04) != 0;
    int hour_24 = (reg_b & 0x02) != 0;
    uint8_t sec = rtc_read_reg(0x00);
    uint8_t min = rtc_read_reg(0x02);
    uint8_t hour = rtc_read_reg(0x04);
    uint8_t weekday = rtc_read_reg(0x06);
    uint8_t day = rtc_read_reg(0x07);
    uint8_t month = rtc_read_reg(0x08);
    uint8_t year = rtc_read_reg(0x09);
    int pm = 0;
    if (!hour_24) {
        pm = (hour & 0x80) != 0;
        hour = (uint8_t)(hour & 0x7F);
    }
    if (!binary_mode) {
        sec = rtc_bcd_to_bin(sec);
        min = rtc_bcd_to_bin(min);
        hour = rtc_bcd_to_bin(hour);
        weekday = rtc_bcd_to_bin(weekday);
        day = rtc_bcd_to_bin(day);
        month = rtc_bcd_to_bin(month);
        year = rtc_bcd_to_bin(year);
    }
    if (!hour_24 && pm && hour < 12) {
        hour = (uint8_t)(hour + 12);
    }
    if (!hour_24 && !pm && hour == 12) {
        hour = 0;
    }
    if (sec > 59) {
        sec = 59;
    }
    if (min > 59) {
        min = 59;
    }
    if (hour > 23) {
        hour = 23;
    }
    if (day < 1) {
        day = 1;
    }
    if (day > 31) {
        day = 31;
    }
    if (month < 1) {
        month = 1;
    }
    if (month > 12) {
        month = 12;
    }
    if (year > 99) {
        year = 99;
    }
    /* Clock weekday is 1=Sunday .. 7=Saturday; convert to
     * 0=Sunday .. 6=Saturday, clamping bad values. */
    if (weekday < 1 || weekday > 7) {
        weekday = 1;
    }
    out->hour = hour;
    out->minute = min;
    out->second = sec;
    out->day = day;
    out->month = month;
    out->year = year;
    out->weekday = (uint8_t)(weekday - 1);
    return 0;
}

void rtc_format_hhmm(const prism_rtc_time *t, char *buf) {
    if (t == NULL || buf == NULL) {
        return;
    }
    buf[0] = (char)('0' + (t->hour / 10));
    buf[1] = (char)('0' + (t->hour % 10));
    buf[2] = ':';
    buf[3] = (char)('0' + (t->minute / 10));
    buf[4] = (char)('0' + (t->minute % 10));
    buf[5] = '\0';
}

void rtc_format_date(const prism_rtc_time *t, char *buf) {
    if (t == NULL || buf == NULL) {
        return;
    }
    int year = 2000 + t->year;
    buf[0] = (char)('0' + (year / 1000) % 10);
    buf[1] = (char)('0' + (year / 100) % 10);
    buf[2] = (char)('0' + (year / 10) % 10);
    buf[3] = (char)('0' + year % 10);
    buf[4] = '-';
    buf[5] = (char)('0' + (t->month / 10));
    buf[6] = (char)('0' + (t->month % 10));
    buf[7] = '-';
    buf[8] = (char)('0' + (t->day / 10));
    buf[9] = (char)('0' + (t->day % 10));
    buf[10] = '\0';
}

int rtc_days_in_month(int year, int month) {
    static const int days[12] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 30;
    }
    if (month == 2) {
        int leap = (year % 4 == 0 && year % 100 != 0) ||
                   (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

int rtc_weekday(int year, int month, int day) {
    /* Zeller's congruence, returning 0=Sunday. */
    int y = year;
    int m = month;
    if (m < 3) {
        m += 12;
        y -= 1;
    }
    int k = y % 100;
    int j = y / 100;
    int h = (day + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    /* Zeller gives 0=Saturday, so shift to 0=Sunday. */
    return (h + 6) % 7;
}

uint32_t rtc_epoch_seconds(const prism_rtc_time *t) {
    if (t == NULL) {
        return 0;
    }
    int year = 2000 + (int)t->year;
    int month = (int)t->month;
    int day = (int)t->day;
    if (month < 1) {
        month = 1;
    }
    if (month > 12) {
        month = 12;
    }
    if (day < 1) {
        day = 1;
    }
    if (day > 31) {
        day = 31;
    }
    uint32_t days = 0;
    for (int y = 2000; y < year; y++) {
        int leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        days += leap ? 366u : 365u;
    }
    for (int m = 1; m < month; m++) {
        days += (uint32_t)rtc_days_in_month(year, m);
    }
    days += (uint32_t)(day - 1);
    return days * 86400u + (uint32_t)t->hour * 3600u +
           (uint32_t)t->minute * 60u + (uint32_t)t->second;
}

/* ---------- write + conversions ---------- */

static void rtc_write_reg(uint8_t reg, uint8_t val) {
    prism_outb(RTC_INDEX, (uint8_t)(reg | 0x80));
    prism_outb(RTC_DATA, val);
}

int rtc_write(const prism_rtc_time *t) {
    if (t == NULL) {
        return -1;
    }
    uint8_t reg_b = rtc_read_reg(0x0B);
    int binary_mode = (reg_b & 0x04) != 0;
    int hour_24 = (reg_b & 0x02) != 0;
    uint8_t sec = t->second;
    uint8_t min = t->minute;
    uint8_t hour = t->hour;
    uint8_t weekday = (uint8_t)(rtc_weekday(2000 + t->year,
                                            t->month, t->day) +
                                1); /* CMOS: 1=Sunday */
    uint8_t day = t->day;
    uint8_t month = t->month;
    uint8_t year = t->year;
    uint8_t hour_reg = hour;
    if (!hour_24) {
        int pm = hour >= 12;
        int h12 = (int)hour % 12;
        if (h12 == 0) {
            h12 = 12;
        }
        hour_reg = (uint8_t)h12;
        if (!binary_mode) {
            hour_reg = rtc_bin_to_bcd(hour_reg);
        }
        if (pm) {
            hour_reg = (uint8_t)(hour_reg | 0x80);
        }
    } else if (!binary_mode) {
        hour_reg = rtc_bin_to_bcd(hour);
    }
    if (!binary_mode) {
        sec = rtc_bin_to_bcd(sec);
        min = rtc_bin_to_bcd(min);
        weekday = rtc_bin_to_bcd(weekday);
        day = rtc_bin_to_bcd(day);
        month = rtc_bin_to_bcd(month);
        year = rtc_bin_to_bcd(year);
    }
    /* Halt updates while the fields change. */
    rtc_write_reg(0x0B, (uint8_t)(reg_b | 0x80));
    rtc_write_reg(0x00, sec);
    rtc_write_reg(0x02, min);
    rtc_write_reg(0x04, hour_reg);
    rtc_write_reg(0x06, weekday);
    rtc_write_reg(0x07, day);
    rtc_write_reg(0x08, month);
    rtc_write_reg(0x09, year);
    rtc_write_reg(0x32, rtc_bin_to_bcd(20)); /* century */
    rtc_write_reg(0x0B, reg_b);
    return 0;
}

/* Days since 1970-01-01 from a civil date (Howard
 * Hinnant's algorithm, integer only). */
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                 /* [0, 399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint32_t rtc_to_unix(const prism_rtc_time *t) {
    int64_t days = days_from_civil(2000 + t->year, t->month,
                                   t->day);
    int64_t sec = days * 86400 + t->hour * 3600 + t->minute * 60 +
                  t->second;
    return (uint32_t)sec;
}

void rtc_from_unix(uint32_t unix_sec, int tz_minutes,
                   prism_rtc_time *out) {
    int64_t sec = (int64_t)unix_sec + (int64_t)tz_minutes * 60;
    int64_t days = sec / 86400;
    int64_t rem = sec % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    out->hour = (uint8_t)(rem / 3600);
    out->minute = (uint8_t)((rem % 3600) / 60);
    out->second = (uint8_t)(rem % 60);
    /* civil from days */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;              /* [0, 146096] */
    int64_t yoe = (doe - doe / 1460 + doe / 36524 -
                   doe / 146096) / 365;          /* [0, 399] */
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;            /* [0, 11] */
    int64_t d = doy - (153 * mp + 2) / 5 + 1;    /* [1, 31] */
    int64_t m = mp + (mp < 10 ? 3 : -9);         /* [1, 12] */
    y += m <= 2;
    out->day = (uint8_t)d;
    out->month = (uint8_t)m;
    out->year = (uint8_t)(y - 2000);
    out->weekday = (uint8_t)rtc_weekday((int)y, (int)m, (int)d);
}

int rtc_from_ntp(uint32_t ntp_seconds, int tz_minutes,
                 prism_rtc_time *out) {
    if (ntp_seconds < 2208988800u) {
        return -1;
    }
    rtc_from_unix(ntp_seconds - 2208988800u, tz_minutes, out);
    return 0;
}
