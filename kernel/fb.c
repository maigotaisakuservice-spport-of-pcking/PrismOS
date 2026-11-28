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

/* 描画クリップ矩形 (ウィンドウ外への描き出し防止)。
 * fb_set_clip で有効化し、fb_clear_clip で解除する。
 * 解除状態では画面全体が描画範囲。 */
static int fb_clip_on;
static uint64_t fb_clip_x0;
static uint64_t fb_clip_y0;
static uint64_t fb_clip_x1;
static uint64_t fb_clip_y1;

void fb_set_clip(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    fb_clip_on = 1;
    fb_clip_x0 = x;
    fb_clip_y0 = y;
    fb_clip_x1 = x + w;
    fb_clip_y1 = y + h;
}

void fb_clear_clip(void) {
    fb_clip_on = 0;
}

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

/* present フック (無ければ NULL)。 */
static void (*fb_present_hook)(void);

void fb_set_present_hook(void (*hook)(void)) {
    fb_present_hook = hook;
}

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
    if (fb_present_hook != NULL) {
        fb_present_hook();
    }
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
    if (fb_clip_on && (x < fb_clip_x0 || x >= fb_clip_x1 ||
                       y < fb_clip_y0 || y >= fb_clip_y1)) {
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

uint32_t fb_unpack_rgb(uint32_t packed) {
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
    return fb_unpack_rgb(packed);
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

/* ---- 角丸矩形と影 (UI 基盤) ----
 * ウィンドウやカードの角丸 (半径 8 / 4) と、面の輪郭線、
 * 柔らかい影を描くための共通部品。すべて行スパン単位で
 * 処理し、クリップ矩形と画面端を尊重する。 */

static uint64_t fb_isqrt(uint64_t v) {
    uint64_t r = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* 角丸矩形 (x, y, w, h, r) の行 ry (0..h-1) の水平スパン
 * [sx, ex) を返す。r = 0 なら通常の矩形。 */
static void fb_rrect_span(int64_t x, int64_t w, int64_t h, int64_t r,
                          int64_t ry, int64_t *sx, int64_t *ex) {
    int64_t inset = 0;
    if (r > 0 && h >= 2 * r) {
        int64_t dy = -1;
        if (ry < r) {
            dy = r - 1 - ry;
        } else if (ry >= h - r) {
            dy = ry - (h - r);
        }
        if (dy >= 0) {
            inset = r - (int64_t)fb_isqrt(
                            (uint64_t)(r * r - dy * dy));
        }
    }
    *sx = x + inset;
    *ex = x + w - inset;
}

void fb_fill_round_rect(int64_t x, int64_t y, int64_t w, int64_t h,
                        int64_t r, uint32_t colour) {
    if (!fb_ok || w <= 0 || h <= 0) {
        return;
    }
    if (r < 0) {
        r = 0;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    for (int64_t ry = 0; ry < h; ry++) {
        int64_t py = y + ry;
        if (py < 0 || py >= (int64_t)fb_h) {
            continue;
        }
        int64_t sx, ex;
        fb_rrect_span(x, w, h, r, ry, &sx, &ex);
        if (ex <= sx) {
            continue;
        }
        if (sx < 0) {
            sx = 0;
        }
        if (ex > (int64_t)fb_w) {
            ex = (int64_t)fb_w;
        }
        if (ex > sx) {
            fb_fill_rect((uint64_t)sx, (uint64_t)py,
                         (uint64_t)(ex - sx), 1, colour);
        }
    }
}

/* 矩形 (x, y, w, h) を角丸矩形 (cx, cy, cw, ch, cr) の
 * 内側だけで塗る。タイトルバーや右上のボタンなど、親の
 * 角丸に沿わせたい部分塗りに使う。 */
void fb_fill_clipped_rrect(int64_t x, int64_t y, int64_t w, int64_t h,
                           int64_t cx, int64_t cy, int64_t cw,
                           int64_t ch, int64_t cr, uint32_t colour) {
    if (!fb_ok || w <= 0 || h <= 0 || cw <= 0 || ch <= 0) {
        return;
    }
    if (cr < 0) {
        cr = 0;
    }
    if (cr > cw / 2) {
        cr = cw / 2;
    }
    if (cr > ch / 2) {
        cr = ch / 2;
    }
    for (int64_t ry = 0; ry < h; ry++) {
        int64_t py = y + ry;
        if (py < 0 || py >= (int64_t)fb_h) {
            continue;
        }
        int64_t iy = py - cy;
        if (iy < 0 || iy >= ch) {
            continue;
        }
        int64_t csx, cex;
        fb_rrect_span(cx, cw, ch, cr, iy, &csx, &cex);
        int64_t sx = x > csx ? x : csx;
        int64_t ex = x + w < cex ? x + w : cex;
        if (sx < 0) {
            sx = 0;
        }
        if (ex > (int64_t)fb_w) {
            ex = (int64_t)fb_w;
        }
        if (ex > sx) {
            fb_fill_rect((uint64_t)sx, (uint64_t)py,
                         (uint64_t)(ex - sx), 1, colour);
        }
    }
}

void fb_stroke_round_rect(int64_t x, int64_t y, int64_t w, int64_t h,
                          int64_t r, uint32_t colour) {
    if (!fb_ok || w <= 0 || h <= 0) {
        return;
    }
    if (r < 0) {
        r = 0;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    for (int64_t ry = 0; ry < h; ry++) {
        int64_t py = y + ry;
        if (py < 0 || py >= (int64_t)fb_h) {
            continue;
        }
        int64_t sx, ex;
        fb_rrect_span(x, w, h, r, ry, &sx, &ex);
        if (ex <= sx) {
            continue;
        }
        if (ry == 0 || ry == h - 1) {
            if (sx < 0) {
                sx = 0;
            }
            if (ex > (int64_t)fb_w) {
                ex = (int64_t)fb_w;
            }
            if (ex > sx) {
                fb_fill_rect((uint64_t)sx, (uint64_t)py,
                             (uint64_t)(ex - sx), 1, colour);
            }
        } else {
            if (sx >= 0 && sx < (int64_t)fb_w) {
                fb_put_pixel((uint64_t)sx, (uint64_t)py, colour);
            }
            if (ex - 1 >= 0 && ex - 1 < (int64_t)fb_w && ex - 1 != sx) {
                fb_put_pixel((uint64_t)(ex - 1), (uint64_t)py,
                             colour);
            }
        }
    }
}



/* 柔らかい影 (第2ラウンド): 角丸矩形への符号付き距離
 * (SDF) から、距離で 2 次減衰するアルファを画素ごとに
 * 求めて暗くする。帯状の近似と違い、角の周りも含めて
 * 連続的にぼける。本体の内側は後から本体色で塗られる
 * ので、影は拡張領域全体に描いてよい (角の切り欠き
 * 部分には正しく影が残る)。
 * 影は下地を暗くする方式のため、呼び出し側は全面再描画
 * のときだけ呼ぶこと (部分再描画では濃化する)。 */
#define FB_SHADOW_BLUR 14
#define FB_SHADOW_ALPHA 105

static void fb_darken_pixel(int64_t px, int64_t py, int alpha) {
    uint32_t c = fb_get_pixel_rgb((uint64_t)px, (uint64_t)py);
    uint32_t rr = ((c >> 16) & 0xFF) * (uint32_t)(255 - alpha) / 255;
    uint32_t gg = ((c >> 8) & 0xFF) * (uint32_t)(255 - alpha) / 255;
    uint32_t bb = (c & 0xFF) * (uint32_t)(255 - alpha) / 255;
    fb_put_pixel((uint64_t)px, (uint64_t)py,
                 fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bb));
}

void fb_shadow_round_rect(int64_t x, int64_t y, int64_t w, int64_t h,
                          int64_t r, int64_t dy) {
    if (!fb_ok || w <= 0 || h <= 0) {
        return;
    }
    if (r < 0) {
        r = 0;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    const int64_t B = FB_SHADOW_BLUR;
    int64_t cx = 2 * x + w; /* 中心の 2 倍 (端数回避) */
    int64_t cy = 2 * (y + dy) + h;
    int64_t hx = w - 2 * r; /* コア矩形の半幅の 2 倍 */
    int64_t hy = h - 2 * r;
    int64_t x0 = x - B;
    int64_t x1 = x + w + B;
    int64_t y0 = y + dy - B;
    int64_t y1 = y + dy + h + B;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (int64_t)fb_w) {
        x1 = (int64_t)fb_w;
    }
    if (y1 > (int64_t)fb_h) {
        y1 = (int64_t)fb_h;
    }
    for (int64_t py = y0; py < y1; py++) {
        int64_t qy2 = 2 * py - cy;
        if (qy2 < 0) {
            qy2 = -qy2;
        }
        int64_t qy = qy2 - hy; /* コアからの距離の 2 倍 */
        for (int64_t px = x0; px < x1; px++) {
            int64_t qx2 = 2 * px - cx;
            if (qx2 < 0) {
                qx2 = -qx2;
            }
            int64_t qx = qx2 - hx;
            int64_t sd2; /* 符号付き距離の 2 倍 */
            if (qx > 0 && qy > 0) {
                sd2 = (int64_t)fb_isqrt((uint64_t)(qx * qx +
                                                   qy * qy));
            } else {
                sd2 = qx > qy ? qx : qy;
            }
            sd2 -= 2 * r; /* コア矩形から角丸の境界へ */
            if (sd2 >= 2 * B || sd2 <= 0) {
                /* 内側は本体が不透明に塗るので影は不要
                 * (半透明パネルでも減衰して見えない)。
                 * 帯の外も影は無い。 */
                continue;
            }
            int64_t t = 2 * B - sd2; /* 2B..0 */
            int alpha = (int)(FB_SHADOW_ALPHA * t * t / (4 * B * B));
            if (alpha > 0) {
                fb_darken_pixel(px, py, alpha);
            }
        }
    }
}

/* 半透明の塗り (第2ラウンド): 既存の下地に colour を
 * alpha/255 で重ねる。パネルの素材感 (壁紙がほんのり
 * 透ける) 用。fb_put_pixel 経由なのでクリップに従う。 */
static void fb_blend_span(int64_t sx, int64_t ex, int64_t py,
                          uint32_t colour, int alpha) {
    if (py < 0 || py >= (int64_t)fb_h) {
        return;
    }
    if (sx < 0) {
        sx = 0;
    }
    if (ex > (int64_t)fb_w) {
        ex = (int64_t)fb_w;
    }
    uint32_t fr = (colour >> 16) & 0xFF;
    uint32_t fg = (colour >> 8) & 0xFF;
    uint32_t fb = colour & 0xFF;
    for (int64_t px = sx; px < ex; px++) {
        uint32_t c = fb_get_pixel_rgb((uint64_t)px, (uint64_t)py);
        uint32_t rr = (fr * (uint32_t)alpha +
                       ((c >> 16) & 0xFF) * (uint32_t)(255 - alpha)) /
                      255;
        uint32_t gg = (fg * (uint32_t)alpha +
                       ((c >> 8) & 0xFF) * (uint32_t)(255 - alpha)) /
                      255;
        uint32_t bb = (fb * (uint32_t)alpha +
                       (c & 0xFF) * (uint32_t)(255 - alpha)) / 255;
        fb_put_pixel((uint64_t)px, (uint64_t)py,
                     fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bb));
    }
}

void fb_fill_rect_alpha(uint64_t x, uint64_t y, uint64_t w,
                        uint64_t h, uint32_t colour, int alpha) {
    if (!fb_ok || alpha <= 0) {
        return;
    }
    if (alpha >= 255) {
        fb_fill_rect(x, y, w, h, colour);
        return;
    }
    uint32_t rgb24 = fb_unpack_rgb(colour);
    for (uint64_t row = 0; row < h; row++) {
        fb_blend_span((int64_t)x, (int64_t)(x + w),
                      (int64_t)(y + row), rgb24, alpha);
    }
}

void fb_fill_round_rect_alpha(int64_t x, int64_t y, int64_t w,
                              int64_t h, int64_t r, uint32_t colour,
                              int alpha) {
    if (!fb_ok || w <= 0 || h <= 0 || alpha <= 0) {
        return;
    }
    if (alpha >= 255) {
        fb_fill_round_rect(x, y, w, h, r, colour);
        return;
    }
    if (r < 0) {
        r = 0;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    uint32_t rgb24 = fb_unpack_rgb(colour);
    for (int64_t ry = 0; ry < h; ry++) {
        int64_t py = y + ry;
        if (py < 0 || py >= (int64_t)fb_h) {
            continue;
        }
        int64_t sx, ex;
        fb_rrect_span(x, w, h, r, ry, &sx, &ex);
        if (ex > sx) {
            fb_blend_span(sx, ex, py, rgb24, alpha);
        }
    }
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

/* UI テキスト描画 (デスクトップのレイアウト規約)。
 * 半角は scale 2*unit (送り 12*unit)、全角は scale unit
 * (送り 24*unit = 半角のちょうど2倍、グリフは 4*unit
 * 右に寄せて中央置き) で描く。半角セル幅 = 12*unit が
 * そのままグリッドの1セルになり、全角は2セル分になる
 * ため、セル単位で位置を計算するレイアウトと描画実幅が
 * 一致し、日本語と英字が混ざっても重ならない。 */
uint64_t fb_draw_text_ui(uint64_t x, uint64_t y, const char *s,
                         uint32_t fg, uint32_t bg, int unit) {
    uint64_t start = x;
    if (s == NULL) {
        return 0;
    }
    if (unit < 1) {
        unit = 1;
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
            fb_draw_char(x, y, (char)cp, fg, bg, 2 * unit);
            x += (uint64_t)(12 * unit);
        } else {
            fb_draw_wide_char(x + (uint64_t)(4 * unit), y, cp, fg,
                              bg, unit);
            x += (uint64_t)(24 * unit);
        }
        s += n;
        remain -= (size_t)n;
    }
    return x - start;
}

/* fb_draw_text_ui と同じ規約での文字列の描画幅 (px)。 */
uint64_t fb_text_ui_width(const char *s, int unit) {
    uint64_t w = 0;
    if (s == NULL) {
        return 0;
    }
    if (unit < 1) {
        unit = 1;
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
        w += (uint64_t)((cp < 0x80 ? 12 : 24) * unit);
        s += n;
        remain -= (size_t)n;
    }
    return w;
}

void fb_draw_image_rgb24_rect(uint64_t x, uint64_t y, uint64_t w,
                              uint64_t h, const uint8_t *src,
                              int src_w, int src_h, int src_stride,
                              int top_down, int rx, int ry, int rw,
                              int rh) {
    /* src_w * 3 は 64 ビットで計算する。int のままだと巨大な幅で
     * 巻き戻り、短すぎる stride が検査を通ってしまう
     * (ペンテスト F3)。 */
    if (!fb_ok || src == NULL || src_w <= 0 || src_h <= 0 ||
        (int64_t)src_stride < (int64_t)src_w * 3 || rw <= 0 ||
        rh <= 0) {
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
        (int64_t)src_stride < (int64_t)src_w * 3) {
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
