/* PrismOS — PNG デコーダ (純粋ロジック)。
 *
 * PNG (RFC 2083) を読み、BGR 3 バイト/ピクセルの画像にする。
 * 圧縮 (IDAT の zlib ストリーム) は既存の inflate を使う。
 * フィルタ (None/Sub/Up/Average/Paeth) の復元もここで行う。
 *
 * 対応範囲 (正直な範囲):
 *   - 色タイプ 0 (グレースケール), 2 (RGB), 3 (パレット),
 *     4 (グレースケール+α), 6 (RGBA)
 *   - ビット深度: タイプ 3 は 1/2/4/8、その他は 8 と 16
 *     (16 は上位バイトを取る)
 *   - α は白地に合成する (ブラウザの表示先が不透明なため)
 *   - インターレース (Adam7) は未対応で -2 を返す
 *   - 展開後の生データが内部バッファ (1MB) を超える画像は
 *     -3 を返す。入力全体も呼び出し側の上限に従う。
 *
 * 出力は max_w x max_h に収まるよう最近傍で縮小される
 * (縮小不要なら等倍)。out は BGR、行は詰めて並ぶ
 * (stride = 幅 x 3、top_down 形式)。
 */
#ifndef PRISM_PNG_H
#define PRISM_PNG_H

#include <stddef.h>
#include <stdint.h>

/* PNG かどうか (先頭の署名だけ見る)。 */
int png_is_png(const uint8_t *data, size_t len);

/* デコードする。成功したら 0、*w_out / *h_out に出力の
 * 大きさが入る。out は dw*dh*3 バイト必要 (dw/dh は
 * max_w/max_h で頭打ち)。負数: -1 壊れている/非対応の
 * 形式、-2 インターレース、-3 大きすぎる。 */
int png_decode(const uint8_t *data, size_t len, uint8_t *out,
               size_t out_cap, int max_w, int max_h, int *w_out,
               int *h_out);

#endif /* PRISM_PNG_H */
