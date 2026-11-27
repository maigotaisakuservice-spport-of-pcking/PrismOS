/* PrismOS — ARM 用シリアル互換スタブ
 *
 * netstack など x86 と共通のコードが使う serial_* を
 * PL011 UART へつなぐ。 */
#include "serial.h"

#include "uart.h"

void serial_write_char(char c) {
    arm_uart_putc(c);
}

void serial_write(const char *s) {
    arm_uart_write(s);
}

void serial_write_line(const char *s) {
    arm_uart_write_line(s);
}
