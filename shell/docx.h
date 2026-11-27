/* PrismOS — .docx (OOXML) の読み書き。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。
 *
 * 書き込みは本物の .docx: ZIP (格納) に [Content_Types].xml、
 * _rels/.rels、word/document.xml を入れる。見出し・箇条書き・
 * 番号付き・太字を段落・書式に変換する。読み手は圧縮の有無を
 * 問わないので、格納でも正しい .docx になる。
 *
 * 読み込みは word/document.xml を読み、段落・見出し・太字を
 * 文書のテキスト形式に戻す。圧縮された項目は DEFLATE 展開
 * する。番号付き段落は word/numbering.xml を読んで番号づけ
 * を出し ("1. " の形)、段落様式だけの段落は番号づけ定義の
 * pStyle か word/styles.xml の様式定義から引く。番号づけが
 * 分からない段落は箇条書きに落とす。
 */
#ifndef PRISM_DOCX_H
#define PRISM_DOCX_H

#include <stddef.h>
#include <stdint.h>

/* 文書テキストを .docx にする。out に書いたバイト数を返す。
 * 失敗したら -1 (出力先が足りないなど)。 */
int docx_export(const char *text, size_t len, uint8_t *out, size_t cap);

/* .docx を文書テキストにする。out に NUL 終端のテキストを
 * 書いて、NUL を除くバイト数を返す。失敗したら -1 (ZIP で
 * ない、document.xml が無い、出力先が足りないなど)。 */
int docx_import(const uint8_t *data, size_t len, char *out, size_t cap);

#endif /* PRISM_DOCX_H */
