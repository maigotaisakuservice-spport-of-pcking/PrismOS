/* PrismOS — 壁紙画像 (BMP) の読み込みと描画
 *
 * 埋め込みの BMP (24 ビット、無圧縮) を解析し、フレームバッファ
 * に合わせて描く。解析と描画は純粋な処理 (fb 経由の描画を除く)
 * なので、ホストテストで直接確かめられる。
 */
#ifndef PRISM_WALLPAPER_H
#define PRISM_WALLPAPER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *pixels; /* BGR、1 ピクセル 3 バイト */
    int width;
    int height;
    int stride; /* 1 行のバイト数 (4 バイト単位に詰めたもの) */
    int top_down; /* 1 = 上から下、0 = 下から上 (BMP 既定) */
} wallpaper_img;

/* BMP データを解析する。成功したら 0 を返し、out に情報を入れる。
 * pixels は data の中を指す (コピーしない)。 */
int wallpaper_parse(const uint8_t *data, size_t len, wallpaper_img *out);

/* 画像を画面いっぱいに描く (画面と画像の大きさが違えば拡大縮小
 * する)。画像が無効なら何もしない。 */
void wallpaper_draw(const wallpaper_img *img);

#endif /* PRISM_WALLPAPER_H */
