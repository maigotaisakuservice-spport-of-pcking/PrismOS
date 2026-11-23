/* PrismOS — 32bit カーネルの内部ヘッダ */
#ifndef PRISM_KERNEL32_H
#define PRISM_KERNEL32_H

#include <stddef.h>
#include <stdint.h>

#include "core.h"

/* alloc.c: 静的アリーナ上のフリーリスト割り当て器 */
void alloc32_init(void);
void *alloc32(size_t size);
void free32(void *ptr);
size_t alloc32_free_bytes(void);

/* main32.c が desktop32.c に出す口 */
void main32_con_write(const char *s);
void main32_con_putc(char c);
/* 入力を 1 文字だけ覗く (0 = まだ無い)。 */
int main32_poll_char(void);
/* ローカルコマンドとシェルコアをまとめて実行する。 */
void main32_exec_line(prism_shell_state *state, const char *line);
/* 出力 1 文字ごとの受け口 (GUI の端末描画用)。 */
void main32_set_con_sink(void (*sink)(char c));
/* PS/2 マウスの生バイトの受け口 (GUI 用)。 */
void main32_set_mouse_sink(void (*sink)(uint8_t byte));
/* VGA テキストモードに戻す (GUI 終了時)。 */
void main32_text_mode(void);

/* desktop32.c: Bochs VBE フレームバッファの GUI デスクトップ */
int desktop32_available(void);
void desktop32_run(prism_shell_state *state);

#endif /* PRISM_KERNEL32_H */
