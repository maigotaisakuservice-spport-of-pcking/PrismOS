/* PrismOS — シリアルポート (COM1) ドライバ */
#include "serial.h"
#include "io.h"
#include "string.h"

#define COM1 0x3F8

void serial_init(void) {
    prism_outb(COM1 + 1, 0x00); /* 割り込み無効 */
    prism_outb(COM1 + 3, 0x80); /* DLAB を立てる */
    prism_outb(COM1 + 0, 0x01); /* 除数 1 = 115200 baud */
    prism_outb(COM1 + 1, 0x00);
    prism_outb(COM1 + 3, 0x03); /* 8 ビット・パリティ無し・ストップ 1 */
    prism_outb(COM1 + 2, 0xC7); /* FIFO 有効・14 バイトで割り込み */
    prism_outb(COM1 + 4, 0x0B); /* RTS/DTR を立てる */
}

static int serial_received(void) {
    return prism_inb(COM1 + 5) & 0x01;
}

static int serial_transmit_empty(void) {
    return prism_inb(COM1 + 5) & 0x20;
}

void serial_write_char(char c) {
    while (!serial_transmit_empty()) {
        /* 送信可能になるまで待つ */
    }
    prism_outb(COM1, (uint8_t)c);
    if (c == '\n') {
        /* 端末表示のために LF の前に CR を補う */
        while (!serial_transmit_empty()) {
        }
        prism_outb(COM1, (uint8_t)'\r');
    }
}

void serial_write(const char *s) {
    while (*s != '\0') {
        serial_write_char(*s++);
    }
}

void serial_write_line(const char *s) {
    serial_write(s);
    serial_write_char('\n');
}

char serial_read_char(void) {
    while (!serial_received()) {
        /* 受信するまで待つ */
    }
    return (char)prism_inb(COM1);
}

int serial_has_data(void) {
    return serial_received();
}

int serial_try_read_char(void) {
    if (!serial_received()) {
        return -1;
    }
    return prism_inb(COM1);
}

size_t serial_read_line(char *buf, size_t cap) {
    size_t len = 0;
    for (;;) {
        char c = serial_read_char();
        if (c == '\r' || c == '\n') {
            serial_write_char('\n');
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                serial_write_char('\b');
                serial_write_char(' ');
                serial_write_char('\b');
            }
            continue;
        }
        if (len + 1 < cap) {
            buf[len++] = c;
            serial_write_char(c); /* エコー */
        }
    }
    buf[len] = '\0';
    return len;
}
