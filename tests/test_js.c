/* PrismOS — JavaScript サブセット (shell/js.c) の
 * ホストテスト。document.write の出力を集めて検証する。 */
#include <stdio.h>
#include <string.h>

#include "js.h"

static int checks;
static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            failures++;                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                              \
    } while (0)

static char out_buf[8192];
static size_t out_len;

static void collect(void *ctx, const char *text) {
    (void)ctx;
    size_t n = strlen(text);
    if (out_len + n < sizeof(out_buf)) {
        memcpy(out_buf + out_len, text, n);
        out_len += n;
        out_buf[out_len] = '\0';
    }
}

static js_host host = {NULL, collect};
static char err[128];

/* 実行して出力が期待通りか。 */
static int run_eq(const char *src, const char *expect) {
    out_len = 0;
    out_buf[0] = '\0';
    err[0] = '\0';
    if (js_run(src, &host, err, sizeof(err)) != 0) {
        printf("  js error for [%s]: %s\n", src, err);
        return 0;
    }
    return strcmp(out_buf, expect) == 0;
}

/* 実行がエラーになるか。 */
static int run_err(const char *src) {
    out_len = 0;
    out_buf[0] = '\0';
    err[0] = '\0';
    return js_run(src, &host, err, sizeof(err)) != 0 &&
           err[0] != '\0';
}

int main(void) {
    /* 算術と優先順位。 */
    CHECK(run_eq("document.write(1 + 2 * 3);", "7"));
    CHECK(run_eq("document.write((1 + 2) * 3);", "9"));
    CHECK(run_eq("document.write(17 % 5);", "2"));
    CHECK(run_eq("document.write(20 / 3);", "6"));
    CHECK(run_eq("document.write(-5 + 3);", "-2"));

    /* 変数と代入。 */
    CHECK(run_eq("var x = 4; x = x * 3; document.write(x);",
                 "12"));
    CHECK(run_eq("let y = 10; y += 5; y -= 3; document.write(y);",
                 "12"));
    CHECK(run_err("var a = 1, b = 2;")); /* 複数宣言は未対応 */
    CHECK(run_eq("var i = 0; i++; ++i; document.write(i);", "2"));

    /* 文字列。 */
    CHECK(run_eq("document.write('foo' + 'bar');", "foobar"));
    CHECK(run_eq("document.write('n=' + 42);", "n=42"));
    CHECK(run_eq("var s = 'x'; s += 'y'; document.write(s);",
                 "xy"));
    CHECK(run_eq("document.write('a' == 'a');", "1"));
    CHECK(run_eq("document.write('abc' < 'abd');", "1"));
    CHECK(run_eq("document.write(\"dq\" + 'sq');", "dqsq"));

    /* 比較と論理。 */
    CHECK(run_eq("document.write(3 < 5 && 5 <= 5);", "1"));
    CHECK(run_eq("document.write(3 > 5 || 0);", "0"));
    CHECK(run_eq("document.write(!(1 == 1));", "0"));

    /* if / else。 */
    CHECK(run_eq("var x = 7; if (x > 5) { document.write('big');"
                 " } else { document.write('small'); }",
                 "big"));
    CHECK(run_eq("var x = 2; if (x > 5) document.write('big');"
                 " else document.write('small');",
                 "small"));

    /* ループ。 */
    CHECK(run_eq("var s = 0; for (var i = 1; i <= 10; i++)"
                 " { s += i; } document.write(s);",
                 "55"));
    CHECK(run_eq("var s = 0; var i = 0; while (i < 5)"
                 " { s += i; i++; } document.write(s);",
                 "10"));
    CHECK(run_eq("var t = ''; for (var i = 0; i < 3; i++)"
                 " { t += i; } document.write(t);",
                 "012"));

    /* 関数と再帰。 */
    CHECK(run_eq("function fib(n) { if (n < 2) { return n; }"
                 " return fib(n - 1) + fib(n - 2); }"
                 " document.write(fib(10));",
                 "55"));
    CHECK(run_eq("function add(a, b) { return a + b; }"
                 " document.write(add(20, 22));",
                 "42"));
    /* 関数内のローカルは外に見えない・外の変数は壊れない。 */
    CHECK(run_eq("var v = 1; function f() { var v = 99;"
                 " return v; } document.write(f());"
                 " document.write(v);",
                 "991"));
    /* グローバルは関数から読める。 */
    CHECK(run_eq("var g = 5; function f() { return g * 2; }"
                 " document.write(f());",
                 "10"));

    /* ブロックスコープ。 */
    CHECK(run_eq("var x = 1; { var x = 2; document.write(x); }"
                 " document.write(x);",
                 "21"));

    /* コメント。 */
    CHECK(run_eq("// hello\nvar x = 3; /* multi\nline */"
                 " document.write(x);",
                 "3"));

    /* エラー系。 */
    CHECK(run_err("document.write(nope);"));       /* 未定義変数 */
    CHECK(run_err("var x = ;"));                   /* 構文エラー */
    CHECK(run_err("document.write(1 / 0);"));      /* ゼロ除算 */
    CHECK(run_err("var x = document.title;"));     /* 未対応メンバ */
    CHECK(run_err("nosuchfunc(1);"));              /* 未定義関数 */
    CHECK(run_err("while (1) { }"));               /* ステップ上限 */
    CHECK(run_err("function f() { return f(); } f();")); /* 深さ上限 */

    printf("test_js: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
