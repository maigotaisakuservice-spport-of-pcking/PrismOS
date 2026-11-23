/* PrismOS — PDF テキスト抽出と書き出し (pdf.h)。 */
#include "pdf.h"

#include "inflate.h"
#include "string.h"

static int is_pdf(const uint8_t *data, size_t len) {
    return len >= 5 && memcmp(data, "%PDF-", 5) == 0;
}

/* data[lo..hi) に needle があるか。 */
static int region_has(const uint8_t *data, size_t lo, size_t hi,
                      const char *needle) {
    size_t nl = 0;
    while (needle[nl] != '\0') {
        nl++;
    }
    if (hi - lo < nl) {
        return 0;
    }
    for (size_t i = lo; i + nl <= hi; i++) {
        if (memcmp(data + i, needle, nl) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 出力へ 1 バイト (容量チェック付き)。 */
static int out_ch(char *out, size_t cap, size_t *pos, char c) {
    if (*pos + 1 >= cap) {
        return -1;
    }
    out[(*pos)++] = c;
    return 0;
}

/* リテラル文字列 ( ... ) を out へ展開する。
 * *cursor は '(' の位置。終わりの ')' の次を返す。 */
static void emit_literal(const uint8_t *s, size_t len,
                         size_t *cursor, char *out, size_t cap,
                         size_t *pos) {
    size_t i = *cursor + 1;
    int depth = 1;
    while (i < len && depth > 0) {
        uint8_t c = s[i];
        if (c == '\\' && i + 1 < len) {
            uint8_t e = s[i + 1];
            if (e >= '0' && e <= '7') {
                int v = 0;
                int n = 0;
                while (n < 3 && i + 1 < len && s[i + 1] >= '0' &&
                       s[i + 1] <= '7') {
                    v = v * 8 + (s[i + 1] - '0');
                    i++;
                    n++;
                }
                out_ch(out, cap, pos, (char)v);
                i++;
                continue;
            }
            char rep = (char)e;
            if (e == 'n') {
                rep = '\n';
            } else if (e == 'r') {
                rep = '\r';
            } else if (e == 't') {
                rep = '\t';
            }
            out_ch(out, cap, pos, rep);
            i += 2;
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                i++;
                break;
            }
        }
        out_ch(out, cap, pos, (char)c);
        i++;
    }
    *cursor = i;
}

/* 16 進文字列 < ... > を展開する。 */
static void emit_hex(const uint8_t *s, size_t len, size_t *cursor,
                     char *out, size_t cap, size_t *pos) {
    size_t i = *cursor + 1;
    int hi = -1;
    while (i < len && s[i] != '>') {
        uint8_t c = s[i];
        int v = -1;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        }
        if (v >= 0) {
            if (hi < 0) {
                hi = v;
            } else {
                out_ch(out, cap, pos, (char)(hi * 16 + v));
                hi = -1;
            }
        }
        i++;
    }
    if (hi >= 0) {
        out_ch(out, cap, pos, (char)(hi * 16));
    }
    *cursor = i < len ? i + 1 : i;
}

/* 内容ストリームからテキスト演算子を拾う。 */
static void extract_from_stream(const uint8_t *s, size_t len,
                                char *out, size_t cap,
                                size_t *pos) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c == '(') {
            /* 直後 (空白を除く) が Tj/TJ/' 対象かは
             * 気にせず、文字列はすべて拾い、Tj 系の
             * 後では区切りを入れない。 */
            emit_literal(s, len, &i, out, cap, pos);
            continue;
        }
        if (c == '<' && i + 1 < len && s[i + 1] != '<') {
            emit_hex(s, len, &i, out, cap, pos);
            continue;
        }
        if ((c == 'T' && i + 1 < len &&
             (s[i + 1] == 'd' || s[i + 1] == 'D' || s[i + 1] == '*')) ||
            (c == '\'' || c == '"')) {
            out_ch(out, cap, pos, '\n');
            i++;
            continue;
        }
        i++;
    }
}

int pdf_extract_text(const uint8_t *data, size_t len, char *out,
                     size_t cap) {
    if (!is_pdf(data, len) || cap < 2) {
        return -1;
    }
    size_t pos = 0;
    size_t i = 0;
    static uint8_t inflated[32768];
    while (i < len) {
        /* "stream" キーワードを探す。 */
        if (i + 6 <= len && memcmp(data + i, "stream", 6) == 0 &&
            (i == 0 || data[i - 1] == '\n' || data[i - 1] == '\r' ||
             data[i - 1] == ' ')) {
            size_t start = i + 6;
            if (start < len && data[start] == '\r') {
                start++;
            }
            if (start < len && data[start] == '\n') {
                start++;
            }
            size_t end = start;
            size_t found = (size_t)-1;
            while (end + 9 <= len) {
                if (memcmp(data + end, "endstream", 9) == 0) {
                    found = end;
                    break;
                }
                end++;
            }
            if (found == (size_t)-1) {
                break;
            }
            /* 直前の辞書 (512 バイト以内) に FlateDecode。 */
            size_t lo = i > 512 ? i - 512 : 0;
            if (region_has(data, lo, i, "FlateDecode")) {
                if (found - start > 2) {
                    int n = inflate_decompress(
                        data + start + 2, found - start - 2,
                        inflated, sizeof(inflated));
                    if (n > 0) {
                        extract_from_stream(inflated, (size_t)n,
                                            out, cap, &pos);
                    }
                }
            } else {
                extract_from_stream(data + start, found - start,
                                    out, cap, &pos);
            }
            if (pos > 0 && pos + 1 < cap && out[pos - 1] != '\n') {
                out[pos++] = '\n';
            }
            i = found + 9;
            continue;
        }
        i++;
    }
    out[pos] = '\0';
    return (int)pos;
}

/* ---------- 書き出し ---------- */

static size_t app_str(uint8_t *out, size_t cap, size_t pos,
                      const char *s) {
    while (*s != '\0' && pos < cap) {
        out[pos++] = (uint8_t)*s++;
    }
    return pos;
}

static size_t app_uint(uint8_t *out, size_t cap, size_t pos,
                       size_t v) {
    char tmp[20];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && pos < cap) {
        out[pos++] = (uint8_t)tmp[--n];
    }
    return pos;
}

int pdf_build(const char *text, size_t len, uint8_t *out,
              size_t cap) {
    /* 内容ストリームを先に組み立てる。 */
    static uint8_t content[16384];
    size_t cp = 0;
    cp = app_str(content, sizeof(content), cp,
                 "BT /F1 12 Tf 14.4 TL 56 756 Td ");
    size_t i = 0;
    int line_start = 1;
    while (i < len) {
        if (!line_start) {
            /* 行の途中 */
        }
        /* 1 行分を ( ... ) Tj T* で出す。 */
        size_t ls = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        if (i > ls || line_start) {
            cp = app_str(content, sizeof(content), cp, "(");
            for (size_t k = ls; k < i; k++) {
                char c = text[k];
                if (c == '(' || c == ')' || c == '\\') {
                    if (cp + 1 < sizeof(content)) {
                        content[cp++] = '\\';
                    }
                    if (cp < sizeof(content)) {
                        content[cp++] = (uint8_t)c;
                    }
                } else if (c >= 32 && c < 127) {
                    if (cp < sizeof(content)) {
                        content[cp++] = (uint8_t)c;
                    }
                } else if (c == '\t') {
                    if (cp < sizeof(content)) {
                        content[cp++] = ' ';
                    }
                } else {
                    if (cp < sizeof(content)) {
                        content[cp++] = '?';
                    }
                }
            }
            cp = app_str(content, sizeof(content), cp, ") Tj T* ");
        }
        line_start = 0;
        if (i < len && text[i] == '\n') {
            i++;
        }
        if (cp > sizeof(content) - 64) {
            break;
        }
    }
    cp = app_str(content, sizeof(content), cp, "ET");

    size_t pos = 0;
    size_t off1, off2, off3, off4, off5;
    pos = app_str(out, cap, pos, "%PDF-1.4\n");
    off1 = pos;
    pos = app_str(out, cap, pos,
                  "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\n"
                  "endobj\n");
    off2 = pos;
    pos = app_str(out, cap, pos,
                  "2 0 obj\n<< /Type /Pages /Kids [3 0 R] "
                  "/Count 1 >>\nendobj\n");
    off3 = pos;
    pos = app_str(out, cap, pos,
                  "3 0 obj\n<< /Type /Page /Parent 2 0 R "
                  "/MediaBox [0 0 612 792] /Contents 4 0 R "
                  "/Resources << /Font << /F1 5 0 R >> >> >>\n"
                  "endobj\n");
    off4 = pos;
    pos = app_str(out, cap, pos, "4 0 obj\n<< /Length ");
    pos = app_uint(out, cap, pos, cp);
    pos = app_str(out, cap, pos, " >>\nstream\n");
    if (pos + cp > cap) {
        return -1;
    }
    for (size_t k = 0; k < cp; k++) {
        out[pos++] = content[k];
    }
    pos = app_str(out, cap, pos, "\nendstream\nendobj\n");
    off5 = pos;
    pos = app_str(out, cap, pos,
                  "5 0 obj\n<< /Type /Font /Subtype /Type1 "
                  "/BaseFont /Helvetica >>\nendobj\n");
    size_t xref = pos;
    pos = app_str(out, cap, pos,
                  "xref\n0 6\n0000000000 65535 f \n");
    size_t offs[5] = { off1, off2, off3, off4, off5 };
    for (int k = 0; k < 5; k++) {
        /* 10 桁ゼロ詰め */
        char num[16];
        size_t v = offs[k];
        for (int d = 9; d >= 0; d--) {
            num[d] = (char)('0' + (v % 10));
            v /= 10;
        }
        num[10] = '\0';
        pos = app_str(out, cap, pos, num);
        pos = app_str(out, cap, pos, " 00000 n \n");
    }
    pos = app_str(out, cap, pos,
                  "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n");
    pos = app_uint(out, cap, pos, xref);
    pos = app_str(out, cap, pos, "\n%%EOF\n");
    if (pos > cap) {
        return -1;
    }
    return (int)pos;
}
