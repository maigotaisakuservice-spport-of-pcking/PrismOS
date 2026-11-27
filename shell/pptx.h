/* PrismOS — .pptx (OOXML) の読み書き。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。
 *
 * 書き込みは本物の .pptx: ZIP (DEFLATE 圧縮) に
 * [Content_Types].xml、_rels/.rels、ppt/presentation.xml、
 * スライドマスタ・各スライドを入れる。1 枚目はタイトル、
 * 残りは箇条書きとして出す。読み手は圧縮の有無を問わない。
 *
 * 制約: レイアウトファイルは出さない (スライドは既定の
 * レイアウトで表示される)。1 ファイル 16384 バイトの上限に
 * 収めるためで、短いスライドなら 8 枚まで書き出せる。
 * 出力が上限を超えるときは pptx_export が -1 を返す。
 *
 * 読み込みは ppt/slides/slideN.xml を読み、最初のテキストを
 * タイトル、残りを箇条書きとして戻す。
 */
#ifndef PRISM_PPTX_H
#define PRISM_PPTX_H

#include <stddef.h>
#include <stdint.h>

#include "slides.h"

/* スライドを .pptx にする。out に書いたバイト数を返す。
 * 失敗したら -1 (出力先が足りないなど)。 */
int pptx_export(const slides_state *s, uint8_t *out, size_t cap);

/* .pptx をスライドに戻す。読み込んだスライドの数を返す
 * (負の数は失敗)。 */
int pptx_import(const uint8_t *data, size_t len, slides_state *out);

#endif /* PRISM_PPTX_H */
