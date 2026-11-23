/* PrismOS — ARM 用 PL011 UART ドライバ (QEMU virt 構成の UART0)
 *
 * ベースは 0x09000000。ポーリングで読み書きする。
 * 115200 baud、8 ビット、FIFO あり。時計は 24MHz。
 */
#ifndef PRISM_ARM_UART_H
#define PRISM_ARM_UART_H

#include <stddef.h>
#include <stdint.h>

void arm_uart_init(void);
void arm_uart_putc(char c);
void arm_uart_set_console_sink(void (*fn)(char));
int arm_uart_try_getc(void);
void arm_uart_write(const char *s);
void arm_uart_write_line(const char *s);
/* 1文字読めるまでポーリングして返す。 */
char arm_uart_getc(void);
/* 受信データがあるかどうか (ブロックしない)。 */
int arm_uart_has_data(void);
/* 1行読む。'\r' / '\n' で確定し、入力はエコーする。
 * 戻り値は読み込んだ文字数。バッファは NUL 終端される。 */
size_t arm_uart_read_line(char *buf, size_t cap);

#endif /* PRISM_ARM_UART_H */
