/* PrismOS — 文書 (Docs アプリ) の純粋ロジック。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。
 *
 * 文書は 16384 バイトまでのテキスト。行頭の目印で行の種類が
 * 決まる (見出し・箇条書き・番号付き・ふつうの行)。太字は
 * 行の中で ** で囲む。ファイル形式はこのテキストそのもの
 * なので、保存・読み込みはそのままのバイト列で行う。
 * 書式の解釈は表示と .docx 変換がこのモジュールに聞く。
 */
#ifndef PRISM_DOC_H
#define PRISM_DOC_H

#include <stddef.h>

#define DOC_MAX 16384 /* 文書全体の上限 (ファイルの上限と同じ) */
#define DOC_LINE_MAX 256 /* 1 行の表示上の上限 (超えた分は折り返す) */

typedef enum {
    DOC_LINE_PLAIN = 0,
    DOC_LINE_H1 = 1,
    DOC_LINE_H2 = 2,
    DOC_LINE_H3 = 3,
    DOC_LINE_BULLET = 4,
    DOC_LINE_NUMBERED = 5
} doc_line_type;

typedef struct {
    char buf[DOC_MAX + 1];
    size_t len;
    size_t cursor; /* 0..len: 次に文字を入れる位置 */
} doc_state;

/* 空の文書にする。 */
void doc_clear(doc_state *d);

/* テキストを文書にする。\r は捨て、16384 バイトを超えた分は
 * 切り捨てる。カーソルは文末に置く。 */
void doc_load(doc_state *d, const char *text, size_t len);

/* 行頭の目印から行の種類を返す。line は NUL 終端の 1 行分
 * (改行なし)。 */
doc_line_type doc_classify_line(const char *line);

/* 行頭の目印の長さを返す (表示で目印を除くときに使う)。
 * ふつうの行は 0。 */
size_t doc_line_marker_len(const char *line);

/* 太字の区切りを 1 つ読む。line[pos..] から次の区切りを返し、
 * *seg_start / *seg_len に区切りの範囲、*bold に太字かどうか
 * を入れる。区切りが無ければ 0 を返す。区切りは ** で囲ま
 * れた範囲。 */
int doc_next_segment(const char *line, size_t len, size_t pos,
                     size_t *seg_start, size_t *seg_len, int *bold);

/* カーソル位置に 1 文字入れる。満杯なら何もしない。 */
void doc_insert_char(doc_state *d, char c);

/* カーソルの直前の 1 文字を消す。先頭なら何もしない。 */
void doc_backspace(doc_state *d);

/* カーソル位置で改行する。満杯なら何もしない。 */
void doc_insert_newline(doc_state *d);

/* カーソルを 1 文字前 / 後ろに動かす (範囲内に収める)。 */
void doc_cursor_left(doc_state *d);
void doc_cursor_right(doc_state *d);

/* カーソル位置の行番号 (0 から) と、その行の先頭からの
 * カーソルの列を返す。 */
size_t doc_cursor_row(const doc_state *d);
size_t doc_cursor_col(const doc_state *d);

/* 行数を返す (末尾の改行の後ろに空行がある場合は数えない)。 */
size_t doc_line_count(const doc_state *d);

/* row 行目の範囲 [start, end) を返す (end は改行の位置か文末)。
 * 範囲外なら 0 を返す。 */
int doc_line_bounds(const doc_state *d, size_t row, size_t *start,
                    size_t *end);

/* カーソルがいる行の種類を返す。 */
doc_line_type doc_cursor_line_type(const doc_state *d);

/* カーソルがいる行の行頭の目印を kind に書き換える。
 * kind が DOC_LINE_PLAIN なら目印を消す。目印の長さが変わっ
 * ても文書の上限を超えない範囲で行う。 */
void doc_set_line_type(doc_state *d, doc_line_type kind);

#endif /* PRISM_DOC_H */
