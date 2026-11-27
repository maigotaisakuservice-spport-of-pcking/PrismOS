/* PrismOS — PS/2 マウスドライバ
 *
 * 割り込み駆動 (IRQ12)。3 バイトのパケットを組み立て、移動量と
 * ボタン状態をためる。デスクトップが mouse_poll() で取り出して
 * カーソルを動かす。パケットの解析と位置のクランプは純粋関数
 * なので、ホストテストで直接確かめられる。
 *
 * 対応するのは標準の 3 バイトパケットだけ (ホイール無し)。
 * PS/2 の Y は上が正のため、画面座標では符号を反転させる
 * (desktop 側ではなく、ここで画面向きに直して渡す)。
 */
#ifndef PRISM_MOUSE_H
#define PRISM_MOUSE_H

#include <stdint.h>

/* PS/2 マウスを初期化する。キーボードの初期化の後に呼ぶこと
 * (設定バイトを読み直して IRQ12 を許可する)。割り込み許可
 * (sti) の前に呼ぶこと。見つからなければ何もしない。 */
void mouse_init(void);

/* マウスが見つかったかどうか。mouse_init() の後で有効。 */
int mouse_present(void);

/* IRQ12 のハンドラ (IDT 経由で呼ばれる)。0x60 を読んで
 * パケットを組み立て、移動量をためてから EOI を送る。 */
void mouse_irq_handler(void);

/* たまった移動量を取り出す。変化があれば 1 を返し、dx と dy に
 * 移動量 (画面向き: 右が +x、下が +y)、buttons に現在の
 * ボタン状態 (bit0 = 左、bit1 = 右、bit2 = 中) を入れる。
 * 変化が無ければ 0 を返す。 */
int mouse_poll(int *dx, int *dy, int *buttons);

/* USB HID など別の入力源から相対移動とボタン状態を
 * 合流させる (mouse_poll がまとめて返す)。 */
void mouse_inject(int dx, int dy, int buttons);

/* 画面の大きさを教える (クランプ用)。デスクトップが呼ぶ。 */
void mouse_set_bounds(int width, int height);
/* カーソルの初期位置を置く。デスクトップが呼ぶ。 */
void mouse_set_position(int x, int y);

/* 現在のカーソル位置を読む。 */
void mouse_position(int *x, int *y);

/* カーソル速度 (%、25-400)。mouse_poll が返す移動量に
 * 適用される。既定は 100。 */
void mouse_set_speed(int percent);

/* 3 バイトのパケットを解析する。成功したら 0 を返し、dx と dy
 * に移動量 (画面向き)、buttons にボタン状態を入れる。
 * 先頭バイトの bit3 が立っていなければ -1 (同期ずれ)。 */
int mouse_parse_packet(const uint8_t *pkt, int *dx, int *dy,
                       int *buttons);

/* 位置に移動量を足して画面内に収める。 */
void mouse_clamp_move(int *x, int *y, int dx, int dy, int width,
                      int height);

#endif /* PRISM_MOUSE_H */
