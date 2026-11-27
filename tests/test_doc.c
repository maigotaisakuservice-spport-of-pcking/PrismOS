/* PrismOS — 文書のホストテスト。 */
#include <stdio.h>
#include <string.h>

#include "doc.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

int main(void) {
    doc_state d;

    /* 行の種類の判定。 */
    check(doc_classify_line("# Title") == DOC_LINE_H1, "h1 marker");
    check(doc_classify_line("## Title") == DOC_LINE_H2, "h2 marker");
    check(doc_classify_line("### Title") == DOC_LINE_H3, "h3 marker");
    check(doc_classify_line("- item") == DOC_LINE_BULLET, "bullet marker");
    check(doc_classify_line("1. first") == DOC_LINE_NUMBERED,
          "numbered marker");
    check(doc_classify_line("12. twelfth") == DOC_LINE_NUMBERED,
          "numbered 2 digits");
    check(doc_classify_line("plain text") == DOC_LINE_PLAIN, "plain line");
    check(doc_classify_line("#") == DOC_LINE_PLAIN, "hash alone is plain");
    check(doc_classify_line("# ") == DOC_LINE_PLAIN,
          "hash space alone is plain");
    check(doc_classify_line("-") == DOC_LINE_PLAIN, "dash alone is plain");
    check(doc_classify_line("1. ") == DOC_LINE_PLAIN,
          "number dot space alone is plain");
    check(doc_classify_line("123. too many") == DOC_LINE_NUMBERED,
          "3 digit number is numbered");
    check(doc_line_marker_len("# Title") == 2, "h1 marker len 2");
    check(doc_line_marker_len("## Title") == 3, "h2 marker len 3");
    check(doc_line_marker_len("### Title") == 4, "h3 marker len 4");
    check(doc_line_marker_len("- item") == 2, "bullet marker len 2");
    check(doc_line_marker_len("12. x") == 4, "numbered marker len 4");
    check(doc_line_marker_len("plain") == 0, "plain marker len 0");

    /* 太字の区切り。 */
    {
        const char *line = "a **bold** c";
        size_t s = 0, n = 0;
        int b = 0;
        check(doc_next_segment(line, strlen(line), 0, &s, &n, &b) == 1 &&
                  s == 0 && n == 2 && b == 0,
              "segment plain head");
        check(doc_next_segment(line, strlen(line), 2, &s, &n, &b) == 1 &&
                  s == 4 && n == 4 && b == 1,
              "segment bold middle");
        check(doc_next_segment(line, strlen(line), 10, &s, &n, &b) == 1 &&
                  s == 10 && n == 2 && b == 0,
              "segment plain tail");
        check(doc_next_segment(line, strlen(line), strlen(line), &s, &n,
                               &b) == 0,
              "segment past end");
        check(doc_next_segment("**only**", 8, 0, &s, &n, &b) == 1 &&
                  s == 2 && n == 4 && b == 1,
              "segment whole bold");
        check(doc_next_segment("open **never", 12, 0, &s, &n, &b) == 1 &&
                  b == 0 && n == 5,
              "unclosed bold: head is plain");
        check(doc_next_segment("open **never", 12, 5, &s, &n, &b) == 1 &&
                  b == 0 && n == 7,
              "unclosed bold: rest is plain");
    }

    /* 編集: 挿入・削除・改行・カーソル。 */
    doc_clear(&d);
    check(d.len == 0 && d.cursor == 0, "clear doc");
    doc_insert_char(&d, 'a');
    doc_insert_char(&d, 'b');
    doc_insert_char(&d, 'c');
    check(strcmp(d.buf, "abc") == 0 && d.cursor == 3, "insert abc");
    doc_cursor_left(&d);
    doc_insert_char(&d, 'X');
    check(strcmp(d.buf, "abXc") == 0, "insert in middle");
    doc_backspace(&d);
    check(strcmp(d.buf, "abc") == 0, "backspace removes X");
    doc_insert_newline(&d);
    doc_insert_char(&d, 'd');
    check(strcmp(d.buf, "ab\ndc") == 0, "newline splits line");
    check(doc_line_count(&d) == 2, "two lines");
    check(doc_cursor_row(&d) == 1, "cursor row 1");
    check(doc_cursor_col(&d) == 1, "cursor col 1");
    {
        size_t s = 0, e = 0;
        check(doc_line_bounds(&d, 0, &s, &e) == 1 && s == 0 && e == 2,
              "line 0 bounds");
        check(doc_line_bounds(&d, 1, &s, &e) == 1 && s == 3 && e == 5,
              "line 1 bounds");
        check(doc_line_bounds(&d, 2, &s, &e) == 0, "line 2 out of range");
    }
    doc_load(&d, "# Head\n- one\nplain\n", 19);
    check(d.len == 19 && strcmp(d.buf, "# Head\n- one\nplain\n") == 0,
          "load text");
    check(doc_line_count(&d) == 3, "load line count 3");
    check(doc_cursor_line_type(&d) == DOC_LINE_PLAIN,
          "cursor line is plain");

    /* 行の種類の書き換え。 */
    doc_load(&d, "Hello\nWorld\n", 12);
    d.cursor = 0;
    doc_set_line_type(&d, DOC_LINE_H1);
    check(strcmp(d.buf, "# Hello\nWorld\n") == 0, "set h1");
    check(doc_cursor_line_type(&d) == DOC_LINE_H1, "cursor line is h1");
    d.cursor = 8; /* "World" の行 */
    doc_set_line_type(&d, DOC_LINE_BULLET);
    check(strcmp(d.buf, "# Hello\n- World\n") == 0, "set bullet");
    doc_set_line_type(&d, DOC_LINE_NUMBERED);
    check(strcmp(d.buf, "# Hello\n2. World\n") == 0,
          "bullet to numbered (row 2)");
    /* 100 以上の番号も番号付き行として扱う。 */
    check(doc_classify_line("100. hundredth") == DOC_LINE_NUMBERED,
          "100 is numbered");
    check(doc_classify_line("999. last") == DOC_LINE_NUMBERED,
          "999 is numbered");
    check(doc_line_marker_len("123. abc") == 5, "3-digit marker len");
    doc_set_line_type(&d, DOC_LINE_PLAIN);
    check(strcmp(d.buf, "# Hello\nWorld\n") == 0, "numbered to plain");
    doc_set_line_type(&d, DOC_LINE_H3);
    check(strcmp(d.buf, "# Hello\n### World\n") == 0, "set h3");

    /* \r は捨てる。16384 バイトで切り捨てる。 */
    {
        static char big[20000];
        memset(big, 'x', sizeof(big));
        doc_load(&d, big, sizeof(big));
        check(d.len == DOC_MAX, "load truncates at 16384");
        doc_load(&d, "a\r\nb\r\n", 6);
        check(strcmp(d.buf, "a\nb\n") == 0, "load drops CR");
    }

    printf("doc tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
