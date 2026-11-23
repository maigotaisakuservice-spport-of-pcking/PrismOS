/* PrismOS — PDF text extraction / building tests.
 * Fixtures are produced with Python zlib at test time
 * (the Makefile generates them before running). */
#include "../shell/pdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    const char *plain_path = argc > 1 ? argv[1] : "build/fixture-plain.pdf";
    const char *flate_path = argc > 2 ? argv[2] : "build/fixture-flate.pdf";
    size_t len = 0;
    char out[8192];
    uint8_t *pdf = read_file(plain_path, &len);
    CHECK(pdf != NULL);
    if (pdf != NULL) {
        int n = pdf_extract_text(pdf, len, out, sizeof(out));
        CHECK(n > 0);
        CHECK(strstr(out, "Hello PrismOS") != NULL);
        CHECK(strstr(out, "Second line") != NULL);
        free(pdf);
    }
    pdf = read_file(flate_path, &len);
    CHECK(pdf != NULL);
    if (pdf != NULL) {
        int n = pdf_extract_text(pdf, len, out, sizeof(out));
        CHECK(n > 0);
        CHECK(strstr(out, "Compressed greeting") != NULL);
        free(pdf);
    }
    /* not a PDF */
    const uint8_t junk[] = "hello world";
    CHECK(pdf_extract_text(junk, sizeof(junk) - 1, out,
                           sizeof(out)) == -1);
    /* build + re-extract round trip */
    static uint8_t built[32768];
    const char *text = "Round trip line\nWith (parens) and \\ slash";
    int blen = pdf_build(text, strlen(text), built, sizeof(built));
    CHECK(blen > 0);
    CHECK(memcmp(built, "%PDF-1.4", 8) == 0);
    if (blen > 0) {
        int n = pdf_extract_text(built, (size_t)blen, out,
                                 sizeof(out));
        CHECK(n > 0);
        CHECK(strstr(out, "Round trip line") != NULL);
        CHECK(strstr(out, "With (parens) and \\ slash") != NULL);
    }
    /* non-Latin bytes become '?' in built PDFs */
    blen = pdf_build("a\xE6\x97\xA5z", 5, built, sizeof(built));
    CHECK(blen > 0);
    if (blen > 0) {
        int n = pdf_extract_text(built, (size_t)blen, out,
                                 sizeof(out));
        CHECK(n > 0);
        CHECK(strstr(out, "a?z") != NULL ||
              strstr(out, "a???z") != NULL);
    }
    printf("pdf tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
