/* PrismOS — DEFLATE 圧縮のホストテスト。圧縮したものを
 * 自分の inflate_decompress で展開して元に戻ること、Python
 * の zlib で展開できることを確かめる (後者はテスト側で
 * /tmp に出したバイナリを別途確認する)。 */
#include <stdio.h>
#include <string.h>

#include "deflate.h"
#include "inflate.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

static void roundtrip(const uint8_t *data, size_t len, const char *name) {
    static uint8_t comp[8192];
    static uint8_t back[8192];
    int cn = deflate_compress_fixed(data, len, comp, sizeof(comp));
    char full[64];
    snprintf(full, sizeof(full), "%s: compress ok", name);
    check(cn > 0, full);
    if (cn <= 0) {
        return;
    }
    int bn = inflate_decompress(comp, (size_t)cn, back, sizeof(back));
    snprintf(full, sizeof(full), "%s: roundtrip", name);
    check(bn == (int)len && (len == 0 || memcmp(back, data, len) == 0),
          full);
    snprintf(full, sizeof(full), "%s: compressed (%d -> %d)", name,
             (int)len, cn);
    check(cn < (int)len || len < 32, full);
}

int main(void) {
    roundtrip((const uint8_t *)"", 0, "empty");
    roundtrip((const uint8_t *)"hello world\n", 12, "hello");
    roundtrip((const uint8_t *)"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 30,
              "repeat a");
    /* XML のような繰り返しの多いテキストはよく縮む。 */
    {
        static char xml[2048];
        size_t n = 0;
        for (int i = 0; i < 20; i++) {
            const char *line =
                "<w:p><w:r><w:t xml:space=\"preserve\">Hello "
                "PrismOS</w:t></w:r></w:p>";
            size_t ll = strlen(line);
            memcpy(xml + n, line, ll);
            n += ll;
        }
        roundtrip((const uint8_t *)xml, n, "xml text");
    }
    /* ランダムに近いバイト列は縮まない (が、壊れない)。 */
    {
        static uint8_t rnd[512];
        uint32_t s = 12345;
        for (int i = 0; i < 512; i++) {
            s = s * 1103515245u + 12345u;
            rnd[i] = (uint8_t)(s >> 16);
        }
        static uint8_t comp[1024];
        static uint8_t back[1024];
        int cn = deflate_compress_fixed(rnd, sizeof(rnd), comp,
                                        sizeof(comp));
        check(cn > 0, "random compress ok");
        int bn = inflate_decompress(comp, (size_t)cn, back, sizeof(back));
        check(bn == 512 && memcmp(back, rnd, 512) == 0,
              "random roundtrip");
    }
    /* 出力先が小さすぎれば -1。 */
    {
        static uint8_t small[4];
        check(deflate_compress_fixed((const uint8_t *)"hello world hello",
                                     17, small, sizeof(small)) == -1,
              "small output rejected");
        check(deflate_compress_fixed(NULL, 0, small, sizeof(small)) == -1,
              "null input rejected");
    }

    printf("deflate tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
