/* PrismOS - CMOS real-time clock.
 *
 * Reads the battery-backed clock through ports 0x70/0x71, so the
 * taskbar clock shows the real time and the calendar app shows
 * the real date. The conversion from the clock's BCD digits
 * to binary is a pure function, so it is covered by the host
 * unit tests.
 */
#ifndef PRISM_RTC_H
#define PRISM_RTC_H

#include <stdint.h>

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t day;     /* 1-31 */
    uint8_t month;   /* 1-12 */
    uint8_t year;    /* 0-99, meaning 2000+year */
    uint8_t weekday; /* 0=Sunday .. 6=Saturday, from the clock */
} prism_rtc_time;

/* One BCD byte (0x00-0x59) to binary. Out-of-range digits are
 * clamped to the digit value, so a bad read cannot produce a
 * wild number. */
uint8_t rtc_bcd_to_bin(uint8_t v);

/* Read the current time and date. Returns 0 on success. */
int rtc_read(prism_rtc_time *out);

/* Binary (0-99) to one BCD byte. */
uint8_t rtc_bin_to_bcd(uint8_t v);

/* Write the time and date back to the CMOS clock (the
 * format bits of register B - BCD/binary and 12/24h -
 * are preserved). Returns 0 on success. */
int rtc_write(const prism_rtc_time *t);

/* Convert Unix seconds (UTC) plus a timezone offset in
 * minutes into broken-down local time. Pure function
 * (civil-from-days, integer only); years 1970-2100. */
void rtc_from_unix(uint32_t unix_sec, int tz_minutes,
                   prism_rtc_time *out);

/* Inverse of rtc_from_unix with tz_minutes = 0. */
uint32_t rtc_to_unix(const prism_rtc_time *t);

/* Convert an NTP timestamp (seconds since 1900) to
 * broken-down local time. Returns 0 on success, -1
 * when the timestamp predates the Unix epoch. */
int rtc_from_ntp(uint32_t ntp_seconds, int tz_minutes,
                 prism_rtc_time *out);

/* Format HH:MM into buf (needs at least 6 bytes). */
void rtc_format_hhmm(const prism_rtc_time *t, char *buf);

/* Format YYYY-MM-DD into buf (needs at least 11 bytes). */
void rtc_format_date(const prism_rtc_time *t, char *buf);

/* Days in a month (month 1-12, full year e.g. 2026). */
int rtc_days_in_month(int year, int month);

/* Weekday of a date (0=Sunday .. 6=Saturday), computed
 * from the date itself, so the calendar does not depend
 * on the clock's weekday register. */
int rtc_weekday(int year, int month, int day);

/* Seconds since 2000-01-01 00:00:00 (pure function).
 * ロックアウトの待ち時間など、時刻の差を測るときに使う。 */
uint32_t rtc_epoch_seconds(const prism_rtc_time *t);

#endif /* PRISM_RTC_H */
