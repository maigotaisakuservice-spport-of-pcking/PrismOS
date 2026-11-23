/* PrismOS — DEFLATE 展開 (RFC 1951)。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。zlib の .docx / .pptx
 * (圧縮された OOXML) を読むために必要。動的ハフマンにも
 * 対応する。出力先は呼び出し側が用意したバッファで、32KB の
 * 履歴ウィンドウを内部に持つ。
 */
#ifndef PRISM_INFLATE_H
#define PRISM_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* src の DEFLATE ストリームを out (cap バイト) に展開する。
 * 戻り値は展開したバイト数。失敗したら -1 (壊れた入力、
 * 出力先が足りない、入力の読み過ぎ)。入力がちょうど
 * ストリームの終わりで終わらなくても、最後のブロックまで
 * 読めた分は成功として扱う (zlib ストリームの後ろに別の
 * データが続く場合があるため)。 */
int inflate_decompress(const uint8_t *src, size_t src_len, uint8_t *out,
                       size_t cap);

#endif /* PRISM_INFLATE_H */
