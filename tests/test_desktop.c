/* PrismOS - 画面・設定・時計のホストテスト
 *
 * カーネルと同じ kernel/fb.c、kernel/settings.c、kernel/rtc.c、
 * kernel/font.h をホストの gcc でコンパイルして実行する。
 * QEMU は不要。`make test` で走る。
 * フレームワーク無し: CHECK マクロで数えて最後に集計する。
 *
 * ついでに build/font_sample.ppm を書き出す。フォントの見た目
 * を目で確認するためのもので、テストの合否には使わない。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fb.h"
#include "font.h"
#include "jfont.h"
#include "rtc.h"
#include "search.h"
#include "settings.h"
#include "wallpaper.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

#define FB_W 320
#define FB_H 120

static uint32_t fb_buf[FB_W * FB_H];

static uint32_t px(int x, int y) {
    return fb_buf[y * FB_W + x];
}


static int contains(const char *hay, const char *needle) {
    size_t hn = strlen(hay);
    size_t nn = strlen(needle);
    if (nn == 0) {
        return 1;
    }
    if (nn > hn) {
        return 0;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        if (strncmp(hay + i, needle, nn) == 0) {
            return 1;
        }
    }
    return 0;
}

static void dump_sample(void) {
    /* 画面全体に "PrismOS Terminal Files Settings 0123" を描く。 */
    fb_init(fb_buf, FB_W, FB_H, FB_W * 4, 32, 8, 16, 8, 8, 8, 0);
    fb_clear(fb_rgb(0x0B, 0x10, 0x26));
    fb_draw_text(8, 8, "PrismOS Terminal Files Settings", fb_rgb(0xFF, 0xFF, 0xFF),
                 fb_rgb(0x0B, 0x10, 0x26), 2);
    fb_draw_text(8, 40, "0123456789 abcdefghijklmnopqrstuvwxyz", fb_rgb(0x4C, 0xC2, 0xFF),
                 fb_rgb(0x0B, 0x10, 0x26), 1);
    fb_draw_text(8, 60, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", fb_rgb(0xFF, 0x6F, 0xB5),
                 fb_rgb(0x0B, 0x10, 0x26), 1);
    FILE *f = fopen("build/font_sample.ppm", "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (int i = 0; i < FB_W * FB_H; i++) {
        uint32_t p = fb_buf[i];
        unsigned char b[3] = {(unsigned char)(p & 0xFF),
                              (unsigned char)((p >> 8) & 0xFF),
                              (unsigned char)((p >> 16) & 0xFF)};
        /* fb は B,G,R の順で置く (32 ビットは BGRA 相当) ので、
         * PPM (RGB) に直す。 */
        unsigned char rgb[3] = {b[2], b[1], b[0]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(void) {
    /* ---- font: 全グリフの行は 5 ビット以内 ---- */
    for (int i = 0; i < 95; i++) {
        for (int r = 0; r < 7; r++) {
            CHECK(PRISM_FONT[i][r] <= 0x1F);
        }
    }
    /* 'A' の 1 行目は 0x0E (01110)、'0' の 1 行目も 0x0E。 */
    CHECK(PRISM_FONT['A' - 32][0] == 0x0E);
    CHECK(PRISM_FONT['0' - 32][0] == 0x0E);
    CHECK(PRISM_FONT[' ' - 32][0] == 0x00);

    /* ---- fb: 初期化と色 ---- */
    fb_init(fb_buf, FB_W, FB_H, FB_W * 4, 32, 8, 16, 8, 8, 8, 0);
    CHECK(fb_ready());
    CHECK(fb_width() == FB_W);
    CHECK(fb_height() == FB_H);
    CHECK(fb_rgb(0x11, 0x22, 0x33) == 0x00112233u);
    /* 24 ビットと 16 ビットは未対応で ready にならない。 */
    fb_init(fb_buf, FB_W, FB_H, FB_W * 3, 24, 8, 16, 8, 8, 8, 0);
    CHECK(fb_ready());
    fb_init(fb_buf, FB_W, FB_H, FB_W * 2, 16, 5, 11, 6, 5, 5, 0);
    CHECK(!fb_ready());
    fb_init(fb_buf, FB_W, FB_H, FB_W * 4, 32, 8, 16, 8, 8, 8, 0);

    /* ---- fb: ピクセルと矩形 ---- */
    fb_clear(fb_rgb(0, 0, 0));
    CHECK(px(0, 0) == 0);
    fb_put_pixel(10, 20, fb_rgb(0xFF, 0, 0));
    CHECK(px(10, 20) == fb_rgb(0xFF, 0, 0));
    CHECK(px(11, 20) == 0);
    fb_put_pixel(FB_W, 0, fb_rgb(0xFF, 0xFF, 0xFF)); /* 範囲外: 無視 */
    fb_put_pixel(0, FB_H, fb_rgb(0xFF, 0xFF, 0xFF));
    fb_fill_rect(5, 5, 3, 2, fb_rgb(0, 0xFF, 0));
    CHECK(px(5, 5) == fb_rgb(0, 0xFF, 0));
    CHECK(px(7, 6) == fb_rgb(0, 0xFF, 0));
    CHECK(px(8, 5) == 0);
    CHECK(px(5, 7) == 0);
    /* はみ出す矩形は画面内に切り詰める。 */
    fb_fill_rect(FB_W - 1, FB_H - 1, 10, 10, fb_rgb(0, 0, 0xFF));
    CHECK(px(FB_W - 1, FB_H - 1) == fb_rgb(0, 0, 0xFF));

    /* ---- fb: 文字と文字列 ---- */
    fb_clear(fb_rgb(0, 0, 0));
    fb_draw_char(0, 0, 'A', fb_rgb(0xFF, 0xFF, 0xFF), fb_rgb(0, 0, 0), 1);
    /* 'A' の 1 行目 (01110) の左から 2 ピクセル目は白、1 ピクセル目は黒。 */
    CHECK(px(1, 0) == fb_rgb(0xFF, 0xFF, 0xFF));
    CHECK(px(0, 0) == 0);
    /* 範囲外の文字は '?' になる。 */
    fb_draw_char(20, 0, '\x7F', fb_rgb(0xFF, 0xFF, 0xFF), fb_rgb(0, 0, 0), 1);
    CHECK(px(21, 0) == fb_rgb(0xFF, 0xFF, 0xFF)); /* '?' の 1 行目も 01110 */
    uint64_t w = fb_draw_text(0, 20, "AB", fb_rgb(0xFF, 0xFF, 0xFF),
                              fb_rgb(0, 0, 0), 1);
    CHECK(w == 12); /* 2 文字 x (5+1) ピクセル */
    w = fb_draw_text(0, 40, "A", fb_rgb(0xFF, 0xFF, 0xFF), fb_rgb(0, 0, 0), 2);
    CHECK(w == 12); /* scale 2 で 1 文字 12 ピクセル */
    /* ---- jfont: UTF-8 decode, glyph lookup, wide text ---- */
    {
        uint32_t cp = 0;
        /* 日 = U+65E5 = E6 97 A5 */
        CHECK(jfont_utf8_decode("\xE6\x97\xA5", 3, &cp) == 3);
        CHECK(cp == 0x65E5);
        CHECK(jfont_utf8_decode("A", 1, &cp) == 1 && cp == 'A');
        CHECK(jfont_utf8_decode("\xFF", 1, &cp) == 1 && cp == 0xFFFD);
        CHECK(jfont_utf8_decode("\xE6\x97", 2, &cp) == 1 &&
              cp == 0xFFFD); /* truncated */
        CHECK(jfont_utf8_decode("\xC0\x80", 2, &cp) == 1 &&
              cp == 0xFFFD); /* overlong */
        const uint8_t *g = jfont_glyph(0x65E5); /* 日 */
        CHECK(g != NULL);
        int any = 0;
        for (int i = 0; i < 32; i++) {
            if (g[i] != 0) {
                any = 1;
            }
        }
        CHECK(any); /* the glyph is not blank */
        CHECK(jfont_glyph(0x4E00) != NULL); /* 一 */
        CHECK(jfont_glyph(0x3042) != NULL); /* あ */
        CHECK(jfont_glyph(0x30A2) != NULL); /* ア */
        CHECK(jfont_glyph(0x10FFFF) == NULL);
        CHECK(jfont_is_wide(0x65E5) == 1);
        CHECK(jfont_is_wide(0x3042) == 1);
        CHECK(jfont_is_wide('A') == 0);
        /* Mixed string width: A (6) + 日 (18) + B (6). */
        fb_clear(fb_rgb(0, 0, 0));
        w = fb_draw_text(0, 60, "A\xE6\x97\xA5" "B",
                         fb_rgb(0xFF, 0xFF, 0xFF), fb_rgb(0, 0, 0), 1);
        CHECK(w == 30);
        /* The wide glyph really painted pixels right of x=6. */
        int painted = 0;
        for (int y = 60; y < 76 && !painted; y++) {
            for (int x = 6; x < 22 && !painted; x++) {
                if (px(x, y) == fb_rgb(0xFF, 0xFF, 0xFF)) {
                    painted = 1;
                }
            }
        }
        CHECK(painted);
        /* Every Japanese char used by the shell help text
         * must have a glyph. */
        const char *help_chars =
            "日本語のヘルプです。コマンド一覧を表示します。"
            "設定を変更できます。画面に表示されます。";
        const char *p = help_chars;
        while (*p != '\0') {
            uint32_t c2 = 0;
            int n = jfont_utf8_decode(p, 3, &c2);
            if (c2 >= 0x80) {
                CHECK(jfont_glyph(c2) != NULL);
            }
            p += n;
        }
    }
    /* 線: 水平線は両端とも描かれる。 */
    fb_clear(fb_rgb(0, 0, 0));
    fb_draw_line(0, 10, 9, 10, fb_rgb(0xFF, 0xFF, 0xFF));
    CHECK(px(0, 10) == fb_rgb(0xFF, 0xFF, 0xFF));
    CHECK(px(9, 10) == fb_rgb(0xFF, 0xFF, 0xFF));
    CHECK(px(5, 11) == 0);

    /* ---- settings: 既定値と往復 ---- */
    prism_settings s;
    settings_defaults(&s);
    CHECK(s.accent_rgb == 0x4CC2FF);
    CHECK(s.taskbar_top == 0);
    CHECK(s.dark_mode == 1);
    char buf[1024]; /* 設定が増えたため 512 では足りない。 */
    size_t n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(contains(buf, "accent=#4CC2FF"));
    CHECK(contains(buf, "taskbar=bottom"));
    CHECK(contains(buf, "mode=dark"));
    CHECK(contains(buf, "install_profile=standard"));
    prism_settings s2;
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.accent_rgb == 0x4CC2FF);
    CHECK(s2.taskbar_top == 0);
    CHECK(s2.dark_mode == 1);
    /* 変更して往復する。 */
    s.accent_rgb = 0xFF6FB5;
    s.taskbar_top = 1;
    s.dark_mode = 0;
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "accent=#FF6FB5"));
    CHECK(contains(buf, "taskbar=top"));
    CHECK(contains(buf, "mode=light"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.accent_rgb == 0xFF6FB5);
    CHECK(s2.taskbar_top == 1);
    CHECK(s2.dark_mode == 0);
    /* tz_offset と update_channel の往復 (直列化の
     * 順序バグ回帰: 値が隣のキーに食われていた)。 */
    s.tz_offset = -330;
    s.update_channel = 1;
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "tz_offset=-330"));
    CHECK(contains(buf, "update_channel=beta"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.tz_offset == -330);
    CHECK(s2.update_channel == 1);
    /* ハイコントラストの往復 (アクセシビリティ)。 */
    s.high_contrast = 1;
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "contrast=on"));
    settings_defaults(&s2);
    CHECK(s2.high_contrast == 0);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.high_contrast == 1);
    /* アクセシビリティ3項目の往復 (文字サイズ・画面
     * キーボード・ナレーター)。 */
    settings_defaults(&s2);
    CHECK(s2.text_scale == 1);
    CHECK(s2.osk == 0);
    CHECK(s2.narrator == 0);
    s.text_scale = 2;
    s.osk = 1;
    s.narrator = 1;
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "text_scale=2"));
    CHECK(contains(buf, "osk=on"));
    CHECK(contains(buf, "narrator=on"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.text_scale == 2);
    CHECK(s2.osk == 1);
    CHECK(s2.narrator == 1);
    /* デスクトップ側オーバーレイが3項目を運ぶ。 */
    {
        prism_settings dst, src;
        settings_defaults(&dst);
        settings_defaults(&src);
        src.text_scale = 2;
        src.osk = 1;
        src.narrator = 1;
        settings_overlay_desktop(&dst, &src);
        CHECK(dst.text_scale == 2);
        CHECK(dst.osk == 1);
        CHECK(dst.narrator == 1);
    }
    /* 不明な行は無視し、壊れた色は無視する。 */
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "hello\naccent=#ZZZZZZ\n", 21) == 0);
    CHECK(s2.accent_rgb == 0x4CC2FF);
    /* インストール構成の往復。 */
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "install_profile=minimal\n", 24) == 1);
    CHECK(s2.install_profile == 1);
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "install_profile=standard\n", 25) == 1);
    CHECK(s2.install_profile == 0);
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "install_profile=bogus\n", 22) == 0);
    CHECK(s2.install_profile == 0);
    /* セキュリティ設定の往復 (フェーズ14)。 */
    settings_defaults(&s2);
    CHECK(s2.usb_media == 0);
    CHECK(s2.pin_min_len == 4);
    CHECK(s2.pin_require_digit == 0);
    CHECK(s2.lockout_attempts == 3);
    CHECK(s2.lockout_seconds == 300);
    s.usb_media = 1;
    s.pin_min_len = 6;
    s.pin_require_digit = 1;
    s.lockout_attempts = 5;
    s.lockout_seconds = 60;
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "usb_media=block"));
    CHECK(contains(buf, "pin_min_len=6"));
    CHECK(contains(buf, "pin_require_digit=on"));
    CHECK(contains(buf, "lockout_attempts=5"));
    CHECK(contains(buf, "lockout_seconds=60"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.usb_media == 1);
    CHECK(s2.pin_min_len == 6);
    CHECK(s2.pin_require_digit == 1);
    CHECK(s2.lockout_attempts == 5);
    CHECK(s2.lockout_seconds == 60);
    /* 範囲外は無視される。 */
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "pin_min_len=99\n", 15) == 0);
    CHECK(s2.pin_min_len == 4);
    /* 4 つのアクセント色は互いに違う。 */
    CHECK(PRISM_ACCENT_COUNT == 7);
    CHECK(PRISM_ACCENTS[0] != PRISM_ACCENTS[1]);
    CHECK(PRISM_ACCENTS[1] != PRISM_ACCENTS[2]);
    CHECK(PRISM_ACCENTS[2] != PRISM_ACCENTS[3]);
    CHECK(PRISM_ACCENTS[3] == 0x4CC26B); /* 緑 */
    char hex[8];
    settings_accent_hex(0x4CC2FF, hex);
    CHECK(strcmp(hex, "#4CC2FF") == 0);
    /* 緑も設定として保存・読み込みできる。 */
    {
        prism_settings gs;
        char gbuf[512];
        settings_defaults(&gs);
        gs.accent_rgb = 0x4CC26B;
        settings_serialize(&gs, gbuf, sizeof(gbuf));
        CHECK(contains(gbuf, "accent=#4CC26B"));
        prism_settings gs2;
        settings_defaults(&gs2);
        CHECK(settings_parse(&gs2, gbuf, strlen(gbuf)) == 1);
        CHECK(gs2.accent_rgb == 0x4CC26B);
    }

    /* ---- rtc: BCD 変換と時刻の書式 ---- */
    CHECK(rtc_bcd_to_bin(0x59) == 59);
    CHECK(rtc_bcd_to_bin(0x00) == 0);
    CHECK(rtc_bcd_to_bin(0x23) == 23);
    CHECK(rtc_bcd_to_bin(0x08) == 8);
    /* ---- rtc: date, weekday and month lengths ---- */
    CHECK(rtc_days_in_month(2026, 2) == 28);
    CHECK(rtc_days_in_month(2024, 2) == 29);
    CHECK(rtc_days_in_month(2026, 11) == 30);
    CHECK(rtc_days_in_month(2026, 1) == 31);
    /* 2026-11-21 was a Saturday; 2026-11-01 was a Sunday. */
    CHECK(rtc_weekday(2026, 11, 21) == 6);
    /* bin<->bcd and unix conversions */
    CHECK(rtc_bin_to_bcd(59) == 0x59);
    CHECK(rtc_bin_to_bcd(9) == 0x09);
    CHECK(rtc_bin_to_bcd(23) == 0x23);
    {
        prism_rtc_time u;
        /* 2026-11-22 10:30:05 UTC = 1795347005? compute via
         * the inverse instead: roundtrip both ways. */
        rtc_from_unix(1795343405u, 0, &u);
        CHECK(u.year == 26 && u.month == 11 && u.day == 22);
        CHECK(u.hour == 10 && u.minute == 30 && u.second == 5);
        CHECK(rtc_to_unix(&u) == 1795343405u);
        rtc_from_unix(1795343405u, 540, &u);
        CHECK(u.hour == 19 && u.day == 22);
        prism_rtc_time e;
        e.year = 0; e.month = 1; e.day = 1;
        e.hour = 0; e.minute = 0; e.second = 0;
        CHECK(rtc_to_unix(&e) == 946684800u);
        rtc_from_unix(946684800u, 0, &u);
        CHECK(u.year == 0 && u.month == 1 && u.day == 1);
        /* NTP conversion: 2031-06-15 08:00 UTC at +09:00
         * is 17:00 local. */
        prism_rtc_time nt;
        CHECK(rtc_from_ntp(1939276800u + 2208988800u, 540,
                           &nt) == 0);
        CHECK(nt.year == 31 && nt.month == 6 && nt.day == 15);
        CHECK(nt.hour == 17 && nt.minute == 0);
        CHECK(rtc_from_ntp(1000u, 0, &nt) == -1);
        /* leap day roundtrip */
        rtc_from_unix(1709164800u, 0, &u); /* 2024-02-29 */
        CHECK(u.year == 24 && u.month == 2 && u.day == 29);
    }
    CHECK(rtc_weekday(2026, 11, 1) == 0);
    CHECK(rtc_weekday(2026, 1, 1) == 4);
    /* ---- rtc: 通算秒 (2000-01-01 からの秒数) ---- */
    {
        prism_rtc_time e0 = {0, 0, 0, 1, 1, 0, 6};
        CHECK(rtc_epoch_seconds(&e0) == 0);
        prism_rtc_time e1 = {0, 0, 1, 1, 1, 0, 6};
        CHECK(rtc_epoch_seconds(&e1) == 1);
        prism_rtc_time e2 = {0, 0, 0, 2, 1, 0, 0};
        CHECK(rtc_epoch_seconds(&e2) == 86400);
        /* 2024 年はうるう年: 2024-03-01 は 2000-01-01 から
         * 8826 日後 (24 年分 8766 日 + 1 月 31 日 + 2 月 29 日)。 */
        prism_rtc_time e3 = {0, 0, 0, 1, 3, 24, 5};
        CHECK(rtc_epoch_seconds(&e3) == 8826u * 86400u);
        prism_rtc_time e4 = {1, 2, 3, 1, 1, 0, 6};
        CHECK(rtc_epoch_seconds(&e4) == 3723);
        CHECK(rtc_epoch_seconds(NULL) == 0);
    }
    prism_rtc_time dt = {8, 30, 5, 21, 11, 26, 6};
    char dstr[11];
    rtc_format_date(&dt, dstr);
    CHECK(strcmp(dstr, "2026-11-21") == 0);

    prism_rtc_time t = {8, 30, 5, 21, 11, 26, 6};
    char hhmm[6];
    rtc_format_hhmm(&t, hhmm);
    CHECK(strcmp(hhmm, "08:30") == 0);
    t.hour = 23;
    t.minute = 5;
    rtc_format_hhmm(&t, hhmm);
    CHECK(strcmp(hhmm, "23:05") == 0);

    /* ---- settings: wallpaper field round-trips ---- */
    settings_defaults(&s);
    CHECK(s.wallpaper_image == 1);
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "wallpaper=image"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.wallpaper_image == 1);
    s.wallpaper_image = 0;
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "wallpaper=drawn"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.wallpaper_image == 0);
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "wallpaper=bogus\n", 16) == 0);
    CHECK(s2.wallpaper_image == 1);

    /* ---- settings: Dock show/hide round-trips ----
     * 既定は非表示 (タスクバーにアイコンを集約する
     * レイアウト)。show/hide の往復は従来通り。 */
    settings_defaults(&s);
    CHECK(s.dock_show == 0);
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "dock=hide"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.dock_show == 0);
    s.dock_show = 1;
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "dock=show"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.dock_show == 1);
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "dock=bogus\n", 11) == 0);
    CHECK(s2.dock_show == 0);

    /* ---- settings: タスクバーのアイコン配置 ---- */
    settings_defaults(&s);
    CHECK(s.taskbar_center == 1);
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "taskbar_icons=center"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.taskbar_center == 1);
    s.taskbar_center = 0;
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "taskbar_icons=left"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.taskbar_center == 0);
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "taskbar_icons=bogus\n", 19) == 0);
    CHECK(s2.taskbar_center == 1);

    /* ---- settings: ショートカットの割り当て ---- */
    settings_defaults(&s);
    /* 既定値は従来のキー (F1.. と Ctrl 組み合わせ)。 */
    CHECK(s.binds[PRISM_ACT_TERMINAL] == 0x81); /* F1 */
    CHECK(s.binds[PRISM_ACT_BROWSER] == 0x84);  /* F4 */
    CHECK(s.binds[PRISM_ACT_SEARCH] == 0x88);   /* F8 */
    CHECK(s.binds[PRISM_ACT_SHEET] == 0xA1);    /* C-o */
    CHECK(s.binds[PRISM_ACT_SLIDES] == 0xA6);   /* C-p */
    /* 既定値に重複がない。 */
    for (int i = 0; i < PRISM_BIND_COUNT; i++) {
        for (int j = i + 1; j < PRISM_BIND_COUNT; j++) {
            CHECK(s.binds[i] != s.binds[j]);
        }
    }
    n = settings_serialize(&s, buf, sizeof(buf));
    /* With all 17 bind lines the file is over 512 bytes;
     * every fixed-size reader of settings.txt must use a
     * buffer of at least the serialize cap (1024). A 512-byte
     * reader silently failed to see usb_media=block. */
    CHECK(n > 512);
    CHECK(n <= 1024);
    CHECK(contains(buf, "bind_terminal=F1"));
    CHECK(contains(buf, "bind_browser=F4"));
    CHECK(contains(buf, "bind_sheet=C-o"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    for (int i = 0; i < PRISM_BIND_COUNT; i++) {
        CHECK(s2.binds[i] == s.binds[i]);
    }
    /* Overlays for merged saves: each side keeps only its
     * own fields, the rest survives from the other side. */
    {
        prism_settings dst, src;
        settings_defaults(&dst);
        settings_defaults(&src);
        dst.accent_rgb = 0x111111;
        dst.usb_media = 0;
        src.accent_rgb = 0x222222;
        src.usb_media = 1;
        src.lockout_attempts = 7;
        src.binds[PRISM_ACT_BROWSER] = 0x89;
        settings_overlay_security(&dst, &src);
        CHECK(dst.usb_media == 1);
        CHECK(dst.lockout_attempts == 7);
        CHECK(dst.accent_rgb == 0x111111); /* not overlaid */
        settings_overlay_desktop(&dst, &src);
        CHECK(dst.accent_rgb == 0x222222);
        CHECK(dst.binds[PRISM_ACT_BROWSER] == 0x89);
        CHECK(dst.usb_media == 1); /* kept from before */
    }
    /* 変更して往復する (Browser を F9 に、Mail を C-b に)。 */
    s.binds[PRISM_ACT_BROWSER] = 0x89; /* F9 */
    s.binds[PRISM_ACT_MAIL] = 0xB0 + ('b' - 'a');
    n = settings_serialize(&s, buf, sizeof(buf));
    CHECK(contains(buf, "bind_browser=F9"));
    CHECK(contains(buf, "bind_mail=C-b"));
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, buf, n) == 1);
    CHECK(s2.binds[PRISM_ACT_BROWSER] == 0x89);
    CHECK(s2.binds[PRISM_ACT_MAIL] == 0xB0 + ('b' - 'a'));
    /* 壊れた割り当て行は無視される。 */
    settings_defaults(&s2);
    CHECK(settings_parse(&s2, "bind_browser=F13\n", 17) == 0);
    CHECK(s2.binds[PRISM_ACT_BROWSER] == 0x84);
    CHECK(settings_parse(&s2, "bind_nope=F2\n", 13) == 0);
    /* キー名の往復 (全ての割り当て可能なコード)。 */
    for (int code = 0x81; code <= 0x8C; code++) {
        char kn[8];
        CHECK(settings_key_name(code, kn, sizeof(kn)) > 0);
        CHECK(settings_key_parse(kn) == code);
    }
    for (char L = 'a'; L <= 'z'; L++) {
        char nm[4] = {'C', '-', L, '\0'};
        int code = settings_key_parse(nm);
        CHECK(code >= 0x81);
        char kn[8];
        CHECK(settings_key_name(code, kn, sizeof(kn)) == 3);
        CHECK(kn[0] == 'C' && kn[1] == '-' && kn[2] == L);
    }
    CHECK(settings_key_parse("F0") == -1);
    CHECK(settings_key_parse("F13") == -1);
    CHECK(settings_key_parse("C-1") == -1);
    CHECK(settings_key_parse("") == -1);
    CHECK(settings_key_name(0x20, buf, sizeof(buf)) == -1);

    /* ---- settings: 全項目最大長でも切り捨てない (監査 M1) ----
     * 長いフィード URL 等を全項目に入れても、末尾のセキュリ
     * ティ項目 (usb_media / ロックアウト) と bind_ 行まで
     * 保存・復元できること。旧 1024 バイトバッファでは末尾が
     * 静かに欠落していた。 */
    {
        prism_settings ms;
        settings_defaults(&ms);
        for (size_t i = 0; i + 1 < sizeof(ms.feed_weather); i++) {
            ms.feed_weather[i] = 'w';
        }
        ms.feed_weather[sizeof(ms.feed_weather) - 1] = '\0';
        for (size_t i = 0; i + 1 < sizeof(ms.feed_news); i++) {
            ms.feed_news[i] = 'n';
        }
        ms.feed_news[sizeof(ms.feed_news) - 1] = '\0';
        for (size_t i = 0; i + 1 < sizeof(ms.update_host); i++) {
            ms.update_host[i] = 'h';
        }
        ms.update_host[sizeof(ms.update_host) - 1] = '\0';
        for (size_t i = 0; i + 1 < sizeof(ms.update_path); i++) {
            ms.update_path[i] = 'p';
        }
        ms.update_path[sizeof(ms.update_path) - 1] = '\0';
        for (size_t i = 0; i + 1 < sizeof(ms.wallpaper_file); i++) {
            ms.wallpaper_file[i] = 'f';
        }
        ms.wallpaper_file[sizeof(ms.wallpaper_file) - 1] = '\0';
        ms.wallpaper_image = 2;
        strcpy(ms.net_ip, "255.255.255.255");
        strcpy(ms.net_mask, "255.255.255.255");
        strcpy(ms.net_gw, "255.255.255.255");
        strcpy(ms.net_dns, "255.255.255.255");
        ms.net_static = 1;
        ms.taskbar_side = 2;
        ms.usb_media = 1;
        ms.pin_min_len = 16;
        ms.pin_require_digit = 1;
        ms.lockout_attempts = 10;
        ms.lockout_seconds = 3600;
        ms.hidden_apps = 40;
        ms.tz_offset = -720;
        for (int a = 0; a < PRISM_BIND_COUNT; a++) {
            ms.binds[a] = 0x8C; /* F12 */
        }
        static char big[SETTINGS_TEXT_CAP];
        size_t bn = settings_serialize(&ms, big, sizeof(big));
        CHECK(bn > 1024);        /* 旧バッファには収まらない */
        CHECK(bn < sizeof(big)); /* 新バッファには収まる */
        CHECK(big[bn - 1] == '\n');
        prism_settings back;
        settings_defaults(&back);
        CHECK(settings_parse(&back, big, bn) == 1);
        CHECK(back.usb_media == 1);
        CHECK(back.pin_min_len == 16);
        CHECK(back.pin_require_digit == 1);
        CHECK(back.lockout_attempts == 10);
        CHECK(back.lockout_seconds == 3600);
        CHECK(back.hidden_apps == 40);
        CHECK(strcmp(back.feed_weather, ms.feed_weather) == 0);
        CHECK(strcmp(back.feed_news, ms.feed_news) == 0);
        CHECK(strcmp(back.update_host, ms.update_host) == 0);
        CHECK(strcmp(back.update_path, ms.update_path) == 0);
        CHECK(strcmp(back.wallpaper_file, ms.wallpaper_file) == 0);
        CHECK(back.binds[PRISM_ACT_STORE] == 0x8C);
        /* 小さいバッファでは「必要長」が返り、切り捨てを
         * 検出できること (保存側はこれを見て保存を止める)。 */
        char small[1024];
        size_t sn = settings_serialize(&ms, small, sizeof(small));
        CHECK(sn == bn);
        CHECK(sn >= sizeof(small));
        CHECK(strlen(small) == sizeof(small) - 1);
    }
    /* 途中で切れた最終行の値は受理しない (監査 M1)。
     * 改行で終わっていない行は、300 が 3 に切れたような
     * 途中切断かもしれないので無視する。 */
    {
        prism_settings ts;
        settings_defaults(&ts);
        CHECK(settings_parse(&ts, "lockout_seconds=3", 17) == 0);
        CHECK(ts.lockout_seconds == 300);
        CHECK(settings_parse(&ts, "lockout_seconds=3\n", 18) == 1);
        CHECK(ts.lockout_seconds == 3);
    }

    /* ---- wallpaper: the real BMP file parses and draws ---- */
    FILE *bmp = fopen("kernel/wallpaper.bmp", "rb");
    CHECK(bmp != NULL);
    if (bmp != NULL) {
        fseek(bmp, 0, SEEK_END);
        long bmp_len = ftell(bmp);
        fseek(bmp, 0, SEEK_SET);
        CHECK(bmp_len == 3072054);
        uint8_t *bmp_buf = (uint8_t *)malloc((size_t)bmp_len);
        CHECK(bmp_buf != NULL);
        if (bmp_buf != NULL) {
            CHECK(fread(bmp_buf, 1, (size_t)bmp_len, bmp) ==
                  (size_t)bmp_len);
            wallpaper_img img;
            CHECK(wallpaper_parse(bmp_buf, (size_t)bmp_len, &img) == 0);
            CHECK(img.width == 1280);
            CHECK(img.height == 800);
            CHECK(img.stride == 3840);
            CHECK(img.top_down == 0);
            /* Bad headers are rejected. */
            CHECK(wallpaper_parse(bmp_buf, 10, &img) != 0);
            uint8_t bad[54];
            memcpy(bad, bmp_buf, 54);
            bad[0] = 'X';
            CHECK(wallpaper_parse(bad, 54, &img) != 0);
            memcpy(bad, bmp_buf, 54);
            bad[28] = 32; /* 32 bpp: not supported */
            CHECK(wallpaper_parse(bad, 54, &img) != 0);
            /* Crafted dimensions (pentest F3 regression): a huge
             * width must not wrap the stride and be accepted. */
            {
                uint8_t hdr[58];
                memset(hdr, 0, sizeof(hdr));
                hdr[0] = 'B';
                hdr[1] = 'M';
                hdr[10] = 54;  /* data offset */
                hdr[14] = 40;  /* header size */
                hdr[26] = 1;   /* planes */
                hdr[28] = 24;  /* bpp */
                hdr[22] = 1;   /* height 1 */
                /* width = 0x55555556 (stride wrapped to 4 pre-fix) */
                hdr[18] = 0x56;
                hdr[19] = 0x55;
                hdr[20] = 0x55;
                hdr[21] = 0x55;
                CHECK(wallpaper_parse(hdr, sizeof(hdr), &img) != 0);
                /* width = 0x40000001 (int stride overflow pre-fix) */
                hdr[18] = 0x01;
                hdr[19] = 0x00;
                hdr[20] = 0x00;
                hdr[21] = 0x40;
                CHECK(wallpaper_parse(hdr, sizeof(hdr), &img) != 0);
                /* width = 100 needs 54 + 304 = 358 bytes, not 58 */
                hdr[18] = 100;
                hdr[19] = 0;
                hdr[20] = 0;
                hdr[21] = 0;
                CHECK(wallpaper_parse(hdr, sizeof(hdr), &img) != 0);
            }
            /* A small valid BMP still parses, including top-down. */
            {
                static uint8_t tiny[70];
                memset(tiny, 0, sizeof(tiny));
                tiny[0] = 'B';
                tiny[1] = 'M';
                tiny[10] = 54;
                tiny[14] = 40;
                tiny[26] = 1;
                tiny[28] = 24;
                tiny[18] = 2; /* width */
                tiny[22] = 2; /* height */
                CHECK(wallpaper_parse(tiny, sizeof(tiny), &img) == 0);
                CHECK(img.width == 2 && img.height == 2 &&
                      img.stride == 8 && img.top_down == 0);
                /* height = -2: top-down */
                tiny[22] = 0xFE;
                tiny[23] = 0xFF;
                tiny[24] = 0xFF;
                tiny[25] = 0xFF;
                CHECK(wallpaper_parse(tiny, sizeof(tiny), &img) == 0);
                CHECK(img.height == 2 && img.top_down == 1);
            }
            /* Dimension cap boundary: 8192 parses, 8193 is rejected
             * even when the file is long enough. */
            {
                static uint8_t wide[24640];
                memset(wide, 0, sizeof(wide));
                wide[0] = 'B';
                wide[1] = 'M';
                wide[10] = 54;
                wide[14] = 40;
                wide[26] = 1;
                wide[28] = 24;
                wide[22] = 1; /* height 1 */
                wide[18] = 0x00;
                wide[19] = 0x20; /* width 8192 */
                CHECK(wallpaper_parse(wide, 54 + 24576, &img) == 0);
                CHECK(img.width == 8192 && img.stride == 24576);
                wide[18] = 0x01; /* width 8193 */
                CHECK(wallpaper_parse(wide, 54 + 24580, &img) != 0);
            }
            /* Draw the image scaled onto the fake framebuffer and
             * check two pixels against the BMP bytes directly. */
            CHECK(wallpaper_parse(bmp_buf, (size_t)bmp_len, &img) == 0);
            fb_init(fb_buf, FB_W, FB_H, FB_W * 4, 32, 8, 16, 8, 8, 8, 0);
            fb_draw_image_rgb24(0, 0, FB_W, FB_H, img.pixels, img.width,
                                img.height, img.stride, img.top_down);
            /* Screen (0,0) is image top-left: BMP pixel (0, 0)
             * top-origin, RGB (1, 7, 25). */
            CHECK(px(0, 0) == fb_rgb(1, 7, 25));
            /* Screen (160,60) maps to image (640, 400) top-origin. */
            {
                int sx = 640;
                int sy_img = 400;
                int row = img.height - 1 - sy_img;
                const uint8_t *sp = img.pixels + (size_t)row * img.stride +
                                    (size_t)sx * 3;
                CHECK(px(160, 60) == fb_rgb(sp[2], sp[1], sp[0]));
            }
            free(bmp_buf);
        }
        fclose(bmp);
    }

    /* ---- search: case-insensitive substring match ---- */
    CHECK(search_match("file", "Files") == 1);
    CHECK(search_match("FILE", "Files") == 1);
    CHECK(search_match("cal", "Calendar") == 1);
    CHECK(search_match("xyz", "Calendar") == 0);
    CHECK(search_match("", "Calendar") == 1);
    CHECK(search_match("note", "note.txt") == 1);
    CHECK(search_match("txt", "note.txt") == 1);
    CHECK(search_match("settings", "Settings") == 1);
    CHECK(search_match("set", "Settings") == 1);
    CHECK(search_match("browser", "Browser") == 1);
    CHECK(search_match("term", "Terminal") == 1);
    CHECK(search_match("longquery", "Files") == 0);
    CHECK(search_match(NULL, "Files") == 0);
    CHECK(search_match("file", NULL) == 0);

    dump_sample();

    if (failures == 0) {
        printf("desktop tests: %d checks, 0 failures\n", checks);
        printf("PASS\n");
    } else {
        printf("desktop tests: %d checks, %d failures\n", checks, failures);
        printf("FAIL\n");
    }
    return failures == 0 ? 0 : 1;
}
