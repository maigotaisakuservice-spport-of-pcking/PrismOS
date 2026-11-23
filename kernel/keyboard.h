/* PrismOS — PS/2 キーボードドライバ (v0.2 フェーズ1)
 *
 * 割り込み駆動 (IRQ1)。押されたキーをスキャンコードセット1から
 * ASCII に変換し、リングバッファにためる。シェルは
 * keyboard_get_char() で 1 文字ずつ取り出す。
 * 画面出力はまだ無いため、タイプした文字の表示 (エコー) は
 * シェル側がシリアルに行う。
 *
 * 対応するキー: 英数字と主な記号、Space、Enter、Backspace、Tab、
 * Shift (左右)、CapsLock、Esc、F1〜F12、矢印キー (0xE0 付き)。
 * F1〜F12 と矢印キーは ASCII ではないため、0x80 以上の特別な
 * コードで返す。デスクトップがアプリの切り替えに使う。
 * Ctrl は単独では使わないが、Ctrl+O (表計算) と Ctrl+V (動画)、
 * Ctrl+I (インストーラー)、Ctrl+M (メール) はアプリ切り替えの
 * 特別なコードで返す。
 */
#define PRISM_KEY_F1 0x81
#define PRISM_KEY_F2 0x82
#define PRISM_KEY_F3 0x83
#define PRISM_KEY_F4 0x84
#define PRISM_KEY_F5 0x85
#define PRISM_KEY_F6 0x86
#define PRISM_KEY_F7 0x87
#define PRISM_KEY_F8 0x88
#define PRISM_KEY_F9 0x89
#define PRISM_KEY_F10 0x8A
#define PRISM_KEY_F11 0x8B
#define PRISM_KEY_F12 0x8C
#define PRISM_KEY_UP 0x90
#define PRISM_KEY_DOWN 0x91
#define PRISM_KEY_RIGHT 0x92
#define PRISM_KEY_LEFT 0x93
#define PRISM_KEY_PGUP 0x94
#define PRISM_KEY_PGDN 0x95
#define PRISM_KEY_ZOOM_IN 0xD0  /* Ctrl + '+' */
#define PRISM_KEY_ZOOM_OUT 0xD1 /* Ctrl + '-' */
#define PRISM_KEY_CTRL_O 0xA1
#define PRISM_KEY_CTRL_V 0xA2
#define PRISM_KEY_CTRL_I 0xA3
#define PRISM_KEY_CTRL_M 0xA4
#define PRISM_KEY_CTRL_D 0xA5
#define PRISM_KEY_CTRL_P 0xA6
#define PRISM_KEY_CTRL_N 0xA7
#define PRISM_KEY_CTRL_E 0xA8
#define PRISM_KEY_CTRL_R 0xA9
/* 上のレガシーコードを持たない英字の Ctrl+英字は
 * PRISM_KEY_CTRL_LETTER_BASE + (英字 - 'a') で返す (0xB0..0xC9)。
 * ショートカットの割り当て変更で全ての Ctrl+英字を使えるように
 * するためのもの。レガシーコードのある英字 (o/v/i/m/d/p/n/e/r)
 * は従来のコードのまま。 */
#define PRISM_KEY_CTRL_LETTER_BASE 0xB0

#ifndef PRISM_KEYBOARD_H
#define PRISM_KEYBOARD_H

/* PS/2 コントローラを初期化する。割り込み許可 (sti) の前に呼ぶこと。 */
void keyboard_init(void);

/* バッファから 1 文字取り出す。空なら -1 を返す。
 * Enter は '\n'、Backspace は '\b' として返す。 */
int keyboard_get_char(void);
void keyboard_set_layout(int jis); /* 0=US, 1=JIS */

/* キーリピート (タイプラマチック) の設定。
 * preset: 0 = 遅い、1 = 標準、2 = 速い。
 * PS/2 キーボードへ 0xF3 コマンドで送る。 */
void keyboard_set_repeat(int preset);

/* 変換済みのキーコードを入力バッファへ入れる (USB HID
 * ドライバ用)。成功したら 0、満杯なら -1。 */
int keyboard_inject(int c);

/* IRQ1 のハンドラ (IDT 経由で呼ばれる)。0x60 を読んで変換し、
 * バッファに入れてから EOI を送る。 */
void keyboard_irq_handler(void);

#endif /* PRISM_KEYBOARD_H */
