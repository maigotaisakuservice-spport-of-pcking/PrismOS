/* PrismOS — PS/2 キーボードドライバ (v0.2 フェーズ1)
 *
 * スキャンコードセット1を ASCII に変換する。QEMU の 8042 は既定で
 * 変換 (set 2 -> set 1) が有効なため、送られてくるのは set 1。
 * 念のため初期化で設定バイトを読み直し、IRQ1 許可と変換を明示的に
 * 有効化してからキーボードを有効化する。
 *
 * IRQ ハンドラは 0x60 を読んで変換し、リングバッファに入れるだけ。
 * シリアル出力はしない (割り込み中のシリアル待ちを避けるため)。
 */
#include "keyboard.h"
#include "uhci.h"
#include "io.h"
#include "pic.h"
#include "security.h"

#include <stdint.h>

#define KBD_DATA 0x60
#define KBD_STATUS 0x64
#define KBD_CMD 0x64

/* スキャンコード -> ASCII (Shift 無し)。0 は「対応する文字なし」。 */
static const char SCAN_MAP[128] = {
    [0x01] = '\x1B',
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x39] = ' ',
};

/* JIS 配列で US と違うキー (物理位置は同じ)。半角/全角キー
 * (0x29) は ASCII を出さない。0x56 は JIS の「ろ」キー位置で
 * バックスラッシュ/アンダースコア。 */
static const char SCAN_MAP_JIS_DIFF[128] = {
    [0x0C] = '-', [0x0D] = '^', [0x1A] = '@', [0x1B] = '[',
    [0x27] = ';', [0x28] = ':', [0x29] = 0,   [0x2B] = ']',
    [0x56] = '\\',
};
static const char SCAN_SHIFT_MAP_JIS_DIFF[128] = {
    [0x03] = '"', [0x07] = '&', [0x08] = '\'', [0x09] = '(',
    [0x0A] = ')', [0x0B] = 0,   [0x0C] = '=', [0x0D] = '~',
    [0x1A] = '`', [0x1B] = '{', [0x27] = '+', [0x28] = '*',
    [0x2B] = '}', [0x56] = '_',
};

static int kbd_jis;

void keyboard_set_layout(int jis) { kbd_jis = jis ? 1 : 0; }

/* Shift ありのときの置き換え (英字以外)。英字は大文字化で処理する。 */
static const char SCAN_SHIFT_MAP[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x1A] = '{', [0x1B] = '}', [0x27] = ':', [0x28] = '"',
    [0x29] = '~', [0x2B] = '|', [0x33] = '<', [0x34] = '>',
    [0x35] = '?',
};

static volatile uint8_t kbd_buf[256];
static volatile uint8_t kbd_head; /* 書き込み位置 (IRQ が進める) */
static volatile uint8_t kbd_tail; /* 読み出し位置 (シェルが進める) */

static volatile int kbd_shift;
static volatile int kbd_caps;
static volatile int kbd_ctrl;
static volatile int kbd_e0; /* 0xE0 拡張プレフィクスを受け取った */

static void ps2_wait_input_empty(void) {
    for (int i = 0; i < 100000; i++) {
        if ((prism_inb(KBD_STATUS) & 0x02) == 0) {
            return;
        }
    }
}

static int ps2_wait_output_full(void) {
    for (int i = 0; i < 100000; i++) {
        if ((prism_inb(KBD_STATUS) & 0x01) != 0) {
            return 1;
        }
    }
    return 0;
}

static void ps2_flush_output(void) {
    while ((prism_inb(KBD_STATUS) & 0x01) != 0) {
        (void)prism_inb(KBD_DATA);
    }
}

void keyboard_init(void) {
    kbd_head = 0;
    kbd_tail = 0;
    kbd_shift = 0;
    kbd_caps = 0;
    kbd_ctrl = 0;
    kbd_e0 = 0;

    ps2_flush_output();

    /* 設定バイトを読む (コマンド 0x20) */
    ps2_wait_input_empty();
    prism_outb(KBD_CMD, 0x20);
    uint8_t config = 0;
    if (ps2_wait_output_full()) {
        config = prism_inb(KBD_DATA);
    }
    /* bit0: IRQ1 許可、bit6: set 2 -> set 1 変換を有効化、
     * bit4 (キーボード無効) を落としてキーボードを有効化する。 */
    config |= 0x01 | 0x40;
    config &= (uint8_t)~0x10;
    ps2_wait_input_empty();
    prism_outb(KBD_CMD, 0x60);
    ps2_wait_input_empty();
    prism_outb(KBD_DATA, config);

    /* キーボードを有効化 (コマンド 0xAE) */
    ps2_wait_input_empty();
    prism_outb(KBD_CMD, 0xAE);

    /* スキャン開始 (0xF4)。ACK (0xFA) が来なくても先に進む。 */
    ps2_flush_output();
    ps2_wait_input_empty();
    prism_outb(KBD_DATA, 0xF4);
    if (ps2_wait_output_full()) {
        (void)prism_inb(KBD_DATA);
    }
}

void keyboard_set_repeat(int preset) {
    /* 0xF3 のパラメータ: 上位 3 ビットが遅延
     * (0=250ms, 1=500ms, 2=750ms)、下位 5 ビットが
     * リピート率の指数表現。 */
    uint8_t param = preset == 0 ? 0x48 : preset == 2 ? 0x1F
                                                     : 0x30;
    ps2_flush_output();
    ps2_wait_input_empty();
    prism_outb(KBD_DATA, 0xF3);
    if (ps2_wait_output_full()) {
        (void)prism_inb(KBD_DATA); /* ACK */
    }
    ps2_wait_input_empty();
    prism_outb(KBD_DATA, param);
    if (ps2_wait_output_full()) {
        (void)prism_inb(KBD_DATA); /* ACK */
    }
}

static int decode_scancode(uint8_t sc) {
    if (sc == 0xE0) {
        kbd_e0 = 1;
        return -1;
    }
    if (kbd_e0) {
        /* 拡張キー。矢印キーだけ特別なコードで返す。 */
        kbd_e0 = 0;
        if (sc == 0x1D) {
            kbd_ctrl = 1; /* 右 Ctrl を押した */
            return -1;
        }
        if (sc == 0x9D) {
            kbd_ctrl = 0; /* 右 Ctrl を離した */
            return -1;
        }
        if ((sc & 0x80) != 0) {
            return -1; /* 離したとき */
        }
        if (sc == 0x48) {
            return PRISM_KEY_UP;
        }
        if (sc == 0x50) {
            return PRISM_KEY_DOWN;
        }
        if (sc == 0x4D) {
            return PRISM_KEY_RIGHT;
        }
        if (sc == 0x4B) {
            return PRISM_KEY_LEFT;
        }
        if (sc == 0x49) {
            return PRISM_KEY_PGUP;
        }
        if (sc == 0x51) {
            return PRISM_KEY_PGDN;
        }
        return -1;
    }
    if ((sc & 0x80) != 0) {
        /* キーを離したとき。Shift と Ctrl だけ追う */
        uint8_t code = (uint8_t)(sc & 0x7F);
        if (code == 0x2A || code == 0x36) {
            kbd_shift = 0;
        }
        if (code == 0x1D) {
            kbd_ctrl = 0;
        }
        return -1;
    }
    if (sc == 0x2A || sc == 0x36) {
        kbd_shift = 1;
        return -1;
    }
    if (sc == 0x3A) {
        kbd_caps = !kbd_caps;
        return -1;
    }
    if (sc == 0x1D) {
        /* 左 Ctrl を押した: 押したことだけ覚えておく */
        kbd_ctrl = 1;
        return -1;
    }
    if (sc == 0x38) {
        /* Alt は使わない */
        return -1;
    }
    if (sc == 0x3B) {
        return PRISM_KEY_F1;
    }
    if (sc == 0x3C) {
        return PRISM_KEY_F2;
    }
    if (sc == 0x3D) {
        return PRISM_KEY_F3;
    }
    if (sc == 0x3E) {
        return PRISM_KEY_F4;
    }
    if (sc == 0x3F) {
        return PRISM_KEY_F5;
    }
    if (sc == 0x40) {
        return PRISM_KEY_F6;
    }
    if (sc == 0x41) {
        return PRISM_KEY_F7;
    }
    if (sc == 0x42) {
        return PRISM_KEY_F8;
    }
    if (sc == 0x43) {
        return PRISM_KEY_F9;
    }
    if (sc == 0x44) {
        return PRISM_KEY_F10;
    }
    if (sc == 0x57) {
        return PRISM_KEY_F11;
    }
    if (sc == 0x58) {
        return PRISM_KEY_F12;
    }
    if (sc >= 128) {
        return -1;
    }
    char c = SCAN_MAP[sc];
    if (kbd_jis && SCAN_MAP_JIS_DIFF[sc] != 0) {
        c = SCAN_MAP_JIS_DIFF[sc];
    } else if (kbd_jis && sc == 0x29) {
        return -1; /* 半角/全角キーは文字を出さない */
    }
    if (c == 0) {
        return -1;
    }
    if (c >= 'a' && c <= 'z') {
        if (kbd_shift ^ kbd_caps) {
            c = (char)(c - 'a' + 'A');
        }
    } else if (kbd_shift) {
        char shifted = SCAN_SHIFT_MAP[sc];
        if (kbd_jis && (SCAN_SHIFT_MAP_JIS_DIFF[sc] != 0 ||
                        sc == 0x0B)) {
            shifted = SCAN_SHIFT_MAP_JIS_DIFF[sc];
            if (shifted == 0) {
                return -1; /* JIS の Shift+0 は文字なし */
            }
        }
        if (shifted != 0) {
            c = shifted;
        }
    }
    if (kbd_ctrl) {
        /* Ctrl と一緒に押した文字: アプリ切り替えの特別な
         * コードで返し、それ以外は捨てる (今まで通り Ctrl は
         * シェルに届かない)。 */
        if (c == 'o' || c == 'O') {
            return PRISM_KEY_CTRL_O;
        }
        if (c == 'v' || c == 'V') {
            return PRISM_KEY_CTRL_V;
        }
        if (c == 'i' || c == 'I') {
            return PRISM_KEY_CTRL_I;
        }
        if (c == 'm' || c == 'M') {
            return PRISM_KEY_CTRL_M;
        }
        if (c == 'd' || c == 'D') {
            return PRISM_KEY_CTRL_D;
        }
        if (c == 'p' || c == 'P') {
            return PRISM_KEY_CTRL_P;
        }
        if (c == 'n' || c == 'N') {
            return PRISM_KEY_CTRL_N;
        }
        if (c == 'e' || c == 'E') {
            return PRISM_KEY_CTRL_E;
        }
        if (c == 'r' || c == 'R') {
            return PRISM_KEY_CTRL_R;
        }
        if (c == '=' || c == '+') {
            return PRISM_KEY_ZOOM_IN;
        }
        if (c == '-') {
            return PRISM_KEY_ZOOM_OUT;
        }
        /* レガシーコードのない英字は汎用コードで返す。
         * ショートカットの割り当て変更 (設定画面) で使う。 */
        if (c >= 'a' && c <= 'z') {
            return PRISM_KEY_CTRL_LETTER_BASE + (c - 'a');
        }
        if (c >= 'A' && c <= 'Z') {
            return PRISM_KEY_CTRL_LETTER_BASE + (c - 'A');
        }
        return -1;
    }
    return (unsigned char)c;
}

void keyboard_irq_handler(void) {
    uint8_t sc = prism_inb(KBD_DATA);
    /* キー入力のタイミングと内容をエントロピーに混ぜる。
     * 割り込み中は SHA-256 を回さない (プールへの XOR だけ)。 */
    entropy_add(((uint32_t)sc << 8) ^ 0xA5A55A5Au);
    int c = decode_scancode(sc);
    if (c >= 0) {
        uint8_t next = (uint8_t)(kbd_head + 1);
        if (next != kbd_tail) {
            kbd_buf[kbd_head] = (uint8_t)c;
            kbd_head = next;
        }
        /* バッファが満杯なら捨てる (上書きしない) */
    }
    pic_send_eoi(1);
}

/* Push an already-decoded key code into the input
 * buffer. Used by the USB HID driver, whose reports are
 * translated to the same codes the PS/2 decoder makes,
 * so the shell and desktop cannot tell the sources
 * apart. Returns 0 on success, -1 when the buffer is
 * full or the code is negative. */
int keyboard_inject(int c) {
    if (c < 0) {
        return -1;
    }
    entropy_add(((uint32_t)(uint8_t)c << 8) ^ 0x5AA5A5A5u);
    uint8_t next = (uint8_t)(kbd_head + 1);
    if (next == kbd_tail) {
        return -1;
    }
    kbd_buf[kbd_head] = (uint8_t)c;
    kbd_head = next;
    return 0;
}

int keyboard_get_char(void) {
    /* USB HID のレポートもここで取り込む (シェル・
     * デスクトップ双方の入力待ちがこの関数を通る)。 */
    uhci_input_poll();
    if (kbd_head == kbd_tail) {
        return -1;
    }
    uint8_t c = kbd_buf[kbd_tail];
    kbd_tail = (uint8_t)(kbd_tail + 1);
    return c;
}
