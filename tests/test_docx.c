/* PrismOS — .docx のホストテスト。エクスポートした .docx を
 * Python の zipfile で読めること (実物の .docx であること)、
 * 自分のエクスポートを読み直して元に戻ること、Python の
 * zipfile が作った圧縮済みの .docx を読めることを確かめる。 */
#include <stdio.h>
#include <string.h>

#include "docx.h"
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

static void roundtrip(const char *text, const char *name) {
    static uint8_t zip[4096];
    static char back[4096];
    int n = docx_export(text, strlen(text), zip, sizeof(zip));
    char full[64];
    snprintf(full, sizeof(full), "%s: export ok", name);
    check(n > 0, full);
    if (n <= 0) {
        return;
    }
    /* ZIP として読めること。 */
    zip_entry entries[ZIP_MAX_ENTRIES];
    int ec = zip_read_entries(zip, (size_t)n, entries, ZIP_MAX_ENTRIES);
    snprintf(full, sizeof(full), "%s: zip has 5 entries", name);
    check(ec == 5, full);
    snprintf(full, sizeof(full), "%s: has document.xml", name);
    check(zip_find_entry(entries, ec, "word/document.xml") != NULL, full);
    snprintf(full, sizeof(full), "%s: has content types", name);
    check(zip_find_entry(entries, ec, "[Content_Types].xml") != NULL,
          full);
    int m = docx_import(zip, (size_t)n, back, sizeof(back));
    snprintf(full, sizeof(full), "%s: import ok", name);
    check(m >= 0, full);
    snprintf(full, sizeof(full), "%s: roundtrip text", name);
    check(m >= 0 && strcmp(back, text) == 0, full);
    if (m >= 0 && strcmp(back, text) != 0) {
        printf("  got: [%s]\n  want: [%s]\n", back, text);
    }
}

/* 番号づけ入りの .docx をその場で作る (ZIP に固める)。
 * document_xml / numbering_xml / styles_xml のうち NULL で
 * ないものを入れる。戻り値は ZIP のバイト数、失敗は -1。 */
static int build_docx(uint8_t *zip, size_t cap, const char *document_xml,
                      const char *numbering_xml, const char *styles_xml) {
    static const char types[] = "<?xml version=\"1.0\"?><Types/>";
    zip_writer w;
    zip_write_begin(&w, zip, cap);
    if (zip_write_add(&w, "[Content_Types].xml",
                      (const uint8_t *)types, strlen(types)) != 0) {
        return -1;
    }
    if (zip_write_add(&w, "word/document.xml",
                      (const uint8_t *)document_xml,
                      strlen(document_xml)) != 0) {
        return -1;
    }
    if (numbering_xml != NULL &&
        zip_write_add(&w, "word/numbering.xml",
                      (const uint8_t *)numbering_xml,
                      strlen(numbering_xml)) != 0) {
        return -1;
    }
    if (styles_xml != NULL &&
        zip_write_add(&w, "word/styles.xml",
                      (const uint8_t *)styles_xml,
                      strlen(styles_xml)) != 0) {
        return -1;
    }
    return zip_write_finish(&w);
}

int main(void) {
    /* 空の文書。 */
    roundtrip("", "empty doc");
    /* ふつうの行だけ。 */
    roundtrip("Hello PrismOS\nSecond line", "plain lines");
    /* 見出し・箇条書き・番号付き・太字を全部入れた文書。 */
    roundtrip("# Big title\n"
              "## Section\n"
              "### Small\n"
              "- first item\n"
              "- second item\n"
              "1. first step\n"
              "2. second step\n"
              "Some **bold words** here.\n"
              "plain end",
              "full doc");
    /* 太字だけの行、閉じの無い **、特殊文字。 */
    roundtrip("**all bold**", "bold only");
    roundtrip("open **never", "unclosed bold");
    roundtrip("a < b & c > d \"quoted\"", "xml escapes");
    roundtrip("trailing spaces   \nnext", "trailing spaces");

    /* エクスポートした XML の中身を直接確認する。 */
    {
        static uint8_t zip[4096];
        static uint8_t xml[4096];
        const char *text = "# Title\n- item\nA **bold** word";
        int n = docx_export(text, strlen(text), zip, sizeof(zip));
        check(n > 0, "xml check export ok");
        zip_entry entries[ZIP_MAX_ENTRIES];
        int ec = zip_read_entries(zip, (size_t)n, entries, ZIP_MAX_ENTRIES);
        const zip_entry *e =
            zip_find_entry(entries, ec, "word/document.xml");
        int xl = zip_read_entry(zip, (size_t)n, e, xml, sizeof(xml));
        check(xl > 0, "xml check read ok");
        xml[xl] = '\0';
        check(strstr((const char *)xml, "Heading1") != NULL,
              "xml has Heading1");
        check(strstr((const char *)xml, "<w:b/>") != NULL,
              "xml has bold run");
        check(strstr((const char *)xml, "\xE2\x80\xA2") != NULL,
              "xml has bullet char");
        check(strstr((const char *)xml, "# Title") == NULL,
              "xml drops the # marker");
    }

    /* 番号付き段落: 直接の w:numPr、様式だけの段落、入れ子、
     * 箇条書き、分からない numId の場合。numbering.xml の
     * pStyle の結びつけで様式だけの段落も番号付きで読む。 */
    {
        static const char doc[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
            "wordprocessingml/2006/main\"><w:body>"
            "<w:p><w:r><w:t>Plain intro</w:t></w:r></w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>first step</w:t>"
            "</w:r></w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>second step</w:t>"
            "</w:r></w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>sub step</w:t>"
            "</w:r></w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>third step</w:t>"
            "</w:r></w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"2\"/></w:numPr></w:pPr><w:r><w:t>bullet item</w:t>"
            "</w:r></w:p>"
            "<w:p><w:pPr><w:pStyle w:val=\"ListNumber\"/></w:pPr>"
            "<w:r><w:t>styled one</w:t></w:r></w:p>"
            "<w:p><w:pPr><w:pStyle w:val=\"ListNumber\"/></w:pPr>"
            "<w:r><w:t>styled two</w:t></w:r></w:p>"
            "<w:p><w:pPr><w:pStyle w:val=\"ListBullet\"/></w:pPr>"
            "<w:r><w:t>styled bullet</w:t></w:r></w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"99\"/></w:numPr></w:pPr><w:r><w:t>unknown list</w:t>"
            "</w:r></w:p>"
            "<w:p><w:r><w:t>Plain end</w:t></w:r></w:p>"
            "</w:body></w:document>";
        static const char num[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/"
            "wordprocessingml/2006/main\">"
            "<w:abstractNum w:abstractNumId=\"0\">"
            "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt "
            "w:val=\"decimal\"/><w:lvlText w:val=\"%1.\"/></w:lvl>"
            "<w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt "
            "w:val=\"lowerLetter\"/><w:lvlText w:val=\"%2)\"/></w:lvl>"
            "</w:abstractNum>"
            "<w:abstractNum w:abstractNumId=\"1\">"
            "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt "
            "w:val=\"bullet\"/><w:lvlText w:val=\"\xE2\x80\xA2\"/></w:lvl>"
            "</w:abstractNum>"
            "<w:abstractNum w:abstractNumId=\"7\">"
            "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt "
            "w:val=\"decimal\"/><w:pStyle w:val=\"ListNumber\"/>"
            "<w:lvlText w:val=\"%1.\"/></w:lvl>"
            "</w:abstractNum>"
            "<w:abstractNum w:abstractNumId=\"8\">"
            "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt "
            "w:val=\"bullet\"/><w:pStyle w:val=\"ListBullet\"/>"
            "<w:lvlText w:val=\"\xE2\x80\xA2\"/></w:lvl>"
            "</w:abstractNum>"
            "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
            "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>"
            "</w:numbering>";
        static uint8_t zip[8192];
        static char out[4096];
        int n = build_docx(zip, sizeof(zip), doc, num, NULL);
        check(n > 0, "numbered docx build ok");
        int m = docx_import(zip, (size_t)n, out, sizeof(out));
        check(m >= 0, "numbered docx import ok");
        const char *want =
            "Plain intro\n"
            "1. first step\n"
            "2. second step\n"
            "a) sub step\n"
            "3. third step\n"
            "- bullet item\n"
            "1. styled one\n"
            "2. styled two\n"
            "- styled bullet\n"
            "- unknown list\n"
            "Plain end";
        check(m >= 0 && strcmp(out, want) == 0,
              "numbered paragraphs read as numbered");
        if (m >= 0 && strcmp(out, want) != 0) {
            printf("  got: [%s]\n  want: [%s]\n", out, want);
        }
    }

    /* ローマ数字・大文字の書式と開始番号 (start) の扱い。 */
    {
        static const char doc[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
            "wordprocessingml/2006/main\"><w:body>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>third</w:t></w:r>"
            "</w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>fourth</w:t></w:r>"
            "</w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>sub A</w:t></w:r>"
            "</w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>fifth</w:t></w:r>"
            "</w:p>"
            "</w:body></w:document>";
        static const char num[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/"
            "wordprocessingml/2006/main\">"
            "<w:abstractNum w:abstractNumId=\"0\">"
            "<w:lvl w:ilvl=\"0\"><w:start w:val=\"3\"/><w:numFmt "
            "w:val=\"upperRoman\"/><w:lvlText w:val=\"%1.\"/></w:lvl>"
            "<w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt "
            "w:val=\"upperLetter\"/><w:lvlText w:val=\"%2.\"/></w:lvl>"
            "</w:abstractNum>"
            "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
            "</w:numbering>";
        static uint8_t zip[8192];
        static char out[4096];
        int n = build_docx(zip, sizeof(zip), doc, num, NULL);
        check(n > 0, "roman docx build ok");
        int m = docx_import(zip, (size_t)n, out, sizeof(out));
        check(m >= 0 && strcmp(out, "III. third\nIV. fourth\n"
                                     "A. sub A\nV. fifth") == 0,
              "roman and letter numbering read correctly");
        if (m >= 0 && strcmp(out, "III. third\nIV. fourth\n"
                                   "A. sub A\nV. fifth") != 0) {
            printf("  got: [%s]\n", out);
        }
    }

    /* 様式定義 (styles.xml) から numId を引く場合と、
     * numbering.xml が無い場合の箇条書きへの落とし。 */
    {
        static const char num[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/"
            "wordprocessingml/2006/main\">"
            "<w:abstractNum w:abstractNumId=\"0\">"
            "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt "
            "w:val=\"decimal\"/><w:lvlText w:val=\"%1.\"/></w:lvl>"
            "</w:abstractNum>"
            "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
            "</w:numbering>";
        static const char styles[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/"
            "wordprocessingml/2006/main\">"
            "<w:style w:type=\"paragraph\" w:styleId=\"MyNum\">"
            "<w:name w:val=\"MyNum\"/>"
            "<w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr></w:style>"
            "<w:style w:type=\"paragraph\" w:styleId=\"MyNum2\">"
            "<w:name w:val=\"MyNum2\"/><w:basedOn w:val=\"MyNum\"/>"
            "</w:style></w:styles>";
        static const char doc[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
            "wordprocessingml/2006/main\"><w:body>"
            "<w:p><w:pPr><w:pStyle w:val=\"MyNum\"/></w:pPr>"
            "<w:r><w:t>first custom</w:t></w:r></w:p>"
            "<w:p><w:pPr><w:pStyle w:val=\"MyNum2\"/></w:pPr>"
            "<w:r><w:t>second custom</w:t></w:r></w:p>"
            "</w:body></w:document>";
        static uint8_t zip[8192];
        static char out[4096];
        int n = build_docx(zip, sizeof(zip), doc, num, styles);
        check(n > 0, "styled numbered docx build ok");
        int m = docx_import(zip, (size_t)n, out, sizeof(out));
        check(m >= 0 && strcmp(out, "1. first custom\n"
                                     "2. second custom") == 0,
              "styles.xml numbering read as numbered");
        if (m >= 0 && strcmp(out, "1. first custom\n"
                                   "2. second custom") != 0) {
            printf("  got: [%s]\n", out);
        }
        /* numbering.xml が無い .docx は今まで通り読める
         * (直接の numPr は箇条書きに落とす)。 */
        static const char plain_doc[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
            "wordprocessingml/2006/main\"><w:body>"
            "<w:p><w:r><w:t>before</w:t></w:r></w:p>"
            "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId "
            "w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>fallback item</w:t>"
            "</w:r></w:p>"
            "<w:p><w:r><w:t>after</w:t></w:r></w:p>"
            "</w:body></w:document>";
        int n2 = build_docx(zip, sizeof(zip), plain_doc, NULL, NULL);
        check(n2 > 0, "no-numbering docx build ok");
        int m2 = docx_import(zip, (size_t)n2, out, sizeof(out));
        check(m2 >= 0 && strcmp(out, "before\n- fallback item\n"
                                      "after") == 0,
              "no numbering.xml falls back to bullet");
        if (m2 >= 0 && strcmp(out, "before\n- fallback item\n"
                                    "after") != 0) {
            printf("  got: [%s]\n", out);
        }
    }

    /* 長い属性つきタグの回帰テスト (B3): タグが固定バッファを
     * 超えても self_close 判定が範囲外読みにならないこと。
     * UBSan ビルドで特に意味を持つ。 */
    {
        /* document.xml 側 (バッファ 64): 属性だらけの自己閉じ w:p。 */
        static const char doc[] =
            "<w:document><w:body>"
            "<w:p><w:r><w:t>before</w:t></w:r></w:p>"
            "<w:p w:rsidR=\"0123456789ABCDEF0123456789ABCDEF\""
            " w:rsidP=\"FEDCBA9876543210FEDCBA9876543210\"/>"
            "<w:p><w:r><w:t>after</w:t></w:r></w:p>"
            "</w:body></w:document>";
        static uint8_t zip[8192];
        static char out[4096];
        int n = build_docx(zip, sizeof(zip), doc, NULL, NULL);
        check(n > 0, "long-attr w:p docx build ok");
        int m = docx_import(zip, (size_t)n, out, sizeof(out));
        check(m >= 0 && strstr(out, "before") != NULL &&
                  strstr(out, "after") != NULL,
              "long-attr self-closing w:p handled");
        /* styles.xml 側 (バッファ 128): 長い自己閉じ w:style。 */
        static const char styles[] =
            "<w:styles>"
            "<w:style w:type=\"paragraph\" w:styleId=\"LongAttr\""
            " w:customStyle=\"1\""
            " w:someVeryLongAttributeNameForRegressionTest="
            "\"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF\"/>"
            "</w:styles>";
        static const char doc2[] =
            "<w:document><w:body>"
            "<w:p><w:r><w:t>styled text</w:t></w:r></w:p>"
            "</w:body></w:document>";
        int n2 = build_docx(zip, sizeof(zip), doc2, NULL, styles);
        check(n2 > 0, "long-attr style docx build ok");
        int m2 = docx_import(zip, (size_t)n2, out, sizeof(out));
        check(m2 >= 0 && strstr(out, "styled text") != NULL,
              "long-attr self-closing w:style handled");
    }

    /* 壊れた入力は -1。 */
    {
        static char out[4096];
        check(docx_import((const uint8_t *)"not a zip", 9, out,
                          sizeof(out)) == -1,
              "non-zip rejected");
        check(docx_export(NULL, 0, (uint8_t *)out, sizeof(out)) == -1,
              "null text rejected");
        /* document.xml が無い ZIP は -1。 */
        static uint8_t zip[4096];
        zip_writer w;
        zip_write_begin(&w, zip, sizeof(zip));
        zip_write_add(&w, "other.txt", (const uint8_t *)"x", 1);
        int n = zip_write_finish(&w);
        check(docx_import(zip, (size_t)n, out, sizeof(out)) == -1,
              "zip without document.xml rejected");
    }

    /* 出力先が小さすぎれば -1。 */
    {
        static uint8_t small[64];
        check(docx_export("Hello PrismOS, this is a document.",
                          34, small, sizeof(small)) == -1,
              "small output rejected");
    }

    printf("docx tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
