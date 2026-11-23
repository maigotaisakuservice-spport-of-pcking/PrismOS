/* PrismOS — PS/2 マウスドライバのホストテスト (純粋関数だけ)。 */
#include <stdio.h>
#include <stdint.h>

#include "mouse.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

int main(void) {
    int dx = 0;
    int dy = 0;
    int buttons = 0;
    uint8_t pkt[3];

    /* 普通の移動: 右に 5、下に 3 (Y はパケットでは上が正なので
     * 3 は上向き、画面では -3 になる)。 */
    pkt[0] = 0x08;
    pkt[1] = 5;
    pkt[2] = 3;
    check(mouse_parse_packet(pkt, &dx, &dy, &buttons) == 0,
          "plain packet parses");
    check(dx == 5 && dy == -3 && buttons == 0,
          "plain packet values (dx=5, dy=-3, no buttons)");

    /* 負の移動: 符号ビット付き、-5 / +3 (画面向き)。 */
    pkt[0] = 0x38; /* bit3 + X sign + Y sign */
    pkt[1] = 0xFB; /* -5 */
    pkt[2] = 0xFD; /* -3 -> 画面では +3 */
    check(mouse_parse_packet(pkt, &dx, &dy, &buttons) == 0,
          "signed packet parses");
    check(dx == -5 && dy == 3, "signed packet values (dx=-5, dy=3)");

    /* 左ボタン。 */
    pkt[0] = 0x09;
    pkt[1] = 0;
    pkt[2] = 0;
    check(mouse_parse_packet(pkt, &dx, &dy, &buttons) == 0,
          "left button packet parses");
    check(buttons == 1, "left button bit set");

    /* 右ボタンと中ボタン。 */
    pkt[0] = 0x0E;
    check(mouse_parse_packet(pkt, &dx, &dy, &buttons) == 0,
          "right+middle packet parses");
    check(buttons == 6, "right+middle bits set");

    /* オーバーフロービットが立っていても移動量はそのまま。 */
    pkt[0] = 0xC8;
    pkt[1] = 10;
    pkt[2] = 10;
    check(mouse_parse_packet(pkt, &dx, &dy, &buttons) == 0,
          "overflow bits ignored");
    check(dx == 10 && dy == -10, "overflow packet values");

    /* 同期ずれ: bit3 が立っていないパケットは拒否する。 */
    pkt[0] = 0x00;
    pkt[1] = 5;
    pkt[2] = 5;
    check(mouse_parse_packet(pkt, &dx, &dy, &buttons) != 0,
          "packet without sync bit rejected");
    pkt[0] = 0x41;
    check(mouse_parse_packet(pkt, &dx, &dy, &buttons) != 0,
          "packet with bit6 but no sync bit rejected");
    check(mouse_parse_packet(NULL, &dx, &dy, &buttons) != 0,
          "NULL packet rejected");

    /* 位置のクランプ。 */
    int x = 1270;
    int y = 790;
    mouse_clamp_move(&x, &y, 20, 20, 1280, 800);
    check(x == 1279 && y == 799, "clamp at bottom right edge");
    x = 5;
    y = 5;
    mouse_clamp_move(&x, &y, -10, -10, 1280, 800);
    check(x == 0 && y == 0, "clamp at top left edge");
    x = 100;
    y = 100;
    mouse_clamp_move(&x, &y, 50, -20, 1280, 800);
    check(x == 150 && y == 80, "move inside the screen");

    printf("mouse tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
