/* PrismOS — 最小限の文字列・メモリ関数 (freestanding 用)
 *
 * カーネルは -nostdlib でビルドするため、コンパイラが暗黙に呼ぶ
 * memcpy / memset 等も含め、自前で用意する。
 */
#ifndef PRISM_STRING_H
#define PRISM_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int memcmp(const void *a, const void *b, size_t n);
char *strstr(const char *haystack, const char *needle);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);

#endif /* PRISM_STRING_H */
