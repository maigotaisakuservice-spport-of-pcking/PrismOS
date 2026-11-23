/* PrismOS — .pptx のホストテスト。エクスポートした .pptx が
 * ZIP として読めること、スライドの XML にタイトルと箇条書き
 * が入っていること、読み直して元に戻ることを確かめる。 */
#include <stdio.h>
#include <string.h>

#include "pptx.h"
#include "zip.h"

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
    static uint8_t zip[16384];
    static uint8_t xml[4096];

    slides_clear(&s);
    slides_set_title(&s, 0, "Opening");
    slides_add_line(&s, 0, "point one");
    slides_add_line(&s, 0, "point two");
    slides_add(&s);
    slides_set_title(&s, 1, "Closing");
    slides_add_line(&s, 1, "goodbye");

    int n = pptx_export(&s, zip, sizeof(zip));
    check(n > 0, "pptx export ok");
    check(n <= 16384, "pptx fits in 16384 bytes");
    if (n > 0) {
        printf("pptx size: %d bytes for 2 slides\n", n);
    }
    zip_entry entries[ZIP_MAX_ENTRIES];
    int ec = zip_read_entries(zip, (size_t)n, entries, ZIP_MAX_ENTRIES);
    check(ec == 7, "pptx has 7 entries (2 slides)");
    check(zip_find_entry(entries, ec, "ppt/presentation.xml") != NULL,
          "has presentation.xml");
    check(zip_find_entry(entries, ec, "ppt/slides/slide1.xml") != NULL,
          "has slide1.xml");
    check(zip_find_entry(entries, ec, "ppt/slides/slide2.xml") != NULL,
          "has slide2.xml");
    check(zip_find_entry(entries, ec,
                         "ppt/slideMasters/slideMaster1.xml") != NULL,
          "has slide master");
    check(zip_find_entry(entries, ec,
                         "ppt/slideLayouts/slideLayout1.xml") == NULL,
          "no slide layout (by design, saves space)");
    /* スライド 1 の XML にタイトルと箇条書きがある。 */
    const zip_entry *e =
        zip_find_entry(entries, ec, "ppt/slides/slide1.xml");
    int xl = zip_read_entry(zip, (size_t)n, e, xml, sizeof(xml));
    check(xl > 0, "slide1 xml reads");
    xml[xl] = '\0';
    check(strstr((const char *)xml, "Opening") != NULL,
          "slide1 has title");
    check(strstr((const char *)xml, "point one") != NULL,
          "slide1 has bullet 1");
    check(strstr((const char *)xml, "point two") != NULL,
          "slide1 has bullet 2");
    check(strstr((const char *)xml, "type=\"title\"") != NULL,
          "slide1 has title placeholder");
    /* 読み直して元に戻る。 */
    slides_state back;
    int got = pptx_import(zip, (size_t)n, &back);
    check(got == 2, "import finds 2 slides");
    check(strcmp(back.slides[0].title, "Opening") == 0,
          "import title 1");
    check(back.slides[0].line_count == 2 &&
              strcmp(back.slides[0].lines[0], "point one") == 0 &&
              strcmp(back.slides[0].lines[1], "point two") == 0,
          "import bullets 1");
    check(strcmp(back.slides[1].title, "Closing") == 0,
          "import title 2");
    check(back.slides[1].line_count == 1 &&
              strcmp(back.slides[1].lines[0], "goodbye") == 0,
          "import bullets 2");

    /* 8 枚の短いスライドは 16384 バイト (ファイル上限) に収まる。
     * 4 枚も収まる。4096 バイトの出力先には 8 枚は収まらず
     * -1 になる (正直な失敗)。 */
    {
        slides_state eight;
        slides_clear(&eight);
        for (int i = 0; i < 8; i++) {
            char t[32];
            snprintf(t, sizeof(t), "Slide %d", i + 1);
            if (i > 0) {
                slides_add(&eight);
            }
            slides_set_title(&eight, i, t);
            slides_add_line(&eight, i, "bullet a");
            slides_add_line(&eight, i, "bullet b");
        }
        int en = pptx_export(&eight, zip, sizeof(zip));
        check(en > 0 && en <= 16384, "8 slides fit in 16384 bytes");
        if (en > 0) {
            printf("pptx size: %d bytes for 8 slides\n", en);
        }
        slides_state four;
        slides_clear(&four);
        for (int i = 0; i < 4; i++) {
            char t[32];
            snprintf(t, sizeof(t), "Slide %d", i + 1);
            if (i > 0) {
                slides_add(&four);
            }
            slides_set_title(&four, i, t);
            slides_add_line(&four, i, "bullet a");
            slides_add_line(&four, i, "bullet b");
        }
        int fn = pptx_export(&four, zip, sizeof(zip));
        check(fn > 0 && fn <= 16384, "4 slides fit in 16384 bytes");
        static uint8_t small[4096];
        check(pptx_export(&eight, small, sizeof(small)) == -1,
              "8 slides do not fit in 4096 bytes");
    }

    /* 8 枚の短いスライドを書き出して読み直す (タイトルと
     * 箇条書きが 8 枚とも戻る)。 */
    {
        slides_state big;
        static uint8_t bigzip[16384];
        slides_clear(&big);
        for (int i = 0; i < 8; i++) {
            char t[32];
            snprintf(t, sizeof(t), "Slide %d", i + 1);
            if (i > 0) {
                slides_add(&big);
            }
            slides_set_title(&big, i, t);
            slides_add_line(&big, i, "bullet a");
            slides_add_line(&big, i, "bullet b");
        }
        int bn = pptx_export(&big, bigzip, sizeof(bigzip));
        check(bn > 0, "8 slides export with big buffer");
        if (bn > 0) {
            printf("pptx size: %d bytes for 8 slides\n", bn);
        }
        slides_state back2;
        check(pptx_import(bigzip, (size_t)bn, &back2) == 8,
              "import 8 slides back");
        check(strcmp(back2.slides[7].title, "Slide 8") == 0,
              "import slide 8 title");
    }

    /* エントリが 24 件を超える ZIP でも、後ろのスライドまで
     * 読める (本物の .pptx は 30 件を超えることがある)。 */
    {
        static uint8_t big[16384];
        zip_writer w;
        zip_write_begin(&w, big, sizeof(big));
        for (int i = 0; i < 30; i++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "other/file%02d.txt", i);
            check(zip_write_add(&w, nm, (const uint8_t *)"x", 1) == 0,
                  "many-entries zip add ok");
        }
        slides_state ms;
        slides_clear(&ms);
        slides_set_title(&ms, 0, "Late Slide");
        slides_add_line(&ms, 0, "found me");
        static uint8_t mz[4096];
        int mn = pptx_export(&ms, mz, sizeof(mz));
        check(mn > 0, "many-entries slide zip ok");
        /* スライドの ZIP を分解して、30 件の後ろに足す。 */
        zip_entry me[ZIP_MAX_ENTRIES];
        int mc = zip_read_entries(mz, (size_t)mn, me, ZIP_MAX_ENTRIES);
        for (int i = 0; i < mc; i++) {
            static uint8_t xb[4096];
            int xl = zip_read_entry(mz, (size_t)mn, &me[i], xb,
                                    sizeof(xb));
            check(xl >= 0, "many-entries slide part read ok");
            check(zip_write_add(&w, me[i].name, xb, (size_t)xl) == 0,
                  "many-entries slide part add ok");
        }
        int bn = zip_write_finish(&w);
        check(bn > 0, "many-entries zip finish ok");
        slides_state back3;
        int got3 = pptx_import(big, (size_t)bn, &back3);
        check(got3 == 1, "import finds slide after 30 entries");
        check(strcmp(back3.slides[0].title, "Late Slide") == 0,
              "import late slide title");
        check(back3.slides[0].line_count == 1 &&
                  strcmp(back3.slides[0].lines[0], "found me") == 0,
              "import late slide bullet");
    }

    /* 長い属性つきタグの回帰テスト (B3): 自己閉じ a:t のタグが
     * 固定バッファ (32) を超えても範囲外読みにならず、
     * 空のテキストとして読み飛ばされること。 */
    {
        static const char slide[] =
            "<p:sld><p:cSld><p:spTree><p:sp><p:txBody>"
            "<a:p><a:r><a:t xml:space=\"preserve\""
            " data-long-attribute=\"0123456789abcdef\"/></a:r>"
            "<a:r><a:t>real text</a:t></a:r></a:p>"
            "</p:txBody></p:sp></p:spTree></p:cSld></p:sld>";
        static uint8_t z3[8192];
        zip_writer w;
        zip_write_begin(&w, z3, sizeof(z3));
        zip_write_add(&w, "ppt/slides/slide1.xml",
                      (const uint8_t *)slide, strlen(slide));
        int zn = zip_write_finish(&w);
        check(zn > 0, "long-attr a:t pptx build ok");
        slides_state out;
        int got = pptx_import(z3, (size_t)zn, &out);
        check(got == 1 && strcmp(out.slides[0].title, "real text") == 0,
              "long-attr self-closing a:t skipped, real text is title");
    }

    /* 壊れた入力は -1。 */
    {
        slides_state out;
        check(pptx_import((const uint8_t *)"not a zip", 9, &out) == -1,
              "non-zip rejected");
        check(pptx_export(NULL, zip, sizeof(zip)) == -1,
              "null slides rejected");
        static uint8_t z2[4096];
        zip_writer w;
        zip_write_begin(&w, z2, sizeof(z2));
        zip_write_add(&w, "other.txt", (const uint8_t *)"x", 1);
        int zn = zip_write_finish(&w);
        check(pptx_import(z2, (size_t)zn, &out) == -1,
              "zip without slides rejected");
    }

    printf("pptx tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
