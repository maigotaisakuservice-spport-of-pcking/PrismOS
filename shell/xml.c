/* PrismOS — 最小の XML 読み取りヘルパー。 */
#include "xml.h"

#include <string.h>

static int starts_with(const char *s, size_t len, size_t pos,
                       const char *prefix) {
    size_t pl = strlen(prefix);
    if (pos + pl > len) {
        return 0;
    }
    return memcmp(s + pos, prefix, pl) == 0;
}

int xml_decode_char(const char *s, size_t len, size_t pos, size_t *adv) {
    if (pos >= len) {
        return 0;
    }
    if (s[pos] != '&') {
        *adv = 1;
        return (unsigned char)s[pos];
    }
    if (starts_with(s, len, pos, "&amp;")) {
        *adv = 5;
        return '&';
    }
    if (starts_with(s, len, pos, "&lt;")) {
        *adv = 4;
        return '<';
    }
    if (starts_with(s, len, pos, "&gt;")) {
        *adv = 4;
        return '>';
    }
    if (starts_with(s, len, pos, "&quot;")) {
        *adv = 6;
        return '"';
    }
    if (starts_with(s, len, pos, "&apos;")) {
        *adv = 6;
        return '\'';
    }
    if (starts_with(s, len, pos, "&#")) {
        size_t i = pos + 2;
        int hex = 0;
        if (i < len && (s[i] == 'x' || s[i] == 'X')) {
            hex = 1;
            i++;
        }
        int v = 0;
        int digits = 0;
        while (i < len && s[i] != ';' && digits < 6) {
            int d;
            if (s[i] >= '0' && s[i] <= '9') {
                d = s[i] - '0';
            } else if (hex && s[i] >= 'a' && s[i] <= 'f') {
                d = s[i] - 'a' + 10;
            } else if (hex && s[i] >= 'A' && s[i] <= 'F') {
                d = s[i] - 'A' + 10;
            } else {
                return 0;
            }
            v = v * (hex ? 16 : 10) + d;
            digits++;
            i++;
        }
        if (i >= len || s[i] != ';' || digits == 0 || v > 0x10FFFF) {
            return 0;
        }
        *adv = i - pos + 1;
        return v;
    }
    return 0;
}

size_t xml_read_text(const char *s, size_t len, size_t pos,
                     const char *close_tag, char *out, size_t *out_len,
                     size_t cap) {
    size_t close_len = strlen(close_tag);
    while (pos < len) {
        if (pos + close_len <= len &&
            memcmp(s + pos, close_tag, close_len) == 0) {
            return pos + close_len;
        }
        if (s[pos] == '<') {
            return (size_t)-1; /* 閉じタグ無し */
        }
        if (s[pos] != '&') {
            /* 生のバイト (UTF-8 を含む) はそのまま写す。 */
            if (*out_len + 1 >= cap) {
                return (size_t)-1;
            }
            out[(*out_len)++] = s[pos++];
            continue;
        }
        size_t adv = 0;
        int c = xml_decode_char(s, len, pos, &adv);
        if (c == 0) {
            return (size_t)-1;
        }
        if (c < 128) {
            if (*out_len + 1 >= cap) {
                return (size_t)-1;
            }
            out[(*out_len)++] = (char)c;
        } else {
            /* 非 ASCII は UTF-8 に戻して書く。 */
            char utf8[4];
            int n = 0;
            if (c < 0x800) {
                utf8[n++] = (char)(0xC0 | (c >> 6));
                utf8[n++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                utf8[n++] = (char)(0xE0 | (c >> 12));
                utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[n++] = (char)(0x80 | (c & 0x3F));
            } else {
                utf8[n++] = (char)(0xF0 | (c >> 18));
                utf8[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
                utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[n++] = (char)(0x80 | (c & 0x3F));
            }
            if (*out_len + (size_t)n >= cap) {
                return (size_t)-1;
            }
            memcpy(out + *out_len, utf8, (size_t)n);
            *out_len += (size_t)n;
        }
        pos += adv;
    }
    return (size_t)-1;
}

int xml_escape_text(const char *s, size_t len, char *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        const char *rep = NULL;
        if (s[i] == '&') {
            rep = "&amp;";
        } else if (s[i] == '<') {
            rep = "&lt;";
        } else if (s[i] == '>') {
            rep = "&gt;";
        } else if (s[i] == '"') {
            rep = "&quot;";
        }
        if (rep != NULL) {
            size_t rlen = strlen(rep);
            if (n + rlen >= cap) {
                return -1;
            }
            memcpy(out + n, rep, rlen);
            n += rlen;
        } else {
            if (n + 1 >= cap) {
                return -1;
            }
            out[n++] = s[i];
        }
    }
    out[n] = '\0';
    return (int)n;
}
