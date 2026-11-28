/* PrismOS — 壁紙画像 (24 ビット BMP) の読み込みと描画 */
#include "wallpaper.h"

#include "fb.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int wallpaper_parse(const uint8_t *data, size_t len, wallpaper_img *out) {
    if (data == NULL || out == NULL || len < 54) {
        return -1;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        return -1;
    }
    uint32_t offset = rd32(data + 10);
    uint32_t header_size = rd32(data + 14);
    if (header_size < 40) {
        return -1;
    }
    int32_t width = (int32_t)rd32(data + 18);
    int32_t height = (int32_t)rd32(data + 22);
    if (width <= 0 || height == 0) {
        return -1;
    }
    if (rd16(data + 26) != 1) { /* プレーン数 */
        return -1;
    }
    if (rd16(data + 28) != 24) { /* 24 ビットだけ */
        return -1;
    }
    if (rd32(data + 30) != 0) { /* 無圧縮だけ */
        return -1;
    }
    int top_down = 0;
    int64_t hgt = height;
    if (height < 0) {
        top_down = 1;
        hgt = -(int64_t)height; /* INT32_MIN でも溢れない */
    }
    /* 寸法の上限: 壁紙・ロゴ・写真表示という用途では 8192 px を
     * 超える BMP は扱わない (埋め込み壁紙は 1280x800)。stride を
     * int に収めるための前提でもある。 */
    if (width > 8192 || hgt > 8192) {
        return -1;
    }
    /* stride と必要長は 64 ビットで計算する。int のままだと巨大な
     * 幅で stride が巻き戻り (幅 0x55555556 → stride 4)、短い
     * ファイルを「収まる」と誤判定して受理してしまう
     * (ペンテスト F3)。 */
    uint64_t stride = ((uint64_t)(uint32_t)width * 3u + 3u) &
                      ~(uint64_t)3u;
    if ((uint64_t)offset > (uint64_t)len ||
        (uint64_t)offset + stride * (uint64_t)hgt > (uint64_t)len) {
        return -1;
    }
    out->pixels = data + offset;
    out->width = width;
    out->height = (int)hgt;
    out->stride = (int)stride;
    out->top_down = top_down;
    return 0;
}

void wallpaper_draw(const wallpaper_img *img) {
    if (img == NULL || img->pixels == NULL) {
        return;
    }
    fb_draw_image_rgb24(0, 0, fb_width(), fb_height(), img->pixels,
                        img->width, img->height, img->stride,
                        img->top_down);
}
