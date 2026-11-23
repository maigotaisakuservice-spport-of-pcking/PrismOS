/* test_ime: 日本語入力 (ローマ字→かな、辞書変換、
 * 候補選択、確定、取消、後退) のホストテスト。 */
#include <stdio.h>
#include <string.h>

#include "../kernel/ime.h"

static int checks;
static int failures;

#define CHECK(cond)                                        \
    do {                                                   \
        checks++;                                          \
        if (!(cond)) {                                     \
            failures++;                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                 \
        }                                                  \
    } while (0)

#define CHECK_STR(got, want)                                  \
    do {                                                      \
        checks++;                                             \
        if (strcmp((got), (want)) != 0) {                     \
            failures++;                                       \
            printf("FAIL %s:%d: got \"%s\" want \"%s\"\n",     \
                   __FILE__, __LINE__, (got), (want));        \
        }                                                     \
    } while (0)

static void kana(const char *roma, const char *want) {
    char buf[256];
    ime_romaji_to_kana(roma, buf, sizeof(buf));
    CHECK_STR(buf, want);
}

static void type_str(const char *s) {
    char out[256];
    for (const char *p = s; *p != '\0'; p++) {
        ime_feed((unsigned char)*p, out, sizeof(out));
    }
}

int main(void) {
    /* ローマ字→かなの純粋変換 */
    kana("konnichiha", "こんにちは");
    kana("gakkou", "がっこう");
    kana("kanji", "かんじ");
    kana("nihongo", "にほんご");
    kana("onna", "おんな");
    kana("shinbun", "しんぶん");
    kana("kitte", "きって");
    kana("matcha", "まっちゃ");
    kana("kyou", "きょう");
    kana("ryokou", "りょこう");
    kana("denwa", "でんわ");
    kana("kanpai", "かんぱい");
    kana("san", "さん");
    kana("hon", "ほん");
    kana("sensei", "せんせい");
    kana("tsuki", "つき");
    kana("watashi", "わたし");
    kana("toukyou", "とうきょう");
    kana("ohayou", "おはよう");
    kana("arigatou", "ありがとう");
    kana("xtu", "っ");
    kana("a", "あ");
    kana("n", "ん");
    kana("nn", "ん");
    kana("kani", "かに"); /* n+母音は な行 */

    /* 辞書引き */
    {
        const char *cands[5];
        int n = ime_lookup("かんじ", cands, 5);
        CHECK(n >= 2);
        CHECK_STR(cands[0], "漢字");
        CHECK_STR(cands[1], "感じ");
        CHECK(ime_lookup("あいうえお", cands, 5) == 0);
    }

    /* 状態機械: 変換→候補切替→確定 */
    ime_reset();
    CHECK(ime_on() == 0);
    CHECK(ime_toggle() == 1);
    type_str("kanji");
    CHECK(ime_active());
    CHECK_STR(ime_display(), "かんじ");
    {
        char out[256];
        CHECK(ime_feed(' ', out, sizeof(out)) == 0);
        CHECK_STR(ime_display(), "漢字");
        CHECK(ime_candidate_count() >= 3);
        CHECK(ime_feed(' ', out, sizeof(out)) == 0);
        CHECK_STR(ime_display(), "感じ");
        CHECK(ime_feed(' ', out, sizeof(out)) == 0);
        CHECK(ime_feed(' ', out, sizeof(out)) == 0);
        /* 候補を一周してカタカナ・かなも通る */
        int seen_kata = 0;
        for (int i = 0; i < 6; i++) {
            ime_feed(' ', out, sizeof(out));
            if (strcmp(ime_display(), "カンジ") == 0) {
                seen_kata = 1;
            }
        }
        CHECK(seen_kata);
        /* 先頭候補に戻して確定 */
        while (strcmp(ime_display(), "漢字") != 0) {
            ime_feed(' ', out, sizeof(out));
        }
        CHECK(ime_feed('\n', out, sizeof(out)) == 1);
        CHECK_STR(out, "漢字");
        CHECK(!ime_active());
    }

    /* 後退で読みを削る、変換中に後退すると変換が戻る */
    type_str("gakko");
    CHECK_STR(ime_display(), "がっこ");
    {
        char out[256];
        ime_feed('\b', out, sizeof(out));
        /* 末尾の子音は未確定としてローマ字で残る */
        CHECK_STR(ime_display(), "がっk");
        ime_feed('o', out, sizeof(out));
        CHECK_STR(ime_display(), "がっこ");
        ime_feed('u', out, sizeof(out));
        CHECK_STR(ime_display(), "がっこう");
        ime_feed(' ', out, sizeof(out));
        CHECK_STR(ime_display(), "学校");
        ime_feed('\b', out, sizeof(out));
        CHECK_STR(ime_display(), "がっこう");
        /* Esc で取消 */
        ime_feed(0x1B, out, sizeof(out));
        CHECK(!ime_active());
    }

    /* 未変換のまま Enter でかな確定 */
    type_str("neko");
    {
        char out[256];
        CHECK(ime_feed('\n', out, sizeof(out)) == 1);
        CHECK_STR(out, "ねこ");
    }

    /* 空のときの Space/Enter は呼び出し側へ素通し */
    {
        char out[256];
        CHECK(ime_feed(' ', out, sizeof(out)) == 0);
        CHECK(!ime_active());
        CHECK(ime_feed('\n', out, sizeof(out)) == 0);
    }

    /* オフのときは何もしない */
    ime_toggle();
    CHECK(ime_on() == 0);
    type_str("kanji");
    CHECK(!ime_active());

    printf("test_ime: %d checks, %d failures\n", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
