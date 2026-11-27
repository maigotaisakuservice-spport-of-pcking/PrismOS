/* PrismOS — ZIP のホストテスト。書き込み (格納) と読み込み
 * (格納 + DEFLATE) の往復、CRC の検出、壊れた入力の拒否を
 * 確かめる。DEFLATE 項目の入力は Python の zipfile が作った
 * 実物の ZIP を tests/zip_deflated.bin に置いてある。 */
#include <stdio.h>
#include <string.h>

#include "zip.h"
#include "zip_deflated_bin.h"

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
    static uint8_t out[4096];
    static uint8_t back[4096];
    zip_writer w;
    zip_entry entries[ZIP_MAX_ENTRIES];

    /* CRC-32 は既知の値と一致する (IEEE 802.3)。 */
    check(zip_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u,
          "crc32 of 123456789");
    check(zip_crc32((const uint8_t *)"", 0) == 0u, "crc32 of empty");

    /* 格納で 3 ファイル書いて、読み直す。 */
    zip_write_begin(&w, out, sizeof(out));
    check(zip_write_add(&w, "a.txt", (const uint8_t *)"hello", 5) == 0,
          "zip add a.txt");
    check(zip_write_add(&w, "b/c.txt", (const uint8_t *)"", 0) == 0,
          "zip add empty file");
    static uint8_t big[1000];
    memset(big, 'x', sizeof(big));
    check(zip_write_add(&w, "big.bin", big, sizeof(big)) == 0,
          "zip add big.bin");
    int total = zip_write_finish(&w);
    check(total > 0, "zip finish ok");
    int n = zip_read_entries(out, (size_t)total, entries, ZIP_MAX_ENTRIES);
    check(n == 3, "zip read finds 3 entries");
    const zip_entry *e = zip_find_entry(entries, n, "a.txt");
    check(e != NULL && e->method == 0 && e->uncomp_size == 5,
          "find a.txt");
    check(zip_find_entry(entries, n, "nope.txt") == NULL,
          "missing entry is NULL");
    int r = zip_read_entry(out, (size_t)total, e, back, sizeof(back));
    check(r == 5 && memcmp(back, "hello", 5) == 0, "read a.txt back");
    e = zip_find_entry(entries, n, "big.bin");
    r = zip_read_entry(out, (size_t)total, e, back, sizeof(back));
    check(r == 1000 && back[999] == 'x', "read big.bin back");
    e = zip_find_entry(entries, n, "b/c.txt");
    r = zip_read_entry(out, (size_t)total, e, back, sizeof(back));
    check(r == 0, "read empty file back");

    /* 出力先が足りなければ失敗する。 */
    {
        zip_writer small;
        static uint8_t tiny[160];
        zip_write_begin(&small, tiny, sizeof(tiny));
        check(zip_write_add(&small, "a.txt",
                            (const uint8_t *)"hello world hello world",
                            23) == 0,
              "small add fits");
        check(zip_write_finish(&small) > 0, "small finish fits");
        check(zip_write_add(&small, "b.txt",
                            (const uint8_t *)"hello world hello world "
                                            "hello world hello world",
                            47) == -1,
              "small add overflows");
    }

    /* 壊れた入力は -1。 */
    check(zip_read_entries(out, 10, entries, ZIP_MAX_ENTRIES) == -1,
          "too short is rejected");
    check(zip_read_entries(NULL, 100, entries, ZIP_MAX_ENTRIES) == -1,
          "null is rejected");
    {
        static uint8_t garbage[100];
        memset(garbage, 0x41, sizeof(garbage));
        check(zip_read_entries(garbage, sizeof(garbage), entries,
                               ZIP_MAX_ENTRIES) == -1,
              "garbage is rejected");
        /* CRC を壊すと読み込みが失敗する。 */
        static uint8_t corrupt[4096];
        memcpy(corrupt, out, (size_t)total);
        corrupt[35] ^= 0xFF; /* "hello" の 1 バイト目 */
        int cn = zip_read_entries(corrupt, (size_t)total, entries,
                                  ZIP_MAX_ENTRIES);
        check(cn == 3, "corrupt zip still parses");
        const zip_entry *ce =
            zip_find_entry(entries, cn, "a.txt");
        check(zip_read_entry(corrupt, (size_t)total, ce, back,
                             sizeof(back)) == -1,
              "crc mismatch rejected");
    }

    /* 細工した大きな local_offset / cd_offset を拒否する
     * (32 ビット環境で境界チェックが巻き戻る問題の回帰テスト)。
     * 0xFFFFFFE2 以上だと off + 30 が 32 ビットで巻き戻る。 */
    {
        int n2 = zip_read_entries(out, (size_t)total, entries,
                                  ZIP_MAX_ENTRIES);
        check(n2 == 3, "crafted-offset zip parses");
        const zip_entry *ae = zip_find_entry(entries, n2, "a.txt");
        check(ae != NULL, "crafted-offset entry found");
        if (ae != NULL) {
            zip_entry evil = *ae;
            evil.local_offset = 0xFFFFFFF0u;
            check(zip_read_entry(out, (size_t)total, &evil, back,
                                 sizeof(back)) == -1,
                  "huge local_offset rejected");
            evil.local_offset = 0xFFFFFFE2u;
            check(zip_read_entry(out, (size_t)total, &evil, back,
                                 sizeof(back)) == -1,
                  "wrapping local_offset rejected");
        }
        /* EOCD の cd_offset + cd_size が巻き戻る細工も拒否。 */
        static uint8_t evilzip[4096];
        memcpy(evilzip, out, (size_t)total);
        uint8_t *eocd = evilzip + total - 22;
        eocd[12] = 0x40; eocd[13] = 0; eocd[14] = 0; eocd[15] = 0;
        eocd[16] = 0xF0; eocd[17] = 0xFF; eocd[18] = 0xFF;
        eocd[19] = 0xFF;
        check(zip_read_entries(evilzip, (size_t)total, entries,
                               ZIP_MAX_ENTRIES) == -1,
              "wrapping cd_offset+cd_size rejected");
    }

    /* 実物の DEFLATE 項目 (Python の zipfile が圧縮したもの)
     * を読む。 */
    {
        int dn = zip_read_entries(zip_deflated_bin, zip_deflated_bin_len,
                                  entries, ZIP_MAX_ENTRIES);
        check(dn == 2, "deflated zip has 2 entries");
        const zip_entry *de =
            zip_find_entry(entries, dn, "word/document.xml");
        check(de != NULL && de->method == 8, "deflated entry found");
        int dr = zip_read_entry(zip_deflated_bin, zip_deflated_bin_len,
                                de, back, sizeof(back));
        check(dr > 0 && memcmp(back, "<?xml", 5) == 0,
              "deflated entry inflates to xml");
        check(strstr((const char *)back, "Hello PrismOS") != NULL,
              "deflated content has text");
    }

    /* DEFLATE で書いて、読み直す。XML のような繰り返しの多い
     * テキストは圧縮される。 */
    {
        static char xmltext[2048];
        size_t xn = 0;
        for (int i = 0; i < 20; i++) {
            const char *line =
                "<w:p><w:r><w:t>Hello PrismOS</w:t></w:r></w:p>";
            size_t ll = strlen(line);
            memcpy(xmltext + xn, line, ll);
            xn += ll;
        }
        zip_write_begin(&w, out, sizeof(out));
        check(zip_write_add_deflated(&w, "x.xml",
                                     (const uint8_t *)xmltext, xn) == 0,
              "deflated add ok");
        int dt = zip_write_finish(&w);
        check(dt > 0 && dt < 600, "deflated zip is small");
        int dn = zip_read_entries(out, (size_t)dt, entries,
                                  ZIP_MAX_ENTRIES);
        check(dn == 1, "deflated zip has 1 entry");
        const zip_entry *de = zip_find_entry(entries, dn, "x.xml");
        check(de != NULL && de->method == 8, "deflated entry is method 8");
        int dr = zip_read_entry(out, (size_t)dt, de, back, sizeof(back));
        check(dr == (int)xn && memcmp(back, xmltext, xn) == 0,
              "deflated entry reads back");
        /* 縮まない入力は格納に落ちる。 */
        static uint8_t rnd[256];
        uint32_t s = 999;
        for (int i = 0; i < 256; i++) {
            s = s * 1103515245u + 12345u;
            rnd[i] = (uint8_t)(s >> 16);
        }
        zip_write_begin(&w, out, sizeof(out));
        check(zip_write_add_deflated(&w, "rnd.bin", rnd, sizeof(rnd)) ==
                  0,
              "random deflated add ok");
        dt = zip_write_finish(&w);
        dn = zip_read_entries(out, (size_t)dt, entries, ZIP_MAX_ENTRIES);
        de = zip_find_entry(entries, dn, "rnd.bin");
        check(de != NULL && de->method == 0,
              "incompressible falls back to stored");
        dr = zip_read_entry(out, (size_t)dt, de, back, sizeof(back));
        check(dr == 256 && memcmp(back, rnd, 256) == 0,
              "stored fallback reads back");
    }

    printf("zip tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
