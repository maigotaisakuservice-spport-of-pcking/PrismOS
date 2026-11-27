/* PrismOS — charset conversion tests. */
#include "../shell/charset.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                        \
    do {                                                   \
        checks++;                                          \
        if (!(cond)) {                                     \
            failures++;                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                 \
        }                                                  \
    } while (0)

static void conv(charset_kind k, const uint8_t *in, size_t n,
                 char *out, size_t cap) {
    size_t w = charset_to_utf8(k, in, n, out, cap - 1);
    out[w] = '\0';
}

int main(void) {
    char out[256];
    /* SJIS "日本語" = 93 FA 96 7B 8C EA */
    const uint8_t sjis[] = { 0x93, 0xFA, 0x96, 0x7B, 0x8C, 0xEA };
    conv(CHARSET_SJIS, sjis, sizeof(sjis), out, sizeof(out));
    CHECK(strcmp(out, "日本語") == 0);
    /* SJIS halfwidth katakana ｱ = B1 -> U+FF71 */
    const uint8_t kana[] = { 0xB1 };
    conv(CHARSET_SJIS, kana, 1, out, sizeof(out));
    CHECK(strcmp(out, "\xEF\xBD\xB1") == 0);
    /* EUC-JP "日本語" = C6 FC CB DC B8 EC */
    const uint8_t euc[] = { 0xC6, 0xFC, 0xCB, 0xDC, 0xB8, 0xEC };
    conv(CHARSET_EUCJP, euc, sizeof(euc), out, sizeof(out));
    CHECK(strcmp(out, "日本語") == 0);
    /* ISO-2022-JP "日本語" = ESC $ B 46 7C 4B 5C 38 6C ESC ( B */
    const uint8_t iso[] = { 0x1B, '$', 'B', 0x46, 0x7C, 0x4B,
                            0x5C, 0x38, 0x6C, 0x1B, '(', 'B' };
    conv(CHARSET_ISO2022JP, iso, sizeof(iso), out, sizeof(out));
    CHECK(strcmp(out, "日本語") == 0);
    /* mixed ASCII + SJIS */
    const uint8_t mix[] = { 'A', 0x93, 0xFA, 'Z' };
    conv(CHARSET_SJIS, mix, sizeof(mix), out, sizeof(out));
    CHECK(strcmp(out, "A日Z") == 0);
    /* unknown pair becomes U+FFFD, no crash */
    const uint8_t bad[] = { 0x81, 0x40, 0x81 };
    conv(CHARSET_SJIS, bad, sizeof(bad), out, sizeof(out));
    CHECK(out[0] != '\0');
    /* detection */
    CHECK(charset_detect(sjis, sizeof(sjis)) == CHARSET_SJIS);
    CHECK(charset_detect(euc, sizeof(euc)) == CHARSET_EUCJP);
    CHECK(charset_detect(iso, sizeof(iso)) == CHARSET_ISO2022JP);
    const uint8_t u8[] = { 0xE6, 0x97, 0xA5 }; /* 日 in UTF-8 */
    CHECK(charset_detect(u8, sizeof(u8)) == CHARSET_UTF8);
    CHECK(charset_detect((const uint8_t *)"abc", 3) ==
          CHARSET_ASCII);
    /* names */
    CHECK(charset_from_name("Shift_JIS") == CHARSET_SJIS);
    CHECK(charset_from_name("euc-jp") == CHARSET_EUCJP);
    CHECK(charset_from_name("ISO-2022-JP") == CHARSET_ISO2022JP);
    CHECK(charset_from_name("utf-8") == CHARSET_UTF8);
    /* truncation never splits a UTF-8 sequence: cap 4 fits
     * one 3-byte char + NUL room handled by caller buffer */
    char small[8];
    size_t w = charset_to_utf8(CHARSET_SJIS, sjis, sizeof(sjis),
                               small, 4);
    CHECK(w == 3);
    printf("charset tests: %d checks, %d failures\n", checks,
           failures);
    return failures != 0;
}
