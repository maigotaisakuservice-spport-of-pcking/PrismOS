/* PrismOS — PNG デコーダ (shell/png.c) のホストテスト。
 *
 * フィクスチャは scripts/gen-png-fixtures.py が生成した
 * tests/png_fixtures.h。既知のピクセルを持つ画像を読み、
 * BGR 出力・フィルタ復元・α合成・パレット・16bit・
 * 縮小・エラー処理を確認する。
 */
#include <stdio.h>
#include <string.h>

#include "png.h"
#include "png_fixtures.h"

static int checks;
static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            failures++;                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                              \
    } while (0)

static uint8_t out[4096];

/* out のピクセル (x,y) が (r,g,b) か (out は BGR 順)。 */
static int px_is(int w, int x, int y, int r, int g, int b) {
    const uint8_t *p = out + ((size_t)y * (size_t)w + (size_t)x) * 3;
    return p[0] == (uint8_t)b && p[1] == (uint8_t)g &&
           p[2] == (uint8_t)r;
}

int main(void) {
    int w = 0, h = 0;

    /* 署名判定。 */
    CHECK(png_is_png(png_rgb8, png_rgb8_len) == 1);
    CHECK(png_is_png((const uint8_t *)"BMxxxxxx", 8) == 0);
    CHECK(png_decode((const uint8_t *)"not a png", 9, out,
                     sizeof(out), 480, 240, &w, &h) == -1);

    /* RGB 8bit (フィルタ 0 と Sub)。 */
    CHECK(png_decode(png_rgb8, png_rgb8_len, out, sizeof(out),
                     480, 240, &w, &h) == 0);
    CHECK(w == 4 && h == 2);
    CHECK(px_is(4, 0, 0, 255, 0, 0));
    CHECK(px_is(4, 1, 0, 0, 255, 0));
    CHECK(px_is(4, 2, 0, 0, 0, 255));
    CHECK(px_is(4, 3, 0, 255, 255, 0));
    CHECK(px_is(4, 0, 1, 0, 255, 255));
    CHECK(px_is(4, 1, 1, 255, 0, 255));
    CHECK(px_is(4, 2, 1, 128, 128, 128));
    CHECK(px_is(4, 3, 1, 0, 0, 0));

    /* RGBA 8bit (フィルタ Up と Paeth、αは白へ合成)。 */
    CHECK(png_decode(png_rgba8, png_rgba8_len, out, sizeof(out),
                     480, 240, &w, &h) == 0);
    CHECK(w == 2 && h == 2);
    CHECK(px_is(2, 0, 0, 255, 0, 0));
    CHECK(px_is(2, 1, 0, 127, 255, 127));
    CHECK(px_is(2, 0, 1, 255, 255, 255));
    CHECK(px_is(2, 1, 1, 10, 20, 30));

    /* パレット 8bit (フィルタ Average も使用)。 */
    CHECK(png_decode(png_pal8, png_pal8_len, out, sizeof(out),
                     480, 240, &w, &h) == 0);
    CHECK(px_is(2, 0, 0, 255, 0, 0));
    CHECK(px_is(2, 1, 0, 0, 255, 0));
    CHECK(px_is(2, 0, 1, 0, 0, 255));
    CHECK(px_is(2, 1, 1, 255, 255, 255));

    /* グレースケール 8bit。 */
    CHECK(png_decode(png_gray8, png_gray8_len, out, sizeof(out),
                     480, 240, &w, &h) == 0);
    CHECK(w == 3 && h == 1);
    CHECK(px_is(3, 0, 0, 0, 0, 0));
    CHECK(px_is(3, 1, 0, 127, 127, 127));
    CHECK(px_is(3, 2, 0, 255, 255, 255));

    /* RGB 16bit (上位バイトを取る)。 */
    CHECK(png_decode(png_rgb16, png_rgb16_len, out, sizeof(out),
                     480, 240, &w, &h) == 0);
    CHECK(px_is(2, 0, 0, 0x12, 0x56, 0x9A));
    CHECK(px_is(2, 1, 0, 0xDE, 0x00, 0x22));

    /* パレット 2bit (ビット詰めの展開)。 */
    CHECK(png_decode(png_pal2, png_pal2_len, out, sizeof(out),
                     480, 240, &w, &h) == 0);
    CHECK(w == 4 && h == 1);
    CHECK(px_is(4, 0, 0, 255, 0, 0));
    CHECK(px_is(4, 1, 0, 0, 255, 0));
    CHECK(px_is(4, 2, 0, 0, 0, 255));
    CHECK(px_is(4, 3, 0, 255, 255, 255));

    /* 縮小: 8x8 を最大 4x4 にすると最近傍で間引かれる。 */
    CHECK(png_decode(png_big8, png_big8_len, out, sizeof(out),
                     4, 4, &w, &h) == 0);
    CHECK(w == 4 && h == 4);
    CHECK(px_is(4, 0, 0, 0, 0, 200));
    CHECK(px_is(4, 1, 0, 64, 0, 200));
    CHECK(px_is(4, 3, 3, 192, 192, 200));

    /* インターレースは -2 (未対応を正直に返す)。 */
    {
        uint8_t buf[256];
        memcpy(buf, png_gray8, png_gray8_len);
        buf[28] = 1; /* IHDR のインターレース方式 */
        CHECK(png_decode(buf, png_gray8_len, out, sizeof(out),
                         480, 240, &w, &h) == -2);
    }

    /* IDAT の途中で切れたデータは -1 (IEND だけの欠落は
     * 画像データが揃っているので許容する)。 */
    CHECK(png_decode(png_rgb8, png_rgb8_len / 2, out,
                     sizeof(out), 480, 240, &w, &h) == -1);

    /* 出力先が小さすぎれば -3。 */
    CHECK(png_decode(png_big8, png_big8_len, out, 100, 480, 240,
                     &w, &h) == -3);

    printf("test_png: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
