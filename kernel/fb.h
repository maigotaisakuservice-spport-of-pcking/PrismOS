/* PrismOS - framebuffer driver.
 *
 * The bootloader hands us a linear framebuffer (address, size,
 * pitch, colour masks). This driver draws pixels, rectangles,
 * lines and 5x7 bitmap text into it. It uses only general-purpose
 * registers (the Makefile forbids SSE in the kernel), and it never
 * assumes a fixed screen size: the bootloader reports the size.
 *
 * Colour is passed as 0xRRGGBB and packed with the masks the
 * bootloader reports, so 32-bit and 24-bit modes both work.
 * If the mode is anything else, fb_ready() is false and all
 * drawing calls do nothing.
 */
#ifndef PRISM_FB_H
#define PRISM_FB_H

#include <stddef.h>
#include <stdint.h>

/* Initialise from a bootloader framebuffer description.
 * addr must be a usable virtual address for the framebuffer. */
/* HHDM のずれを渡す (シャドウバッファ確保用)。
 * fb_init より前に呼ぶ。 */
void fb_set_hhdm(uint64_t hhdm_offset);

/* シャドウの内容を実フレームバッファへ反映する。 */
void fb_flush(void);

/* present フック。fb_flush が実フレームバッファへコピー
 * した直後に呼ばれる。バッキングメモリへ描く表示装置
 * (virtio-gpu 等) が、転送と反映をここで行う。NULL なら
 * 何もしない (従来の表示装置はこちら)。 */
void fb_set_present_hook(void (*hook)(void));

/* 第 2 ディスプレイ (ミラー先)。ブートローダが 2 つ目の
 * フレームバッファを報告したときに main が登録する。
 * fb_flush のたびに、重なる領域がそのままコピーされる
 * (拡張デスクトップではなくミラー)。bpp は主画面と
 * 同じ 32/24 のときだけ有効にする。 */
void fb_set_mirror(void *addr, uint64_t pitch, uint64_t height,
                   uint16_t bpp);
int fb_mirror_active(void);

void fb_init(void *addr, uint64_t width, uint64_t height, uint64_t pitch,
             uint16_t bpp, uint8_t red_size, uint8_t red_shift,
             uint8_t green_size, uint8_t green_shift,
             uint8_t blue_size, uint8_t blue_shift);
int fb_ready(void);
uint64_t fb_width(void);
uint64_t fb_height(void);

/* Pack 0xRRGGBB into the framebuffer pixel format. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Unpack a framebuffer-format pixel back to 0xRRGGBB
 * (the inverse of fb_rgb, using the reported masks). */
uint32_t fb_unpack_rgb(uint32_t packed);

void fb_put_pixel(uint64_t x, uint64_t y, uint32_t colour);

/* 描画済みピクセルを 0xRRGGBB で読み戻す (スクリーン
 * ショット用)。範囲外や未初期化なら 0。 */
uint32_t fb_get_pixel_rgb(uint64_t x, uint64_t y);
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                  uint32_t colour);
void fb_clear(uint32_t colour);
/* 角丸矩形の塗りと 1px 輪郭、柔らかい影 (UI 基盤)。
 * r は角の半径 (0 で通常の矩形)。影は本体を描く前に
 * 呼び、本体の外側の帯だけを暗くする。 */
void fb_fill_round_rect(int64_t x, int64_t y, int64_t w, int64_t h,
                        int64_t r, uint32_t colour);
void fb_stroke_round_rect(int64_t x, int64_t y, int64_t w, int64_t h,
                          int64_t r, uint32_t colour);
void fb_fill_clipped_rrect(int64_t x, int64_t y, int64_t w, int64_t h,
                           int64_t cx, int64_t cy, int64_t cw,
                           int64_t ch, int64_t cr, uint32_t colour);
void fb_shadow_round_rect(int64_t x, int64_t y, int64_t w, int64_t h,
                          int64_t r, int64_t dy);
/* 半透明の塗り (colour を alpha/255 で下地へ重ねる)。
 * パネルの素材感用。alpha >= 255 で通常の塗りになる。 */
void fb_fill_rect_alpha(uint64_t x, uint64_t y, uint64_t w,
                        uint64_t h, uint32_t colour, int alpha);
void fb_fill_round_rect_alpha(int64_t x, int64_t y, int64_t w,
                              int64_t h, int64_t r, uint32_t colour,
                              int alpha);
void fb_draw_line(int64_t x0, int64_t y0, int64_t x1, int64_t y1,
                  uint32_t colour);

/* Draw one character from the built-in 5x7 font. scale 1 = 5x7
 * pixels, scale 2 = 10x14. The cell is 6*scale wide and
 * 8*scale tall (one pixel of spacing on each side). */
void fb_draw_char(uint64_t x, uint64_t y, char c, uint32_t fg,
                  uint32_t bg, int scale);
/* Draw one full-width character (kana/kanji) from the
 * 16x16 Japanese font (jfont.h). The box is 16*scale wide
 * and 16*scale tall. Codepoints without a glyph draw '?'. */
void fb_draw_wide_char(uint64_t x, uint64_t y, uint32_t cp, uint32_t fg,
                       uint32_t bg, int scale);
/* Draw a NUL-terminated UTF-8 string: ASCII uses the 5x7
 * font, other codepoints use the Japanese font. Returns
 * the width in pixels. */
uint64_t fb_draw_text(uint64_t x, uint64_t y, const char *s, uint32_t fg,
                      uint32_t bg, int scale);
/* 描画クリップ矩形 (ウィンドウ外への描き出し防止)。
 * 設定中は fb_put_pixel が矩形の外側を描かない。 */
void fb_set_clip(uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void fb_clear_clip(void);
/* UI テキスト: 半角セル 12*unit px・全角はその2倍の送りで
 * 描く、デスクトップのレイアウト規約に一致する描画。 */
uint64_t fb_draw_text_ui(uint64_t x, uint64_t y, const char *s,
                         uint32_t fg, uint32_t bg, int unit);
uint64_t fb_text_ui_width(const char *s, int unit);

/* Draw a 24-bit RGB image (BGR bytes, rows padded to 4 bytes)
 * into a rectangle, scaling it to fit. top_down = 1 means the
 * first row is the top of the image (BMP top-down); 0 means the
 * first row is the bottom (BMP default). */
/* 矩形領域だけを拡大して描く (ズーム表示用)。 */
void fb_draw_image_rgb24_rect(uint64_t x, uint64_t y, uint64_t w,
                              uint64_t h, const uint8_t *src,
                              int src_w, int src_h, int src_stride,
                              int top_down, int rx, int ry, int rw,
                              int rh);

void fb_draw_image_rgb24(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                         const uint8_t *src, int src_w, int src_h,
                         int src_stride, int top_down);

void fb_draw_image_rgb24_fade(uint64_t x, uint64_t y, uint64_t w,
                              uint64_t h, const uint8_t *src,
                              int src_w, int src_h, int src_stride,
                              int top_down, int fade);

/* Draw a 16-bit RGB565 image (little-endian pixels, no padding)
 * into a rectangle, scaling it to fit. The first row is the top
 * of the image. Used by the video player (PV1 frames). */
void fb_draw_image_rgb565(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                          const uint16_t *src, int src_w, int src_h);

#endif /* PRISM_FB_H */
