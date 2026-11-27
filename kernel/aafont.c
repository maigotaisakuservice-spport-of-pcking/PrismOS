/* PrismOS - anti-aliased text rendering.
 *
 * Draws the generated Noto Sans CJK coverage tables
 * (aafont_data.c) over the framebuffer with per-pixel
 * blending. Glyphs are PackBits-decompressed into a
 * small LRU cache at master resolution; sizes without a
 * master are bilinearly resampled from the nearest one.
 * Codepoints the AA tables lack fall back to the bitmap
 * glyphs (drawn as solid pixels), so coverage never
 * drops below the bitmap font's.
 */
#include "aafont.h"

#include "fb.h"
#include "font.h"
#include "jfont.h"

/* ---- master selection ------------------------------------ */

static const prism_aa_size *aa_pick(int px, int bold) {
    const prism_aa_size *exact = NULL;
    const prism_aa_size *over = NULL;
    const prism_aa_size *largest = NULL;
    for (int i = 0; i < prism_aa_sizes_count; i++) {
        const prism_aa_size *t = &prism_aa_sizes[i];
        if ((t->weight != 0) != (bold != 0)) {
            continue;
        }
        if ((int)t->px == px) {
            exact = t;
        }
        if ((int)t->px > px &&
            (over == NULL || t->px < over->px)) {
            over = t;
        }
        if (largest == NULL || t->px > largest->px) {
            largest = t;
        }
    }
    if (exact != NULL) {
        return exact;
    }
    if (over != NULL) {
        return over;
    }
    return largest;
}

void fb_aa_line_metrics(int px, int bold, int *asc, int *desc) {
    const prism_aa_size *t = aa_pick(px, bold);
    if (t == NULL) {
        *asc = px;
        *desc = px / 4;
        return;
    }
    *asc = (int)t->asc * px / (int)t->px;
    *desc = (int)t->desc * px / (int)t->px;
}

/* ---- glyph records --------------------------------------- */

static const prism_aa_rec *aa_lookup(const prism_aa_size *t,
                                     uint32_t cp, int *kind,
                                     int *idx) {
    if (cp >= 32 && cp < 127) {
        *kind = 0;
        *idx = (int)cp - 32;
        return &t->ascii[cp - 32];
    }
    if (cp > 0xFFFF) {
        return NULL;
    }
    int lo = 0;
    int hi = PRISM_AA_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t v = prism_aa_cps[mid];
        if (v == cp) {
            *kind = 1;
            *idx = mid;
            return &t->wide[mid];
        }
        if (v < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

/* ---- decompression cache --------------------------------- */

#define AA_MAX_W 40
#define AA_MAX_H 40
#define AA_CACHE_N 128

typedef struct {
    const prism_aa_size *tab;
    int kind;
    int idx;
    uint8_t w;
    uint8_t h;
    uint32_t age;
    uint8_t data[AA_MAX_W * AA_MAX_H / 2];
} aa_ent;

static aa_ent aa_cache[AA_CACHE_N];
static uint32_t aa_age;

/* Byte i of a master's compressed blob (split into parts
 * of PRISM_AA_PART_LEN bytes in the generated data). */
static uint8_t aa_byte(const prism_aa_size *t, uint32_t i) {
    return t->parts[i >> PRISM_AA_PART_SHIFT]
                   [i & (PRISM_AA_PART_LEN - 1)];
}

/* PackBits-decode one glyph's packed-nibble coverage.
 * Returns 0 when the glyph does not fit the cache box. */
static int aa_decode(const prism_aa_size *t,
                     const prism_aa_rec *rec, uint8_t *out) {
    int need = ((rec->w + 1) / 2) * rec->h;
    uint32_t src = rec->off;
    uint32_t end = (rec + 1)->off;
    int produced = 0;
    while (src < end && produced < need) {
        uint8_t c = aa_byte(t, src++);
        if (c <= 127) {
            int n = (int)c + 1;
            while (n-- > 0 && src < end && produced < need) {
                out[produced++] = aa_byte(t, src++);
            }
        } else if (c >= 129) {
            int n = 257 - (int)c;
            if (src >= end) {
                break;
            }
            uint8_t v = aa_byte(t, src++);
            while (n-- > 0 && produced < need) {
                out[produced++] = v;
            }
        }
        /* 128 is a no-op in PackBits. */
    }
    return produced == need;
}

static const uint8_t *aa_glyph_data(const prism_aa_size *t,
                                    const prism_aa_rec *rec,
                                    int kind, int idx) {
    if (rec->w == 0 || rec->h == 0 || rec->off == 0xFFFFFFFFu) {
        return NULL;
    }
    if (rec->w > AA_MAX_W || rec->h > AA_MAX_H) {
        return NULL;
    }
    aa_ent *slot = NULL;
    aa_ent *oldest = NULL;
    for (int i = 0; i < AA_CACHE_N; i++) {
        aa_ent *e = &aa_cache[i];
        if (e->tab == t && e->kind == kind && e->idx == idx) {
            e->age = ++aa_age;
            return e->data;
        }
        if (oldest == NULL || e->age < oldest->age) {
            oldest = e;
        }
        if (e->tab == NULL) {
            slot = e;
            break;
        }
    }
    if (slot == NULL) {
        slot = oldest;
    }
    if (!aa_decode(t, rec, slot->data)) {
        return NULL;
    }
    slot->tab = t;
    slot->kind = kind;
    slot->idx = idx;
    slot->w = rec->w;
    slot->h = rec->h;
    slot->age = ++aa_age;
    return slot->data;
}

/* ---- blending -------------------------------------------- */

static void aa_blend(int64_t x, int64_t y, uint32_t fg_rgb,
                     int alpha) {
    if (alpha <= 0 || x < 0 || y < 0) {
        return;
    }
    if (alpha >= 255) {
        fb_put_pixel((uint64_t)x, (uint64_t)y,
                     fb_rgb((uint8_t)(fg_rgb >> 16),
                            (uint8_t)(fg_rgb >> 8),
                            (uint8_t)fg_rgb));
        return;
    }
    uint32_t bgc = fb_get_pixel_rgb((uint64_t)x, (uint64_t)y);
    int r = ((int)(fg_rgb >> 16) & 0xFF) * alpha +
            ((int)(bgc >> 16) & 0xFF) * (255 - alpha);
    int g = ((int)(fg_rgb >> 8) & 0xFF) * alpha +
            ((int)(bgc >> 8) & 0xFF) * (255 - alpha);
    int b = ((int)fg_rgb & 0xFF) * alpha +
            ((int)bgc & 0xFF) * (255 - alpha);
    fb_put_pixel((uint64_t)x, (uint64_t)y,
                 fb_rgb((uint8_t)((r + 127) / 255),
                        (uint8_t)((g + 127) / 255),
                        (uint8_t)((b + 127) / 255)));
}

static int aa_cov(const uint8_t *data, int w, int sx, int sy) {
    int byte = data[sy * ((w + 1) / 2) + sx / 2];
    int nib = (sx & 1) ? (byte & 0x0F) : (byte >> 4);
    return nib * 17;
}

/* Draw one glyph's coverage, resampled from the master
 * (tab->px) to the requested size (px). */
static void aa_draw_glyph(int64_t gx, int64_t gy,
                          const prism_aa_size *t,
                          const prism_aa_rec *rec,
                          const uint8_t *data, uint32_t fg_rgb,
                          int px) {
    int mw = rec->w;
    int mh = rec->h;
    if (mw <= 0 || mh <= 0) {
        return;
    }
    if (px == (int)t->px) {
        for (int yy = 0; yy < mh; yy++) {
            for (int xx = 0; xx < mw; xx++) {
                int a = aa_cov(data, mw, xx, yy);
                if (a > 0) {
                    aa_blend(gx + xx, gy + yy, fg_rgb, a);
                }
            }
        }
        return;
    }
    int dw = (mw * px + (int)t->px / 2) / (int)t->px;
    int dh = (mh * px + (int)t->px / 2) / (int)t->px;
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    for (int yy = 0; yy < dh; yy++) {
        /* Source coordinate (8.8 fixed) of this row's centre. */
        int fy = ((2 * yy + 1) * mh * 128) / dh - 128;
        if (fy < 0) {
            fy = 0;
        }
        if (fy > (mh - 1) * 256) {
            fy = (mh - 1) * 256;
        }
        int y0 = fy >> 8;
        int y1 = y0 + 1 < mh ? y0 + 1 : y0;
        int wy = fy & 0xFF;
        for (int xx = 0; xx < dw; xx++) {
            int fx = ((2 * xx + 1) * mw * 128) / dw - 128;
            if (fx < 0) {
                fx = 0;
            }
            if (fx > (mw - 1) * 256) {
                fx = (mw - 1) * 256;
            }
            int x0 = fx >> 8;
            int x1 = x0 + 1 < mw ? x0 + 1 : x0;
            int wx = fx & 0xFF;
            int c00 = aa_cov(data, mw, x0, y0);
            int c10 = aa_cov(data, mw, x1, y0);
            int c01 = aa_cov(data, mw, x0, y1);
            int c11 = aa_cov(data, mw, x1, y1);
            int top = c00 + (c10 - c00) * wx / 256;
            int bot = c01 + (c11 - c01) * wx / 256;
            int a = top + (bot - top) * wy / 256;
            if (a > 0) {
                aa_blend(gx + xx, gy + yy, fg_rgb, a);
            }
        }
    }
}

/* Bitmap fallback for codepoints the AA tables lack:
 * solid pixels from the 5x7 / 16x16 bitmap glyphs, sized
 * to roughly the requested em. Returns the advance used. */
static int aa_fallback(int64_t pen_x, int64_t ybase, uint32_t cp,
                       uint32_t fg_packed, int px) {
    uint32_t fg_rgb = fb_unpack_rgb(fg_packed);
    if (cp < 0x80) {
        int sc = px / 14;
        if (sc < 1) {
            sc = 1;
        }
        unsigned char ch = (unsigned char)cp;
        if (ch < 32 || ch > 126) {
            ch = '?';
        }
        const uint8_t *g = PRISM_FONT[ch - 32];
        int64_t top = ybase - 6 * sc;
        for (int row = 0; row < PRISM_FONT_HEIGHT; row++) {
            for (int col = 0; col < PRISM_FONT_WIDTH; col++) {
                if ((g[row] >> (PRISM_FONT_WIDTH - 1 - col)) & 1) {
                    for (int dy = 0; dy < sc; dy++) {
                        for (int dx = 0; dx < sc; dx++) {
                            aa_blend(pen_x + col * sc + dx,
                                     top + row * sc + dy, fg_rgb,
                                     255);
                        }
                    }
                }
            }
        }
        return px / 2 > 0 ? px / 2 : 1;
    }
    const uint8_t *g = jfont_glyph(cp);
    if (g == NULL) {
        return px;
    }
    int sc = px / 16;
    if (sc < 1) {
        sc = 1;
    }
    int64_t top = ybase - 14 * sc;
    for (int row = 0; row < PRISM_JFONT_ROWS; row++) {
        uint16_t bits =
            (uint16_t)((g[row * 2] << 8) | g[row * 2 + 1]);
        for (int col = 0; col < PRISM_JFONT_COLS; col++) {
            if ((bits >> (PRISM_JFONT_COLS - 1 - col)) & 1) {
                for (int dy = 0; dy < sc; dy++) {
                    for (int dx = 0; dx < sc; dx++) {
                        aa_blend(pen_x + col * sc + dx,
                                 top + row * sc + dy, fg_rgb, 255);
                    }
                }
            }
        }
    }
    return px;
}

/* Shared walker: measures (draw == 0) or draws a string.
 * Pen positions accumulate in 1/64 px so proportional
 * advances stay exact. */
static uint64_t aa_walk(int64_t x, int64_t ybase, const char *s,
                        uint32_t fg, int32_t bg, int px, int bold,
                        int draw) {
    if (s == NULL || px < 6) {
        return 0;
    }
    if (px > 96) {
        px = 96;
    }
    const prism_aa_size *tab = aa_pick(px, bold);
    if (tab == NULL) {
        return 0;
    }
    uint32_t fg_rgb = fb_unpack_rgb(fg);
    int64_t pen64 = x * 64;
    size_t remain = 0;
    while (s[remain] != '\0') {
        remain++;
    }
    const char *p = s;
    while (remain > 0) {
        uint32_t cp = 0;
        int n = jfont_utf8_decode(p, remain, &cp);
        if (n <= 0) {
            break;
        }
        p += n;
        remain -= (size_t)n;
        int kind = 0;
        int idx = 0;
        const prism_aa_size *gt = tab;
        const prism_aa_rec *rec = aa_lookup(gt, cp, &kind, &idx);
        if ((rec == NULL || rec->adv == 0) && bold) {
            /* Bold lacks a few glyphs: use the regular
             * master of the same size for this glyph. */
            const prism_aa_size *rt = aa_pick(px, 0);
            if (rt != NULL && rt != tab) {
                int k2 = 0;
                int i2 = 0;
                const prism_aa_rec *r2 = aa_lookup(rt, cp, &k2, &i2);
                if (r2 != NULL && r2->adv != 0) {
                    gt = rt;
                    rec = r2;
                    kind = k2;
                    idx = i2;
                }
            }
        }
        int64_t pen_x = pen64 / 64;
        if (rec == NULL || rec->adv == 0) {
            int adv;
            if (draw) {
                adv = aa_fallback(pen_x, ybase, cp, fg, px);
            } else {
                adv = (cp < 0x80) ? (px / 2 > 0 ? px / 2 : 1) : px;
            }
            pen64 += (int64_t)adv * 64;
            continue;
        }
        int adv64 = (int)rec->adv * px / (int)gt->px;
        if (draw) {
            if (bg >= 0) {
                int asc = (int)gt->asc * px / (int)gt->px;
                int top = (int)ybase - asc;
                int fh = ((int)gt->asc + (int)gt->desc) * px /
                         (int)gt->px;
                /* Clamp the painted box near the em so a
                 * glyph never repaints the row above. */
                int want_top = (int)ybase - px;
                if (top < want_top) {
                    fh -= want_top - top;
                    top = want_top;
                }
                if (fh > px + px / 4) {
                    fh = px + px / 4;
                }
                if (fh > 0 && adv64 > 0) {
                    fb_fill_rect((uint64_t)pen_x, (uint64_t)top,
                                 (uint64_t)((adv64 + 63) / 64),
                                 (uint64_t)fh, (uint32_t)bg);
                }
            }
            const uint8_t *data = aa_glyph_data(gt, rec, kind, idx);
            if (data != NULL) {
                int ox = (int)rec->ox * px / (int)gt->px;
                int oy = (int)rec->oy * px / (int)gt->px;
                aa_draw_glyph(pen_x + ox, ybase + oy, gt, rec, data,
                              fg_rgb, px);
            }
        }
        pen64 += adv64;
    }
    return (uint64_t)((pen64 - x * 64 + 32) / 64);
}

uint64_t fb_draw_text_aa(int64_t x, int64_t ybase, const char *s,
                         uint32_t fg, int32_t bg, int px, int bold) {
    return aa_walk(x, ybase, s, fg, bg, px, bold, 1);
}

uint64_t fb_text_aa_width(const char *s, int px, int bold) {
    return aa_walk(0, 0, s, 0, -1, px, bold, 0);
}
