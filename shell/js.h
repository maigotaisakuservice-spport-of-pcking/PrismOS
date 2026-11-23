/* PrismOS — 小さな JavaScript サブセット (純粋ロジック)。
 *
 * ブラウザが <script> を実行するためのインタプリタ。
 * 対応範囲 (正直なサブセット):
 *   - var / let / const、代入 (+=, -=)、if/else、while、
 *     for、関数宣言と再帰、return、ブロック
 *   - 数値は整数のみ (小数・Infinity・NaN は無い。
 *     割り算は整数除算、ゼロ除算はエラー)
 *   - 文字列 ('...' と "..."、+ で連結、== 等の比較)
 *   - 演算子: + - * / % == != < <= > >= && || ! ++ --
 *   - document.write(text): ホストへ文字列を渡す
 *   - 配列・オブジェクト・クロージャ・DOM (getElementById
 *     等) は未対応で、使うとエラーになる
 *   - 無限ループ防止のステップ上限と呼び出し深さ上限あり
 *
 * ホスト (ブラウザ) は write コールバックで出力を受け取る。
 */
#ifndef PRISM_JS_H
#define PRISM_JS_H

#include <stddef.h>

typedef struct {
    void *ctx;
    /* document.write の出力先。 */
    void (*write)(void *ctx, const char *text);
} js_host;

/* ソースを実行する。成功したら 0、エラーなら -1 で
 * err に理由が入る。 */
int js_run(const char *source, const js_host *host, char *err,
           size_t err_cap);

#endif /* PRISM_JS_H */
