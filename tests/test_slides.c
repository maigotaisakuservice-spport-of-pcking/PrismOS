/* PrismOS — プレゼンテーションのホストテスト。 */
#include <stdio.h>
#include <string.h>

#include "slides.h"

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
    slides_state s;
    char buf[1024];

    slides_clear(&s);
    check(s.count == 1, "clear has 1 slide");
    check(s.slides[0].title[0] == '\0', "clear title empty");
    slides_set_title(&s, 0, "Hello");
    check(strcmp(s.slides[0].title, "Hello") == 0, "set title");
    check(slides_add_line(&s, 0, "first point") == 0, "add line 0");
    check(slides_add_line(&s, 0, "second point") == 1, "add line 1");
    check(s.slides[0].line_count == 2, "two lines");
    slides_set_line(&s, 0, 1, "changed");
    check(strcmp(s.slides[0].lines[1], "changed") == 0, "set line");
    slides_remove_line(&s, 0, 0);
    check(s.slides[0].line_count == 1 &&
              strcmp(s.slides[0].lines[0], "changed") == 0,
          "remove line shifts");
    slides_remove_line(&s, 0, 5);
    check(s.slides[0].line_count == 1, "remove out of range ignored");

    /* 上限: 6 行まで、8 枚まで。 */
    for (int i = 0; i < 10; i++) {
        slides_add_line(&s, 0, "x");
    }
    check(s.slides[0].line_count == SLIDES_LINES_MAX,
          "lines capped at 6");
    check(slides_add_line(&s, 0, "overflow") == -1, "line overflow -1");
    for (int i = 0; i < SLIDES_MAX + 2; i++) {
        slides_add(&s);
    }
    check(s.count == SLIDES_MAX, "slides capped at SLIDES_MAX");
    check(slides_add(&s) == -1, "slide overflow -1");
    slides_delete(&s, SLIDES_MAX - 1);
    check(s.count == SLIDES_MAX - 1, "delete last slide");
    slides_delete(&s, 0);
    check(s.count == SLIDES_MAX - 2, "delete first slide");
    slides_clear(&s);
    slides_delete(&s, 0);
    check(s.count == 1, "cannot delete the only slide");

    /* 直列化と読み込みの往復。 */
    slides_clear(&s);
    slides_set_title(&s, 0, "Opening");
    slides_add_line(&s, 0, "point one");
    slides_add_line(&s, 0, "point two");
    int idx = slides_add(&s);
    check(idx == 1, "add second slide");
    slides_set_title(&s, 1, "Closing");
    slides_add_line(&s, 1, "goodbye");
    int n = slides_serialize(&s, buf, sizeof(buf));
    check(n > 0, "serialize ok");
    check(strcmp(buf, "Opening\n- point one\n- point two\n===\n"
                      "Closing\n- goodbye\n") == 0,
          "serialize text");
    slides_state t;
    int parsed = slides_parse(&t, buf, (size_t)n);
    check(parsed == 2, "parse 2 slides");
    check(strcmp(t.slides[0].title, "Opening") == 0, "parse title 1");
    check(t.slides[0].line_count == 2 &&
              strcmp(t.slides[0].lines[1], "point two") == 0,
          "parse lines 1");
    check(strcmp(t.slides[1].title, "Closing") == 0, "parse title 2");
    check(t.slides[1].line_count == 1 &&
              strcmp(t.slides[1].lines[0], "goodbye") == 0,
          "parse lines 2");

    /* タイトルの後のふつうの行は箇条書きになる。 */
    slides_parse(&t, "Title\nplain line\n", 17);
    check(t.count == 1 && t.slides[0].line_count == 1 &&
              strcmp(t.slides[0].lines[0], "plain line") == 0,
          "plain line after title becomes bullet");
    /* \r は捨てる。空の入力は 1 枚の空スライド。 */
    slides_parse(&t, "A\r\n- b\r\n", 8);
    check(strcmp(t.slides[0].title, "A") == 0 &&
              strcmp(t.slides[0].lines[0], "b") == 0,
          "parse drops CR");
    slides_parse(&t, "", 0);
    check(t.count == 1 && t.slides[0].title[0] == '\0',
          "empty input is one blank slide");

    /* 長いタイトル・行は切り詰める。 */
    {
        static char long_text[200];
        memset(long_text, 'x', sizeof(long_text) - 1);
        long_text[sizeof(long_text) - 1] = '\0';
        slides_clear(&s);
        slides_set_title(&s, 0, long_text);
        check(strlen(s.slides[0].title) == SLIDES_TITLE_MAX,
              "title truncated");
        slides_add_line(&s, 0, long_text);
        check(strlen(s.slides[0].lines[0]) == SLIDES_LINE_MAX,
              "line truncated");
    }

    /* 小さいバッファは -1。 */
    {
        char tiny[8];
        check(slides_serialize(&s, tiny, sizeof(tiny)) == -1,
              "small buffer rejected");
    }

    /* 24 枚までの拡張。 */
    {
        slides_state st2;
        slides_clear(&st2);
        int added = 0;
        for (int i = 0; i < 30; i++) {
            if (slides_add(&st2) >= 0) {
                added++;
            }
        }
        check(added == SLIDES_MAX - 1, "slides fill to cap");
        check(st2.count == SLIDES_MAX, "24 slides held");
        check(slides_add(&st2) == -1, "25th slide refused");
    }

    printf("slides tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
