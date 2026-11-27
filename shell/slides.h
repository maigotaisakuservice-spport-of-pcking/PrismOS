/* PrismOS — プレゼンテーション (Slides アプリ) の純粋ロジック。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。
 *
 * スライドは最大 8 枚。1 枚はタイトル 1 行と箇条書き 6 行まで。
 * 保存形式は素直なテキスト: スライドごとにタイトル行、続けて
 * "- 箇条書き" の行を並べ、"===" の行で区切る。
 */
#ifndef PRISM_SLIDES_H
#define PRISM_SLIDES_H

#include <stddef.h>

#define SLIDES_MAX 24
#define SLIDES_TITLE_MAX 64
#define SLIDES_LINES_MAX 6
#define SLIDES_LINE_MAX 64

typedef struct {
    char title[SLIDES_TITLE_MAX + 1];
    char lines[SLIDES_LINES_MAX][SLIDES_LINE_MAX + 1];
    int line_count;
} slide;

typedef struct {
    slide slides[SLIDES_MAX];
    int count;
} slides_state;

/* 空のデッキ (タイトルだけのスライド 1 枚) にする。 */
void slides_clear(slides_state *s);

/* 空のスライドを末尾に追加する。成功したらその番号、満杯なら
 * -1。 */
int slides_add(slides_state *s);

/* index 番目のスライドを消す。1 枚だけのときは消さない。 */
void slides_delete(slides_state *s, int index);

/* タイトル・箇条書きを入れる。長ければ切り詰める。
 * 範囲外は無視する。 */
void slides_set_title(slides_state *s, int index, const char *title);
void slides_set_line(slides_state *s, int index, int line,
                     const char *text);

/* 箇条書きを 1 行追加する。成功したらその行番号、満杯なら -1。 */
int slides_add_line(slides_state *s, int index, const char *text);

/* 箇条書きを 1 行消す。範囲外は無視する。 */
void slides_remove_line(slides_state *s, int index, int line);

/* 全スライドをテキスト形式に直列化する。戻り値は書いた文字数
 * (NUL を除く)。buf が足りなければ -1。 */
int slides_serialize(const slides_state *s, char *buf, size_t cap);

/* テキスト形式を読み込む。読み込んだスライドの数を返す
 * (負の数は失敗)。書式の悪い行は飛ばす。 */
int slides_parse(slides_state *s, const char *data, size_t len);

#endif /* PRISM_SLIDES_H */
