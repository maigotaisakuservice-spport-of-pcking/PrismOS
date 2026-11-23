/* PrismOS — 表計算のホストテスト。 */
#include <stdio.h>
#include <string.h>

#include "sheet.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

static void disp(sheet_state *s, int col, int row, char *buf, size_t cap) {
    sheet_display(s, col, row, buf, cap);
}

int main(void) {
    sheet_state s;
    char buf[64];

    /* 数値の表示: 小数部の末尾の 0 は落とす。 */
    check(sheet_format_milli(12345, buf, sizeof(buf)) == 6 &&
              strcmp(buf, "12.345") == 0,
          "format 12.345");
    check(sheet_format_milli(12000, buf, sizeof(buf)) == 2 &&
              strcmp(buf, "12") == 0,
          "format 12");
    check(sheet_format_milli(-500, buf, sizeof(buf)) == 4 &&
              strcmp(buf, "-0.5") == 0,
          "format -0.5");
    check(sheet_format_milli(0, buf, sizeof(buf)) == 1 &&
              strcmp(buf, "0") == 0,
          "format 0");
    check(sheet_format_milli(1001, buf, sizeof(buf)) == 5 &&
              strcmp(buf, "1.001") == 0,
          "format 1.001");

    sheet_clear(&s);
    /* 数値と文字列。 */
    sheet_set(&s, 0, 0, "10");
    disp(&s, 0, 0, buf, sizeof(buf));
    check(strcmp(buf, "10") == 0, "number cell shows 10");
    sheet_set(&s, 0, 1, "2.5");
    disp(&s, 0, 1, buf, sizeof(buf));
    check(strcmp(buf, "2.5") == 0, "decimal cell shows 2.5");
    sheet_set(&s, 1, 0, "hello");
    disp(&s, 1, 0, buf, sizeof(buf));
    check(strcmp(buf, "hello") == 0, "text cell shows hello");
    disp(&s, 2, 2, buf, sizeof(buf));
    check(strcmp(buf, "") == 0, "empty cell shows empty");
    check(strcmp(sheet_raw(&s, 0, 0), "10") == 0, "raw text kept");

    /* 四則演算。 */
    sheet_set(&s, 2, 0, "=A1+A2");
    disp(&s, 2, 0, buf, sizeof(buf));
    check(strcmp(buf, "12.5") == 0, "A1+A2 = 12.5");
    sheet_set(&s, 2, 1, "=A1-A2");
    disp(&s, 2, 1, buf, sizeof(buf));
    check(strcmp(buf, "7.5") == 0, "A1-A2 = 7.5");
    sheet_set(&s, 2, 2, "=A1*A2");
    disp(&s, 2, 2, buf, sizeof(buf));
    check(strcmp(buf, "25") == 0, "A1*A2 = 25");
    sheet_set(&s, 2, 3, "=A1/A2");
    disp(&s, 2, 3, buf, sizeof(buf));
    check(strcmp(buf, "4") == 0, "A1/A2 = 4");
    sheet_set(&s, 2, 4, "=(A1+A2)*2");
    disp(&s, 2, 4, buf, sizeof(buf));
    check(strcmp(buf, "25") == 0, "(A1+A2)*2 = 25");
    sheet_set(&s, 2, 5, "=-A1");
    disp(&s, 2, 5, buf, sizeof(buf));
    check(strcmp(buf, "-10") == 0, "-A1 = -10");
    sheet_set(&s, 2, 6, "=A1/3");
    disp(&s, 2, 6, buf, sizeof(buf));
    check(strcmp(buf, "3.333") == 0, "A1/3 = 3.333");

    /* 参照先を変えると結果も変わる (再計算)。 */
    sheet_set(&s, 0, 0, "20");
    disp(&s, 2, 0, buf, sizeof(buf));
    check(strcmp(buf, "22.5") == 0, "recompute after A1 change");

    /* 関数と範囲。 */
    sheet_set(&s, 0, 0, "10");
    sheet_set(&s, 0, 1, "20");
    sheet_set(&s, 0, 2, "30");
    sheet_set(&s, 3, 0, "=SUM(A1:A3)");
    disp(&s, 3, 0, buf, sizeof(buf));
    check(strcmp(buf, "60") == 0, "SUM(A1:A3) = 60");
    sheet_set(&s, 3, 1, "=AVG(A1:A3)");
    disp(&s, 3, 1, buf, sizeof(buf));
    check(strcmp(buf, "20") == 0, "AVG(A1:A3) = 20");
    sheet_set(&s, 3, 2, "=MIN(A1:A3)");
    disp(&s, 3, 2, buf, sizeof(buf));
    check(strcmp(buf, "10") == 0, "MIN(A1:A3) = 10");
    sheet_set(&s, 3, 3, "=MAX(A1:A3)");
    disp(&s, 3, 3, buf, sizeof(buf));
    check(strcmp(buf, "30") == 0, "MAX(A1:A3) = 30");
    sheet_set(&s, 3, 4, "=SUM(A1:B2)");
    disp(&s, 3, 4, buf, sizeof(buf));
    check(strcmp(buf, "30") == 0, "SUM(A1:B2) skips text, = 30");
    sheet_set(&s, 3, 5, "=sum(a1:a3)");
    disp(&s, 3, 5, buf, sizeof(buf));
    check(strcmp(buf, "60") == 0, "lowercase sum() works");
    sheet_set(&s, 3, 6, "=AVG(A1:A1)");
    disp(&s, 3, 6, buf, sizeof(buf));
    check(strcmp(buf, "10") == 0, "AVG of one cell");

    /* エラー。 */
    sheet_set(&s, 4, 0, "=A1/0");
    disp(&s, 4, 0, buf, sizeof(buf));
    check(strcmp(buf, "#DIV/0!") == 0, "division by zero");
    sheet_set(&s, 4, 1, "=A1+");
    disp(&s, 4, 1, buf, sizeof(buf));
    check(strcmp(buf, "#ERR") == 0, "bad formula is #ERR");
    sheet_set(&s, 4, 2, "=A51");
    disp(&s, 4, 2, buf, sizeof(buf));
    check(strcmp(buf, "#ERR") == 0, "bad reference is #ERR");
    sheet_set(&s, 4, 3, "=FOO(A1:A2)");
    disp(&s, 4, 3, buf, sizeof(buf));
    check(strcmp(buf, "#ERR") == 0, "unknown function is #ERR");
    sheet_set(&s, 4, 4, "=B1");
    disp(&s, 4, 4, buf, sizeof(buf));
    check(strcmp(buf, "#ERR") == 0, "text cell in formula is #ERR");
    /* 循環参照。 */
    sheet_set(&s, 5, 0, "=F2");
    sheet_set(&s, 5, 1, "=F1");
    disp(&s, 5, 0, buf, sizeof(buf));
    check(strcmp(buf, "#CIRC!") == 0, "circular reference F1");
    disp(&s, 5, 1, buf, sizeof(buf));
    check(strcmp(buf, "#CIRC!") == 0, "circular reference F2");
    /* 自己参照。 */
    sheet_set(&s, 6, 0, "=G1");
    disp(&s, 6, 0, buf, sizeof(buf));
    check(strcmp(buf, "#CIRC!") == 0, "self reference is #CIRC!");
    /* エラーの伝播。 */
    sheet_set(&s, 7, 0, "=E1+1");
    disp(&s, 7, 0, buf, sizeof(buf));
    check(strcmp(buf, "#DIV/0!") == 0, "error propagates through refs");

    /* 保存と読み込みの往復。 */
    {
        char out[4096];
        int n = sheet_serialize(&s, out, sizeof(out));
        check(n > 0, "serialize produces text");
        check(strstr(out, "A1=10\n") != NULL, "serialized A1");
        check(strstr(out, "D1==SUM(A1:A3)\n") != NULL,
              "serialized formula D1");
        check(strstr(out, "J20=") == NULL, "empty cells not serialized");
        sheet_state s2;
        int loaded = sheet_parse(&s2, out, (size_t)n);
        check(loaded > 0, "parse loads cells");
        disp(&s2, 3, 0, buf, sizeof(buf));
        check(strcmp(buf, "60") == 0, "round-trip SUM still 60");
        disp(&s2, 0, 1, buf, sizeof(buf));
        check(strcmp(buf, "20") == 0, "round-trip A2 still 20");
        disp(&s2, 5, 0, buf, sizeof(buf));
        check(strcmp(buf, "#CIRC!") == 0, "round-trip keeps cycle error");
    }
    /* 壊れた行は飛ばす。 */
    {
        sheet_state s3;
        int loaded = sheet_parse(&s3, "A1=5\nBAD LINE\nZ9=1\nB2=7\n",
                                 24);
        check(loaded == 2, "bad lines skipped, 2 loaded");
        disp(&s3, 0, 0, buf, sizeof(buf));
        check(strcmp(buf, "5") == 0, "A1 loaded after bad lines");
        disp(&s3, 1, 1, buf, sizeof(buf));
        check(strcmp(buf, "7") == 0, "B2 loaded after bad lines");
    }
    /* 長い入力は 31 文字に切り詰める。 */
    sheet_set(&s, 0, 0, "abcdefghijklmnopqrstuvwxyz0123456789");
    check(strlen(sheet_raw(&s, 0, 0)) == SHEET_CELL_MAX,
          "long cell text truncated to 31");
    /* 範囲外は無視する。 */
    sheet_set(&s, 26, 0, "1");
    sheet_set(&s, 0, 50, "1");
    check(sheet_raw(&s, 26, 0) == NULL, "out of range col ignored");
    check(sheet_raw(&s, 0, 50) == NULL, "out of range row ignored");

    /* 26 列 x 50 行への拡張: Z50 と広い SUM。 */
    {
        sheet_clear(&s);
        sheet_set(&s, 25, 49, "7");
        sheet_set(&s, 0, 0, "=Z50*6");
        sheet_display(&s, 0, 0, buf, sizeof(buf));
        check(strcmp(buf, "42") == 0, "Z50 ref works");
        for (int r = 0; r < 50; r++) {
            sheet_set(&s, 1, r, "2");
        }
        sheet_set(&s, 2, 0, "=SUM(B1:B50)");
        sheet_display(&s, 2, 0, buf, sizeof(buf));
        check(strcmp(buf, "100") == 0, "SUM B1:B50 = 100");
        /* 範囲外参照はエラー表示のまま (クラッシュしない)。 */
        sheet_set(&s, 3, 0, "=A51");
        sheet_display(&s, 3, 0, buf, sizeof(buf));
        check(buf[0] == '#', "A51 is an error");
    }

    printf("sheet tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
