/* PrismOS — 表計算の純粋ロジック。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない (カーネルは SSE/x87
 * 無効でビルドするため)。数は 1000 分の 1 単位の固定小数点
 * (int64_t の milli 値) で持つ: 12.345 は 12345。
 *
 * グリッドは 10 列 (A〜J) × 20 行。セルには数値・文字列・
 * 数式 (= で始まる) が入る。数式は四則演算・括弧・セル参照・
 * SUM/AVG/MIN/MAX (範囲指定つき) を使える。参照が循環すれば
 * "#CIRC!"、0 除算は "#DIV/0!"、書式が悪ければ "#ERR"、
 * 桁あふれは "#NUM!" になる。
 *
 * 保存形式は 1 行 1 セルの "A1=内容" という素直なテキストで、
 * 空のセルは書かない (ファイルの 16384 バイト上限に収めるため)。
 */
#ifndef PRISM_SHEET_H
#define PRISM_SHEET_H

#include <stddef.h>
#include <stdint.h>

#define SHEET_COLS 26
#define SHEET_ROWS 50
#define SHEET_CELL_MAX 31 /* セルの中身 (NUL を除く) の上限 */

typedef struct {
    char raw[SHEET_CELL_MAX + 1];
    int state; /* 0 = 未評価、1 = 評価中、2 = 評価済み */
    int kind;  /* 0 = 空、1 = 数値、2 = 文字列、3 = エラー */
    int64_t value; /* kind == 1 のときの milli 値 */
    char err[8];   /* kind == 3 のときの表示 ("#DIV/0!" など) */
} sheet_cell;

typedef struct {
    sheet_cell cells[SHEET_ROWS * SHEET_COLS];
} sheet_state;

/* 全部空にする。 */
void sheet_clear(sheet_state *s);

/* セルに中身を入れて、全体を再計算する。raw は 31 文字までに
 * 切り詰める。範囲外の列・行は無視する。 */
void sheet_set(sheet_state *s, int col, int row, const char *raw);

/* セルの中身 (生の文字列) を返す。範囲外なら NULL。 */
const char *sheet_raw(const sheet_state *s, int col, int row);

/* セルの表示文字列を buf に書く: 数値は "12.345" の形、
 * 文字列はそのまま、エラーは "#DIV/0!" など、空は ""。
 * 戻り値は書いた文字数 (NUL を除く)。 */
int sheet_display(const sheet_state *s, int col, int row, char *buf,
                  size_t cap);

/* 全セルを再計算する (sheet_set が内部で呼ぶ。読み込み後に
 * まとめて呼ぶ用途もある)。 */
void sheet_recompute(sheet_state *s);

/* 空でないセルだけを "A1=内容\n" の形で直列化する。
 * 戻り値は書いた文字数 (NUL を除く)。buf が足りなければ -1。 */
int sheet_serialize(const sheet_state *s, char *buf, size_t cap);

/* 直列化したテキストを読み込む。書式の悪い行は飛ばす。
 * 読み込んだセルの数を返す (負の数は失敗)。 */
int sheet_parse(sheet_state *s, const char *data, size_t len);

/* milli 値を "12.345" / "12" / "-0.5" の形で buf に書く。
 * 戻り値は書いた文字数 (NUL を除く)。 */
int sheet_format_milli(int64_t milli, char *buf, size_t cap);

#endif /* PRISM_SHEET_H */
