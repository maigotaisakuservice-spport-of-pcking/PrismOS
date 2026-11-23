/* PrismOS — virtio 入力 (タブレット) ドライバ、x86 用。
 *
 * モダン virtio PCI (1AF4:1052) の virtio-tablet から
 * 絶対座標イベント (EV_ABS) とボタン (EV_KEY) を受け、
 * デスクトップのマウス位置へ橋渡しする。QEMU の入力
 * デバイスは従来型 (レガシー) を持たないため、PCI
 * ケーパビリティ経由のモダン輸送 (共通設定/通知/機器
 * 設定) を実装する。イベントキュー (キュー 0) に
 * 8 バイトのバッファを張り、使われた分をポーリングで
 * 回収する。ステータスキュー (キュー 1) は使わない。
 */
#ifndef PRISM_VINPUT_H
#define PRISM_VINPUT_H

#include <stdint.h>

/* 見つかれば初期化して 1、無ければ 0 を返す。 */
int vinput_init(uint64_t hhdm_offset);
int vinput_present(void);

/* たまったイベントを取り出す。絶対座標 (画面ピクセルへ
 * 換算済み) かボタンに変化があれば 1 を返し、x/y/buttons
 * を入れる。width/height は画面の大きさ。 */
int vinput_poll(int *x, int *y, int *buttons, int width,
                int height);

/* 現在の生状態 (テスト表示用)。 */
void vinput_state(int *raw_x, int *raw_y, int *buttons,
                  int *max_x, int *max_y);

#endif
