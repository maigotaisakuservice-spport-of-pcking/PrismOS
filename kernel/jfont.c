/* PrismOS - Japanese font lookup and UTF-8 decoding.
 * See jfont.h. The glyph table (jfont_data.c) and the
 * sorted codepoint index (jfont_index.h) are generated
 * by scripts/gen-jfont.py; do not edit them by hand. */

#include "jfont.h"

#include "jfont_index.h"

int jfont_utf8_decode(const char *s, size_t len, uint32_t *cp) {
    if (len == 0) {
        *cp = 0xFFFD;
        return 0;
    }
    unsigned char b0 = (unsigned char)s[0];
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }
    int need;
    uint32_t val;
    if ((b0 & 0xE0) == 0xC0) {
        need = 2;
        val = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 3;
        val = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 4;
        val = b0 & 0x07;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if (len < (size_t)need) {
        *cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i < need; i++) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        val = (val << 6) | (uint32_t)(b & 0x3F);
    }
    /* Reject overlong forms and surrogates. */
    if ((need == 2 && val < 0x80) || (need == 3 && val < 0x800) ||
        (need == 4 && (val < 0x10000 || val > 0x10FFFF)) ||
        (val >= 0xD800 && val <= 0xDFFF)) {
        *cp = 0xFFFD;
        return 1;
    }
    *cp = val;
    return need;
}

const uint8_t *jfont_glyph(uint32_t cp) {
    if (cp > 0xFFFF) {
        return NULL;
    }
    int lo = 0;
    int hi = PRISM_JFONT_COUNT - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint16_t v = PRISM_JFONT_CPS[mid];
        if (v == cp) {
            return PRISM_JFONT_DATA[mid];
        }
        if (v < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

int jfont_is_wide(uint32_t cp) {
    /* Full-width ranges (East Asian Width F/W for the
     * repertoires this font covers): kana, kanji, CJK
     * symbols and full-width forms are wide; half-width
     * kana (U+FF61..U+FF9F) are not in the font and count
     * as narrow here. */
    if (cp >= 0x1100 && cp <= 0x115F) {
        return 1;
    }
    if (cp >= 0x2E80 && cp <= 0xA4CF) {
        return 1;
    }
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
        return 1;
    }
    if (cp >= 0xF900 && cp <= 0xFAFF) {
        return 1;
    }
    if (cp >= 0xFE30 && cp <= 0xFE6F) {
        return 1;
    }
    if (cp >= 0xFF00 && cp <= 0xFF60) {
        return 1;
    }
    if (cp >= 0xFFE0 && cp <= 0xFFE6) {
        return 1;
    }
    return 0;
}
