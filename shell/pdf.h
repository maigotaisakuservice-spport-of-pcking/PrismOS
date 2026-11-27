/* PrismOS — PDF のテキスト抽出と書き出し (E-3)
 *
 * 読み取り: ページ内容ストリーム (無圧縮 / FlateDecode)
 * から Tj / TJ の文字列を取り出す。レイアウトの完全な
 * 再現はしない (行区切りは Td/TD/T* で近似する)。
 * 書き出し: 最小の PDF 1.4 を生成する。標準フォント
 * (Helvetica) を使うため、書けるのは Latin 文字のみで、
 * それ以外は '?' になる (日本語の埋め込みフォントは
 * 別途必要 — 正直な制限)。
 */
#ifndef PRISM_PDF_H
#define PRISM_PDF_H

#include <stddef.h>
#include <stdint.h>

/* PDF データからテキストを抽出する。out に cap-1
 * バイトまで書き、NUL 終端する。抽出したバイト数を
 * 返す。PDF でなければ -1。 */
int pdf_extract_text(const uint8_t *data, size_t len, char *out,
                     size_t cap);

/* テキスト (Latin のみ、それ以外は '?') から最小の
 * PDF を組み立てる。成功したら長さ、失敗は -1。 */
int pdf_build(const char *text, size_t len, uint8_t *out,
              size_t cap);

#endif /* PRISM_PDF_H */
