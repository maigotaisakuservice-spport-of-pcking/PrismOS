/* PrismOS — DEFLATE 展開のホストテスト。
 * ベクトルは zlib (Python) が作った生の DEFLATE ストリーム
 * (tests/inflate_vectors.h)。レベル 0 (格納)、1 (固定)、
 * 6・9 (動的) を含む。 */
#include <stdio.h>
#include <string.h>

#include "inflate.h"
#include "inflate_vectors.h"

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
    static uint8_t out[8192];

    /* 全ベクトルを展開して期待値と比較する。 */
    static const uint8_t *vecs[] = {
        vec_empty_l0, vec_empty_l1, vec_empty_l6, vec_empty_l9,
        vec_hello_l0, vec_hello_l1, vec_hello_l6, vec_hello_l9,
        vec_repeat_a_l0, vec_repeat_a_l1, vec_repeat_a_l6,
        vec_repeat_a_l9,
        vec_lorem_l0, vec_lorem_l1, vec_lorem_l6, vec_lorem_l9,
        vec_mixed_l0, vec_mixed_l1, vec_mixed_l6, vec_mixed_l9,
        vec_text_l0, vec_text_l1, vec_text_l6, vec_text_l9,
    };
    static const size_t vec_lens[] = {
        vec_empty_l0_len, vec_empty_l1_len, vec_empty_l6_len,
        vec_empty_l9_len,
        vec_hello_l0_len, vec_hello_l1_len, vec_hello_l6_len,
        vec_hello_l9_len,
        vec_repeat_a_l0_len, vec_repeat_a_l1_len, vec_repeat_a_l6_len,
        vec_repeat_a_l9_len,
        vec_lorem_l0_len, vec_lorem_l1_len, vec_lorem_l6_len,
        vec_lorem_l9_len,
        vec_mixed_l0_len, vec_mixed_l1_len, vec_mixed_l6_len,
        vec_mixed_l9_len,
        vec_text_l0_len, vec_text_l1_len, vec_text_l6_len,
        vec_text_l9_len,
    };
    static const uint8_t *exps[] = {
        exp_empty_l0, exp_empty_l1, exp_empty_l6, exp_empty_l9,
        exp_hello_l0, exp_hello_l1, exp_hello_l6, exp_hello_l9,
        exp_repeat_a_l0, exp_repeat_a_l1, exp_repeat_a_l6,
        exp_repeat_a_l9,
        exp_lorem_l0, exp_lorem_l1, exp_lorem_l6, exp_lorem_l9,
        exp_mixed_l0, exp_mixed_l1, exp_mixed_l6, exp_mixed_l9,
        exp_text_l0, exp_text_l1, exp_text_l6, exp_text_l9,
    };
    static const size_t exp_lens[] = {
        exp_empty_l0_len, exp_empty_l1_len, exp_empty_l6_len,
        exp_empty_l9_len,
        exp_hello_l0_len, exp_hello_l1_len, exp_hello_l6_len,
        exp_hello_l9_len,
        exp_repeat_a_l0_len, exp_repeat_a_l1_len, exp_repeat_a_l6_len,
        exp_repeat_a_l9_len,
        exp_lorem_l0_len, exp_lorem_l1_len, exp_lorem_l6_len,
        exp_lorem_l9_len,
        exp_mixed_l0_len, exp_mixed_l1_len, exp_mixed_l6_len,
        exp_mixed_l9_len,
        exp_text_l0_len, exp_text_l1_len, exp_text_l6_len,
        exp_text_l9_len,
    };
    for (int i = 0; i < 24; i++) {
        memset(out, 0, sizeof(out));
        int n = inflate_decompress(vecs[i], vec_lens[i], out,
                                   sizeof(out));
        check(n == (int)exp_lens[i] &&
                  (exp_lens[i] == 0 ||
                   memcmp(out, exps[i], exp_lens[i]) == 0),
              vec_names[i]);
    }

    /* 壊れた入力は -1。 */
    {
        static const uint8_t bad_btype[] = { 0x07 }; /* BFINAL=1, BTYPE=3 */
        check(inflate_decompress(bad_btype, sizeof(bad_btype), out,
                                 sizeof(out)) == -1,
              "reserved btype rejected");
        check(inflate_decompress(NULL, 0, out, sizeof(out)) == -1,
              "null input rejected");
        check(inflate_decompress(vec_hello_l6, vec_hello_l6_len, out, 3) ==
                  -1,
              "output too small rejected");
        /* 途中で切れたストリームは -1。 */
        check(inflate_decompress(vec_lorem_l9, 4, out, sizeof(out)) == -1,
              "truncated stream rejected");
        /* 1 バイトだけの入力は -1。 */
        static const uint8_t one[] = { 0x03 };
        check(inflate_decompress(one, sizeof(one), out, sizeof(out)) == -1,
              "one byte rejected");
    }

    printf("inflate tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
