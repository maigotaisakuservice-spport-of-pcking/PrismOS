/* PrismOS - Japanese text support: UTF-8 decoding and the
 * 16x16 bitmap font generated from Noto Sans CJK (OFL 1.1,
 * see assets/NotoSansCJK-LICENSE.txt and jfont_index.h).
 *
 * The ASCII font stays the hand-drawn 5x7 one in font.h;
 * this module covers the JIS X 0208 repertoire (kana, kanji,
 * full-width symbols) for codepoints >= 0x80. */
#ifndef PRISM_JFONT_H
#define PRISM_JFONT_H

#include <stddef.h>
#include <stdint.h>

#define PRISM_JFONT_COLS 16
#define PRISM_JFONT_ROWS 16

/* Decode one UTF-8 character from s (at most len bytes).
 * Stores the codepoint in *cp and returns the number of
 * bytes consumed. Invalid or truncated input yields
 * U+FFFD and consumes 1 byte (or 0 when len is 0). */
int jfont_utf8_decode(const char *s, size_t len, uint32_t *cp);

/* 32-byte 16x16 glyph for a codepoint, or NULL when the
 * font has no glyph for it. */
const uint8_t *jfont_glyph(uint32_t cp);

/* 1 when the codepoint is a full-width character (kana,
 * kanji, full-width forms): it occupies two console
 * cells. 0 for half-width codepoints. */
int jfont_is_wide(uint32_t cp);

#endif /* PRISM_JFONT_H */
