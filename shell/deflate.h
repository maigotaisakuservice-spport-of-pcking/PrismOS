/* PrismOS — DEFLATE 圧縮 (固定ハフマン、リテラルのみ)。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。ZIP の書き込みで
 * .pptx の XML を圧縮するために必要。
 *
 * 1 つの固定ハフマンブロックだけを出す (一致検索はしない)。
 * XML のような繰り返しの多いテキストは、固定ハフマンだけ
 * でも 6 割ほどに縮む。圧縮しても縮まない入力には使う
 * 意味がないので、呼び出し側が元の大きさと比べて選ぶこと。
 * 出力は正しい DEFLATE ストリームで、どんな ZIP 読み手
 * でも展開できる。
 */
#ifndef PRISM_DEFLATE_H
#define PRISM_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

/* src を固定ハフマンの DEFLATE ストリームにして out に書く。
 * 戻り値は書いたバイト数、出力先が足りなければ -1。 */
int deflate_compress_fixed(const uint8_t *src, size_t len, uint8_t *out,
                           size_t cap);

#endif /* PRISM_DEFLATE_H */
