/* PrismOS - framebuffer driver. */
#include "fb.h"
#include "font.h"
#include "jfont.h"
#include "pmm.h"
#include "string.h"

static uint8_t *fb_addr;
static uint64_t fb_w;
static uint64_t fb_h;
static uint64_t fb_pitch;
static uint64_t fb_bpp;
static uint8_t fb_rsize;
static uint8_t fb_rshift;
static uint8_t fb_gsize;
static uint8_t fb_gshift;
static uint8_t fb_bsize;
static uint8_t fb_bshift;
static int fb_ok;

/* 二重バッファ: 描画はまずシャドウ (通常メモリ) へ
 * 行い、fb_flush() で実フレームバッファへ一括コピー
 * する。画面のちらつき (描画途中の状態が見える) を
 * 防ぐ。シャドウを確保できない場合は従来通り直接
 * 描画する。 */
#ifndef PRISM_FB_NO_SHADOW
static uint8_t *fb_shadow;
static uint64_t fb_shadow_pages;
static uint64_t fb_hhdm;
#endif
static int fb_dirty;

/* 第 2 ディスプレイへのミラー先 (無ければ NULL)。 */
static uint8_t *fb_mirror;
static uint64_t fb_mirror_pitch;
static uint64_t fb_mirror_h;

void fb_set_mirror(void *addr, uint64_t pitch, uint64_t height,
                   uint16_t bpp) {
    if (addr != NULL && pitch > 0 && height > 0 &&
        (bpp == 32 || bpp == 24)) {
        fb_mirror = (uint8_t *)addr;
        fb_mirror_pitch = pitch;
        fb_mirror_h = height;
    }
}

int fb_mirror_active(void) {
    return fb_mirror != NULL;
}

void fb_set_hhdm(uint64_t hhdm_offset) {
#ifndef PRISM_FB_NO_SHADOW
    fb_hhdm = hhdm_offset;
#else
    (void)hhdm_offset;
#endif
}

static uint8_t *fb_base(void) {
#ifndef PRISM_FB_NO_SHADOW
    return fb_shadow != NULL ? fb_shadow : fb_addr;
#else
    return fb_addr;
#endif
}

void fb_flush(void) {
#ifndef PRISM_FB_NO_SHADOW
    if (fb_shadow == NULL || !fb_dirty || !fb_ok) {
        return;
    }
    memcpy(fb_addr, fb_shadow, (size_t)(fb_pitch * fb_h));
    if (fb_mirror != NULL) {
        uint64_t rows = fb_h < fb_mirror_h ? fb_h : fb_mirror_h;
        uint64_t row_bytes =
            fb_pitch < fb_mirror_pitch ? fb_pitch : fb_mirror_pitch;
        for (uint64_t y = 0; y < rows; y++) {
            memcpy(fb_mirror + y * fb_mirror_pitch,
                   fb_shadow + y * fb_pitch, (size_t)row_bytes);
        }
    }
    fb_dirty = 0;
#endif
}


void fb_init(void *addr, uint64_t width, uint64_t height, uint64_t pitch,
             uint16_t bpp, uint8_t red_size, uint8_t red_shift,
             uint8_t green_size, uint8_t green_shift,
             uint8_t blue_size, uint8_t blue_shift) {
    fb_addr = (uint8_t *)addr;
    fb_w = width;
    fb_h = height;
    fb_pitch = pitch;
    fb_bpp = bpp;
    fb_rsize = red_size;
    fb_rshift = red_shift;
    fb_gsize = green_size;
    fb_gshift = green_shift;
    fb_bsize = blue_size;
    fb_bshift = blue_shift;
    /* Only 32-bit and 24-bit colour are supported. Anything else
     * leaves the driver off, so callers can fall back to serial. */
    fb_ok = (addr != NULL) && (width > 0) && (height > 0) &&
            (bpp == 32 || bpp == 24);
    /* シャドウバッファを確保する (解像度変更で再初期化
     * されたときは、足りなければ取り直す)。 */
#ifdef PRISM_FB_NO_SHADOW
    /* ホストテストでは PMM が無いので直接描画する。 */
#else
    if (fb_ok && fb_hhdm != 0) {
        uint64_t need = (pitch * height + 4095) / 4096;
        if (need > fb_shadow_pages) {
            uint64_t phys = pmm_alloc_pages((size_t)need);
            if (phys != 0) {
                fb_shadow = (uint8_t *)(uintptr_t)(phys + fb_hhdm);
                fb_shadow_pages = need;
                memset(fb_shadow, 0, (size_t)(pitch * height));
                fb_dirty = 1;
            }
        }
    }
#endif
}

int fb_ready(void) {
    return fb_ok;
}

uint64_t fb_width(void) {
    return fb_w;
}

uint64_t fb_height(void) {
    return fb_h;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t pixel = 0;
    if (fb_rsize > 0 && fb_rsize <= 8) {
        pixel |= ((uint32_t)(r >> (8 - fb_rsize))) << fb_rshift;
    }
    if (fb_gsize > 0 && fb_gsize <= 8) {
        pixel |= ((uint32_t)(g >> (8 - fb_gsize))) << fb_gshift;
    }
    if (fb_bsize > 0 && fb_bsize <= 8) {
        pixel |= ((uint32_t)(b >> (8 - fb_bsize))) << fb_bshift;
    }
    return pixel;
}

void fb_put_pixel(uint64_t x, uint64_t y, uint32_t colour) {
    if (!fb_ok || x >= fb_w || y >= fb_h) {
        return;
    }
    uint64_t bytes = fb_bpp / 8;
    uint8_t *p = fb_base() + y * fb_pitch + x * bytes;
    fb_dirty = 1;
    if (fb_bpp == 32) {
        p[0] = (uint8_t)(colour & 0xFF);
        p[1] = (uint8_t)((colour >> 8) & 0xFF);
        p[2] = (uint8_t)((colour >> 16) & 0xFF);
        p[3] = (uint8_t)((colour >> 24) & 0xFF);
    } else {
        p[0] = (uint8_t)(colour & 0xFF);
        p[1] = (uint8_t)((colour >> 8) & 0xFF);
        p[2] = (uint8_t)((colour >> 16) & 0xFF);
    }
}

/* size ビットのチャンネル値を 8 ビットへ広げる
 * (上位ビットの複製で埋める)。 */
static uint32_t fb_expand(uint32_t v, uint8_t size) {
    if (size == 0 || size > 8) {
        return 0;
    }
    if (size == 8) {
        return v;
    }
    uint32_t out = v << (8 - size);
    if (2 * size - 8 > 0) {
        out |= v >> (2 * size - 8);
    } else {
        out |= v;
    }
    return out & 0xFF;
}

uint32_t fb_get_pixel_rgb(uint64_t x, uint64_t y) {
    if (!fb_ok || x >= fb_w || y >= fb_h) {
        return 0;
    }
    uint64_t bytes = fb_bpp / 8;
    const uint8_t *p = fb_base() + y * fb_pitch + x * bytes;
    uint32_t packed = 0;
    for (uint64_t i = 0; i < bytes && i < 4; i++) {
        packed |= (uint32_t)p[i] << (8 * i);
    }
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    if (fb_rsize > 0 && fb_rsize <= 8) {
        r = fb_expand((packed >> fb_rshift) &
                          ((1u << fb_rsize) - 1u),
                      fb_rsize);
    }
    if (fb_gsize > 0 && fb_gsize <= 8) {
        g = fb_expand((packed >> fb_gshift) &
                          ((1u << fb_gsize) - 1u),
                      fb_gsize);
    }
    if (fb_bsize > 0 && fb_bsize <= 8) {
        b = fb_expand((packed >> fb_bshift) &
                          ((1u << fb_bsize) - 1u),
                      fb_bsize);
    }
    return (r << 16) | (g << 8) | b;
}

void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                  uint32_t colour) {
    if (!fb_ok) {
        return;
    }
    if (x >= fb_w || y >= fb_h) {
        return;
    }
    if (x + w > fb_w) {
        w = fb_w - x;
    }
    if (y + h > fb_h) {
        h = fb_h - y;
    }
    for (uint64_t row = 0; row < h; row++) {
        for (uint64_t col = 0; col < w; col++) {
            fb_put_pixel(x + col, y + row, colour);
        }
    }
}

void fb_clear(uint32_t colour) {
    fb_fill_rect(0, 0, fb_w, fb_h, colour);
}

void fb_draw_line(int64_t x0, int64_t y0, int64_t x1, int64_t y1,
                  uint32_t colour) {
    /* Bresenham's line algorithm. */
    int64_t dx = x1 - x0;
    if (dx < 0) {
        dx = -dx;
    }
    int64_t sx = (x0 < x1) ? 1 : -1;
    int64_t dy = y1 - y0;
    if (dy < 0) {
        dy = -dy;
    }
    int64_t sy = (y0 < y1) ? 1 : -1;
    int64_t err = dx - dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0) {
            fb_put_pixel((uint64_t)x0, (uint64_t)y0, colour);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int64_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void fb_draw_char(uint64_t x, uint64_t y, char c, uint32_t fg,
                  uint32_t bg, int scale) {
    if (!fb_ok || scale < 1) {
        return;
    }
    unsigned char ch = (unsigned char)c;
    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    const uint8_t *glyph = PRISM_FONT[ch - 32];
    for (int row = 0; row < PRISM_FONT_HEIGHT; row++) {
        for (int col = 0; col < PRISM_FONT_WIDTH; col++) {
            int on = (glyph[row] >> (PRISM_FONT_WIDTH - 1 - col)) & 1;
            uint32_t colour = on ? fg : bg;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    fb_put_pixel(x + (uint64_t)(col * scale + dx),
                                 y + (uint64_t)(row * scale + dy), colour);
                }
            }
        }
    }
}

void fb_draw_wide_char(uint64_t x, uint64_t y, uint32_t cp, uint32_t fg,
                       uint32_t bg, int scale) {
    if (!fb_ok || scale < 1) {
        return;
    }
    const uint8_t *glyph = jfont_glyph(cp);
    if (glyph == NULL) {
        fb_draw_char(x, y, '?', fg, bg, scale);
        return;
    }
    for (int row = 0; row < PRISM_JFONT_ROWS; row++) {
        uint16_t bits = (uint16_t)((glyph[row * 2] << 8) | glyph[row * 2 + 1]);
        for (int col = 0; col < PRISM_JFONT_COLS; col++) {
            int on = (bits >> (PRISM_JFONT_COLS - 1 - col)) & 1;
            uint32_t colour = on ? fg : bg;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    fb_put_pixel(x + (uint64_t)(col * scale + dx),
                                 y + (uint64_t)(row * scale + dy), colour);
                }
            }
        }
    }
}

uint64_t fb_draw_text(uint64_t x, uint64_t y, const char *s, uint32_t fg,
                      uint32_t bg, int scale) {
    uint64_t start = x;
    if (s == NULL) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    size_t remain = 0;
    while (s[remain] != '\0') {
        remain++;
    }
    while (remain > 0) {
        uint32_t cp = 0;
        int n = jfont_utf8_decode(s, remain, &cp);
        if (n <= 0) {
            break;
        }
        if (cp < 0x80) {
            fb_draw_char(x, y, (char)cp, fg, bg, scale);
            x += (uint64_t)((PRISM_FONT_WIDTH + 1) * scale);
        } else {
            fb_draw_wide_char(x, y, cp, fg, bg, scale);
            x += (uint64_t)((PRISM_JFONT_COLS + 2) * scale);
        }
        s += n;
        remain -= (size_t)n;
    }
    return x - start;
}

void fb_draw_image_rgb24_rect(uint64_t x, uint64_t y, uint64_t w,
                              uint64_t h, const uint8_t *src,
                              int src_w, int src_h, int src_stride,
                              int top_down, int rx, int ry, int rw,
                              int rh) {
    if (!fb_ok || src == NULL || src_w <= 0 || src_h <= 0 ||
        src_stride < src_w * 3 || rw <= 0 || rh <= 0) {
        return;
    }
    if (x >= fb_w || y >= fb_h) {
        return;
    }
    if (x + w > fb_w) {
        w = fb_w - x;
    }
    if (y + h > fb_h) {
        h = fb_h - y;
    }
    uint64_t bytes = fb_bpp / 8;
    for (uint64_t row = 0; row < h; row++) {
        int sy = ry + (int)(row * (uint64_t)rh / h);
        if (!top_down) {
            sy = src_h - 1 - sy;
        }
        const uint8_t *srow = src + (size_t)sy * (size_t)src_stride;
        uint8_t *drow = fb_base() + (y + row) * fb_pitch + x * bytes;
        fb_dirty = 1;
        for (uint64_t col = 0; col < w; col++) {
            int sx = rx + (int)(col * (uint64_t)rw / w);
            const uint8_t *sp = srow + (size_t)sx * 3;
            /* src は BGR の順。fb_rgb は 0xRRGGBB を取る。 */
            uint32_t pix = fb_rgb(sp[2], sp[1], sp[0]);
            if (fb_bpp == 32) {
                drow[0] = (uint8_t)(pix & 0xFF);
                drow[1] = (uint8_t)((pix >> 8) & 0xFF);
                drow[2] = (uint8_t)((pix >> 16) & 0xFF);
                drow[3] = (uint8_t)((pix >> 24) & 0xFF);
            } else {
                drow[0] = (uint8_t)(pix & 0xFF);
                drow[1] = (uint8_t)((pix >> 8) & 0xFF);
                drow[2] = (uint8_t)((pix >> 16) & 0xFF);
            }
            drow += bytes;
        }
    }
}

void fb_draw_image_rgb24(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                         const uint8_t *src, int src_w, int src_h,
                         int src_stride, int top_down) {
    fb_draw_image_rgb24_rect(x, y, w, h, src, src_w, src_h,
                             src_stride, top_down, 0, 0, src_w,
                             src_h);
}

/* フェード付き描画: fade = 0 で白 (背景)、256 で原画。
 * 起動ロゴのフェードイン用。整数演算のみ。 */
void fb_draw_image_rgb24_fade(uint64_t x, uint64_t y, uint64_t w,
                              uint64_t h, const uint8_t *src,
                              int src_w, int src_h, int src_stride,
                              int top_down, int fade) {
    if (!fb_ok || src == NULL || src_w <= 0 || src_h <= 0 ||
        src_stride < src_w * 3) {
        return;
    }
    if (x >= fb_w || y >= fb_h) {
        return;
    }
    if (x + w > fb_w) {
        w = fb_w - x;
    }
    if (y + h > fb_h) {
        h = fb_h - y;
    }
    if (fade < 0) {
        fade = 0;
    }
    if (fade > 256) {
        fade = 256;
    }
    uint64_t bytes = fb_bpp / 8;
    for (uint64_t row = 0; row < h; row++) {
        int sy = (int)(row * (uint64_t)src_h / h);
        if (!top_down) {
            sy = src_h - 1 - sy;
        }
        const uint8_t *srow = src + (size_t)sy * (size_t)src_stride;
        uint8_t *drow = fb_base() + (y + row) * fb_pitch + x * bytes;
        fb_dirty = 1;
        for (uint64_t col = 0; col < w; col++) {
            int sx = (int)(col * (uint64_t)src_w / w);
            const uint8_t *sp = srow + (size_t)sx * 3;
            uint32_t r = 255 - (uint32_t)(255 - sp[2]) *
                                   (uint32_t)fade / 256;
            uint32_t g = 255 - (uint32_t)(255 - sp[1]) *
                                   (uint32_t)fade / 256;
            uint32_t b = 255 - (uint32_t)(255 - sp[0]) *
                                   (uint32_t)fade / 256;
            uint32_t pix = fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
            if (fb_bpp == 32) {
                drow[0] = (uint8_t)(pix & 0xFF);
                drow[1] = (uint8_t)((pix >> 8) & 0xFF);
                drow[2] = (uint8_t)((pix >> 16) & 0xFF);
                drow[3] = 0;
            } else if (fb_bpp == 24) {
                drow[0] = (uint8_t)(pix & 0xFF);
                drow[1] = (uint8_t)((pix >> 8) & 0xFF);
                drow[2] = (uint8_t)((pix >> 16) & 0xFF);
            }
            drow += bytes;
        }
    }
}

void fb_draw_image_rgb565(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                          const uint16_t *src, int src_w, int src_h) {
    if (!fb_ok || src == NULL || src_w <= 0 || src_h <= 0) {
        return;
    }
    if (x >= fb_w || y >= fb_h) {
        return;
    }
    if (x + w > fb_w) {
        w = fb_w - x;
    }
    if (y + h > fb_h) {
        h = fb_h - y;
    }
    uint64_t bytes = fb_bpp / 8;
    for (uint64_t row = 0; row < h; row++) {
        int sy = (int)(row * (uint64_t)src_h / h);
        const uint16_t *srow = src + (size_t)sy * (size_t)src_w;
        uint8_t *drow = fb_base() + (y + row) * fb_pitch + x * bytes;
        fb_dirty = 1;
        for (uint64_t col = 0; col < w; col++) {
            int sx = (int)(col * (uint64_t)src_w / w);
            uint16_t v = srow[sx];
            /* RGB565 を 8 ビットずつに広げる。 */
            uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
            uint8_t g6 = (uint8_t)((v >> 5) & 0x3F);
            uint8_t b5 = (uint8_t)(v & 0x1F);
            uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
            uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
            uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
            uint32_t pix = fb_rgb(r, g, b);
            if (fb_bpp == 32) {
                drow[0] = (uint8_t)(pix & 0xFF);
                drow[1] = (uint8_t)((pix >> 8) & 0xFF);
                drow[2] = (uint8_t)((pix >> 16) & 0xFF);
                drow[3] = (uint8_t)((pix >> 24) & 0xFF);
            } else {
                drow[0] = (uint8_t)(pix & 0xFF);
                drow[1] = (uint8_t)((pix >> 8) & 0xFF);
                drow[2] = (uint8_t)((pix >> 16) & 0xFF);
            }
            drow += bytes;
        }
    }
}
