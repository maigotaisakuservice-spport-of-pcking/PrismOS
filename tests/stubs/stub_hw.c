/* ホストテスト用のハードウェアスタブ。
 * カーネルソースをホストでテストするとき、シリアルや PIC の
 * 実装が無いとリンクできない。純粋関数だけをテストするため、
 * ここでは何もしない版を用意する (呼ばれないことが前提)。 */
#include <stddef.h>
#include <stdint.h>

#include "pic.h"
#include "serial.h"

void serial_init(void) {
}

void serial_write_char(char c) {
    (void)c;
}

void serial_write(const char *s) {
    (void)s;
}

void serial_write_line(const char *s) {
    (void)s;
}

char serial_read_char(void) {
    return 0;
}

int serial_has_data(void) {
    return 0;
}

int serial_try_read_char(void) {
    return -1;
}

size_t serial_read_line(char *buf, size_t cap) {
    (void)buf;
    (void)cap;
    return 0;
}

void pic_send_eoi(uint8_t irq) {
    (void)irq;
}

/* USB HID の入力合流先 (uhci.c が呼ぶ)。ホストテストでは
 * 純粋関数だけが対象なので、何もしない版でよい。本物が
 * リンクされるテスト (test_mouse) と衝突しないよう弱い
 * シンボルにする。 */
__attribute__((weak)) int keyboard_inject(int c) {
    (void)c;
    return 0;
}

__attribute__((weak)) void mouse_inject(int dx, int dy, int buttons) {
    (void)dx;
    (void)dy;
    (void)buttons;
}
