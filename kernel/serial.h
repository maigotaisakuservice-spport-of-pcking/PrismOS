/* PrismOS — シリアルポート (COM1) ドライバ
 *
 * v0.1 の唯一の入出力デバイス。Limine 8 では terminal プロトコルが
 * deprecated のため、画面出力には頼らず COM1 (0x3F8, 115200 baud, 8N1)
 * だけを使う。QEMU では `-serial stdio` でホストの標準入出力に繋がる。
 */
#ifndef PRISM_SERIAL_H
#define PRISM_SERIAL_H

#include <stddef.h>
#include <stdint.h>

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *s);
void serial_write_line(const char *s);
/* 1文字読めるまでポーリングして返す。 */
char serial_read_char(void);
/* 1行読む。'\r' / '\n' で確定し、入力はエコーする。
 * 戻り値は読み込んだ文字数。バッファは NUL 終端される。 */
size_t serial_read_line(char *buf, size_t cap);

#endif /* PRISM_SERIAL_H */
