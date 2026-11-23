/* PrismOS — ARM 用 PL011 UART ドライバ (QEMU virt 構成の UART0)
 *
 * ベースは 0x09000000。ポーリングで読み書きする。
 * 115200 baud、8 ビット、FIFO あり。時計は 24MHz。
 *   24000000 / (16 * 115200) = 13.02 なので IBRD=13、FBRD=1。
 */
#include "uart.h"

#define UART0_BASE ((volatile uint32_t *)0x09000000u)
#define UART_DR    0x00 /* データ */
#define UART_FR    0x06 /* フラグ (32 ビットレジスタ単位で数える) */
#define UART_IBRD  0x09 /* 整数ボーレート除数 */
#define UART_FBRD  0x0A /* 小数ボーレート除数 */
#define UART_LCRH  0x0B /* 回線制御 */
#define UART_CR    0x0C /* 制御 */
#define UART_IMSC  0x0E /* 割り込みマスク */
#define UART_ICR   0x11 /* 割り込みクリア */

#define UART_FR_TXFF (1u << 5) /* 送信 FIFO 満杯 */
#define UART_FR_RXFE (1u << 4) /* 受信 FIFO 空 */

static volatile uint32_t *uart_reg(uint32_t off) {
    return UART0_BASE + off;
}

void arm_uart_init(void) {
    /* 一旦止めて、設定してから動かす。 */
    *uart_reg(UART_CR) = 0;
    *uart_reg(UART_ICR) = 0x7ff;
    *uart_reg(UART_IBRD) = 13;
    *uart_reg(UART_FBRD) = 1;
    /* 8 ビット、FIFO 有効。 */
    *uart_reg(UART_LCRH) = 0x70;
    *uart_reg(UART_IMSC) = 0;
    /* 送信・受信・UART を有効化。 */
    *uart_reg(UART_CR) = 0x301;
}

/* 画面コンソールへのミラー先 (main.c が ramfb の準備後に
 * 登録する)。登録前は何もしない。 */
static void (*g_console_sink)(char);

void arm_uart_set_console_sink(void (*fn)(char)) {
    g_console_sink = fn;
}

void arm_uart_putc(char c) {
    if (c == '\n') {
        arm_uart_putc('\r');
    }
    while ((*uart_reg(UART_FR) & UART_FR_TXFF) != 0) {
        /* 送信 FIFO が空くまで待つ。 */
    }
    *uart_reg(UART_DR) = (uint32_t)(unsigned char)c;
    if (g_console_sink != NULL) {
        g_console_sink(c);
    }
}

/* 受信 FIFO が空なら -1、そうでなければ 1 バイト返す
 * (ブロックしない)。 */
int arm_uart_try_getc(void) {
    if ((*uart_reg(UART_FR) & UART_FR_RXFE) != 0) {
        return -1;
    }
    return (int)(*uart_reg(UART_DR) & 0xFF);
}

void arm_uart_write(const char *s) {
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        arm_uart_putc(*s);
        s++;
    }
}

void arm_uart_write_line(const char *s) {
    arm_uart_write(s);
    arm_uart_putc('\n');
}

char arm_uart_getc(void) {
    while ((*uart_reg(UART_FR) & UART_FR_RXFE) != 0) {
        /* 受信 FIFO に来るまで待つ。 */
    }
    return (char)(*uart_reg(UART_DR) & 0xffu);
}

int arm_uart_has_data(void) {
    return (*uart_reg(UART_FR) & UART_FR_RXFE) == 0;
}

size_t arm_uart_read_line(char *buf, size_t cap) {
    size_t n = 0;
    if (buf == NULL || cap == 0) {
        return 0;
    }
    for (;;) {
        char c = arm_uart_getc();
        if (c == '\r' || c == '\n') {
            arm_uart_putc('\n');
            break;
        }
        if (c == 0x7f || c == 0x08) {
            /* Backspace: 1 文字消して画面も戻す。 */
            if (n > 0) {
                n--;
                arm_uart_putc('\b');
                arm_uart_putc(' ');
                arm_uart_putc('\b');
            }
            continue;
        }
        if (c < 0x20 || c >= 0x7f) {
            continue;
        }
        if (n + 1 < cap) {
            buf[n] = c;
            n++;
            arm_uart_putc(c);
        }
    }
    buf[n] = '\0';
    return n;
}
