/* PrismOS — 最小の XML 読み取りヘルパー (.docx/.pptx 共用)。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。実体参照の解決と、
 * テキスト要素の中身の読み取りだけを行う。本格的な XML
 * パーサではない (OOXML の決まった形を読むためのもの)。
 */
#ifndef PRISM_XML_H
#define PRISM_XML_H

#include <stddef.h>

/* XML 実体参照を解いて 1 文字分読む。*adv に進んだバイト数
 * を入れる。戻り値は解いた文字 (Unicode の符号位置)。
 * 生のバイトはそのままの値を返す。失敗は 0。 */
int xml_decode_char(const char *s, size_t len, size_t pos, size_t *adv);

/* close_tag (例: "</w:t>") までのテキストを読む。実体参照を
 * 解き、非 ASCII は UTF-8 に戻して out に足す。戻り値は
 * close_tag の直後の位置、失敗は (size_t)-1。 */
size_t xml_read_text(const char *s, size_t len, size_t pos,
                     const char *close_tag, char *out, size_t *out_len,
                     size_t cap);

/* XML の特殊文字を逃がして out に書く。戻り値は書いた文字数
 * (NUL を除く)。足りなければ -1。 */
int xml_escape_text(const char *s, size_t len, char *out, size_t cap);

#endif /* PRISM_XML_H */
