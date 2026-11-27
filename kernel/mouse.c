/* PrismOS — PS/2 マウスドライバ (IRQ12、3 バイトパケット) */
#include "mouse.h"

#include "io.h"
#include "pic.h"
#include "security.h"
#include "serial.h"

#include <stdint.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

static volatile int mouse_found;
static volatile int mouse_pkt_state; /* 0, 1, 2 = パケットの何バイト目か */
static volatile uint8_t mouse_pkt[3];
static volatile int mouse_accum_dx;
static volatile int mouse_accum_dy;
static volatile int mouse_buttons;
static volatile int mouse_changed;
static volatile int mouse_pos_x;
static volatile int mouse_pos_y;
static volatile int mouse_bound_w;
static volatile int mouse_bound_h;

static void ps2_wait_input_empty(void) {
    for (int i = 0; i < 100000; i++) {
        if ((prism_inb(PS2_STATUS) & 0x02) == 0) {
            return;
        }
    }
}

static int ps2_wait_output_full(void) {
    for (int i = 0; i < 100000; i++) {
        if ((prism_inb(PS2_STATUS) & 0x01) != 0) {
            return 1;
        }
    }
    return 0;
}

static void ps2_flush_output(void) {
    while ((prism_inb(PS2_STATUS) & 0x01) != 0) {
        (void)prism_inb(PS2_DATA);
    }
}

/* マウスへ 1 バイト送る (コマンド 0xD4 経由)。ACK (0xFA) が
 * 返れば 1、来なければ 0 を返す。 */
static int mouse_send(uint8_t cmd) {
    ps2_flush_output();
    ps2_wait_input_empty();
    prism_outb(PS2_CMD, 0xD4);
    ps2_wait_input_empty();
    prism_outb(PS2_DATA, cmd);
    if (ps2_wait_output_full()) {
        uint8_t ack = prism_inb(PS2_DATA);
        return ack == 0xFA;
    }
    return 0;
}

/* マウスをリセットし、BAT (自己診断) の完了コード
 * 0xAA / 0x00 を読み捨てる。S3 復帰後はマウスが
 * コマンドを受け付けない状態で残ることがあり、
 * リセットを挟むと応答が戻る。ACK が無ければ何もしない
 * (呼び出し側の従来判定に任せる)。 */
static void mouse_reset_and_bat(void) {
    if (!mouse_send(0xFF)) {
        return;
    }
    int seen_aa = 0;
    for (int round = 0; round < 40 && !seen_aa; round++) {
        if (!ps2_wait_output_full()) {
            break;
        }
        if (prism_inb(PS2_DATA) == 0xAA) {
            seen_aa = 1;
        }
    }
    if (seen_aa && ps2_wait_output_full()) {
        (void)prism_inb(PS2_DATA); /* 機器 ID (通常 0x00) */
    }
}

void mouse_init(void) {
    mouse_found = 0;
    mouse_pkt_state = 0;
    mouse_accum_dx = 0;
    mouse_accum_dy = 0;
    mouse_buttons = 0;
    mouse_changed = 0;
    mouse_pos_x = 0;
    mouse_pos_y = 0;
    mouse_bound_w = 1280;
    mouse_bound_h = 800;

    ps2_flush_output();

    /* 補助ポート (マウス) を有効化する (コマンド 0xA8) */
    ps2_wait_input_empty();
    prism_outb(PS2_CMD, 0xA8);

    /* 設定バイトを読み直し、IRQ12 (bit1) を許可して補助ポートの
     * クロック無効 (bit5) を落とす。キーボードの設定は残す。 */
    ps2_wait_input_empty();
    prism_outb(PS2_CMD, 0x20);
    uint8_t config = 0;
    if (ps2_wait_output_full()) {
        config = prism_inb(PS2_DATA);
    }
    config |= 0x02;
    config &= (uint8_t)~0x20;
    ps2_wait_input_empty();
    prism_outb(PS2_CMD, 0x60);
    ps2_wait_input_empty();
    prism_outb(PS2_DATA, config);

    /* 既定値に戻してから、データ送信を有効化する。
     * 応答が無ければリセット + BAT を挟んで再試行する
     * (S3 復帰後の無応答対策)。 */
    if (!mouse_send(0xF6)) {
        mouse_reset_and_bat();
        if (!mouse_send(0xF6)) {
            serial_write_line("[boot] mouse: (no PS/2 mouse found)");
            return;
        }
    }
    if (!mouse_send(0xF4)) {
        serial_write_line("[boot] mouse: (no PS/2 mouse found)");
        return;
    }
    mouse_found = 1;
    serial_write_line("[boot] mouse: PS/2 ready (IRQ12).");
}

int mouse_present(void) {
    return mouse_found;
}

void mouse_set_bounds(int width, int height) {
    if (width > 0) {
        mouse_bound_w = width;
    }
    if (height > 0) {
        mouse_bound_h = height;
    }
}

void mouse_set_position(int x, int y) {
    mouse_pos_x = x;
    mouse_pos_y = y;
    mouse_accum_dx = 0;
    mouse_accum_dy = 0;
    mouse_changed = 1;
}

void mouse_position(int *x, int *y) {
    if (x != NULL) {
        *x = mouse_pos_x;
    }
    if (y != NULL) {
        *y = mouse_pos_y;
    }
}

int mouse_parse_packet(const uint8_t *pkt, int *dx, int *dy,
                       int *buttons) {
    if (pkt == NULL) {
        return -1;
    }
    /* bit3 は常に 1。立っていなければ同期ずれ。 */
    if ((pkt[0] & 0x08) == 0) {
        return -1;
    }
    int x = (int)(int8_t)pkt[1];
    int y = (int)(int8_t)pkt[2];
    /* PS/2 の Y は上が正。画面は下が正なので反転する。 */
    if (dx != NULL) {
        *dx = x;
    }
    if (dy != NULL) {
        *dy = -y;
    }
    if (buttons != NULL) {
        *buttons = pkt[0] & 0x07;
    }
    return 0;
}

void mouse_clamp_move(int *x, int *y, int dx, int dy, int width,
                      int height) {
    if (x == NULL || y == NULL) {
        return;
    }
    int nx = *x + dx;
    int ny = *y + dy;
    if (nx < 0) {
        nx = 0;
    }
    if (width > 0 && nx > width - 1) {
        nx = width - 1;
    }
    if (ny < 0) {
        ny = 0;
    }
    if (height > 0 && ny > height - 1) {
        ny = height - 1;
    }
    *x = nx;
    *y = ny;
}

void mouse_irq_handler(void) {
    uint8_t status = prism_inb(PS2_STATUS);
    if ((status & 0x01) == 0) {
        pic_send_eoi(12);
        return;
    }
    if ((status & 0x20) == 0) {
        /* キーボード側のデータ。ここでは読まず、IRQ1 に任せる。 */
        pic_send_eoi(12);
        return;
    }
    uint8_t b = prism_inb(PS2_DATA);
    /* マウスの動きもエントロピーに混ぜる。 */
    entropy_add(((uint32_t)b << 16) ^ 0x5A5AA5A5u);
    if (mouse_pkt_state == 0 && (b & 0x08) == 0) {
        /* 同期ずれ: このバイトを捨てる。 */
        pic_send_eoi(12);
        return;
    }
    mouse_pkt[mouse_pkt_state] = b;
    mouse_pkt_state++;
    if (mouse_pkt_state < 3) {
        pic_send_eoi(12);
        return;
    }
    mouse_pkt_state = 0;
    int dx = 0;
    int dy = 0;
    int buttons = 0;
    uint8_t pkt[3];
    pkt[0] = mouse_pkt[0];
    pkt[1] = mouse_pkt[1];
    pkt[2] = mouse_pkt[2];
    if (mouse_parse_packet(pkt, &dx, &dy, &buttons) == 0) {
        mouse_accum_dx += dx;
        mouse_accum_dy += dy;
        int nx = mouse_pos_x;
        int ny = mouse_pos_y;
        mouse_clamp_move(&nx, &ny, dx, dy, mouse_bound_w, mouse_bound_h);
        mouse_pos_x = nx;
        mouse_pos_y = ny;
        if (buttons != mouse_buttons) {
            mouse_buttons = buttons;
        }
        mouse_changed = 1;
    }
    pic_send_eoi(12);
}

/* Feed a relative movement + button state from another
 * source (the USB HID mouse driver) into the same
 * accumulation mouse_poll() drains, so the desktop sees
 * one merged pointer. */
void mouse_inject(int dx, int dy, int buttons) {
    mouse_accum_dx += dx;
    mouse_accum_dy += dy;
    mouse_buttons = buttons;
    mouse_changed = 1;
}

static int mouse_speed_pct = 100;
static int mouse_rem_x;
static int mouse_rem_y;

void mouse_set_speed(int percent) {
    if (percent < 25) {
        percent = 25;
    }
    if (percent > 400) {
        percent = 400;
    }
    mouse_speed_pct = percent;
    mouse_rem_x = 0;
    mouse_rem_y = 0;
}

int mouse_poll(int *dx, int *dy, int *buttons) {
    if (!mouse_changed) {
        return 0;
    }
    mouse_changed = 0;
    if (dx != NULL) {
        int total = mouse_accum_dx * mouse_speed_pct + mouse_rem_x;
        *dx = total / 100;
        mouse_rem_x = total - *dx * 100;
    }
    if (dy != NULL) {
        int total = mouse_accum_dy * mouse_speed_pct + mouse_rem_y;
        *dy = total / 100;
        mouse_rem_y = total - *dy * 100;
    }
    if (buttons != NULL) {
        *buttons = mouse_buttons;
    }
    mouse_accum_dx = 0;
    mouse_accum_dy = 0;
    return 1;
}

