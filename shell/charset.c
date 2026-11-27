/* PrismOS — 文字コード変換 (charset.h)。 */
#include "charset.h"

#include "charset_tables.h"
#include "string.h"

static uint32_t map_lookup(const uint32_t *map, size_t count,
                           uint32_t key) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t k = map[mid] >> 16;
        if (k == key) {
            return map[mid] & 0xFFFF;
        }
        if (k < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static size_t utf8_put(char *out, size_t cap, size_t pos,
                       uint32_t cp) {
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    }
    if (pos + n > cap) {
        return (size_t)-1;
    }
    memcpy(out + pos, tmp, n);
    return pos + n;
}

static size_t put_cp(char *out, size_t cap, size_t pos, uint32_t cp) {
    size_t r = utf8_put(out, cap, pos, cp);
    return r == (size_t)-1 ? pos : r;
}

charset_kind charset_from_name(const char *name) {
    if (name == NULL) {
        return CHARSET_UTF8;
    }
    char buf[24];
    size_t i = 0;
    while (name[i] != '\0' && i + 1 < sizeof(buf)) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        buf[i] = c;
        i++;
    }
    buf[i] = '\0';
    if (strcmp(buf, "shift_jis") == 0 ||
        strcmp(buf, "shift-jis") == 0 || strcmp(buf, "sjis") == 0 ||
        strcmp(buf, "x-sjis") == 0 ||
        strcmp(buf, "windows-31j") == 0) {
        return CHARSET_SJIS;
    }
    if (strcmp(buf, "euc-jp") == 0 || strcmp(buf, "eucjp") == 0) {
        return CHARSET_EUCJP;
    }
    if (strcmp(buf, "iso-2022-jp") == 0 ||
        strcmp(buf, "iso2022jp") == 0) {
        return CHARSET_ISO2022JP;
    }
    return CHARSET_UTF8;
}

charset_kind charset_detect(const uint8_t *data, size_t len) {
    int has_high = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0x1B && i + 2 < len && data[i + 1] == '$') {
            return CHARSET_ISO2022JP;
        }
        if (data[i] >= 0x80) {
            has_high = 1;
        }
    }
    if (!has_high) {
        return CHARSET_ASCII;
    }
    /* UTF-8 として妥当か。 */
    int utf8_ok = 1;
    for (size_t i = 0; i < len;) {
        uint8_t c = data[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        int need;
        if ((c & 0xE0) == 0xC0) {
            need = 1;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
        } else {
            utf8_ok = 0;
            break;
        }
        for (int k = 1; k <= need; k++) {
            if (i + (size_t)k >= len ||
                (data[i + k] & 0xC0) != 0x80) {
                utf8_ok = 0;
                break;
            }
        }
        if (!utf8_ok) {
            break;
        }
        i += (size_t)need + 1;
    }
    if (utf8_ok) {
        return CHARSET_UTF8;
    }
    /* SJIS と EUC の見分け: SJIS の 2 バイト目は
     * 0x40-0xFC、EUC は両バイトとも 0xA1-0xFE。
     * 0x40-0xA0 の 2 バイト目が出たら SJIS とみなす。 */
    for (size_t i = 0; i + 1 < len; i++) {
        uint8_t a = data[i], b = data[i + 1];
        int sjis_lead = (a >= 0x81 && a <= 0x9F) ||
                        (a >= 0xE0 && a <= 0xFC);
        if (sjis_lead && b >= 0x40 && b <= 0xA0 && b != 0x7F) {
            return CHARSET_SJIS;
        }
    }
    /* 半角カタカナ (0xA1-0xDF の単独) が多ければ SJIS。 */
    int kana = 0, euc_pairs = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] >= 0xA1 && data[i] <= 0xDF) {
            kana++;
        }
        if (i + 1 < len && data[i] >= 0xA1 && data[i + 1] >= 0xA1) {
            euc_pairs++;
        }
    }
    return kana > euc_pairs ? CHARSET_SJIS : CHARSET_EUCJP;
}

size_t charset_to_utf8(charset_kind kind, const uint8_t *in,
                       size_t in_len, char *out, size_t cap) {
    if (kind == CHARSET_UTF8 || kind == CHARSET_ASCII) {
        size_t n = in_len < cap ? in_len : cap;
        memcpy(out, in, n);
        return n;
    }
    size_t pos = 0;
    if (kind == CHARSET_SJIS) {
        for (size_t i = 0; i < in_len;) {
            uint8_t c = in[i];
            if (c < 0x80) {
                pos = put_cp(out, cap, pos, c);
                i++;
            } else if (c >= 0xA1 && c <= 0xDF) {
                pos = put_cp(out, cap, pos,
                             0xFF61 + (uint32_t)(c - 0xA1));
                i++;
            } else if (i + 1 < in_len) {
                uint32_t cp = map_lookup(
                    SJIS_map, SJIS_MAP_LEN,
                    ((uint32_t)c << 8) | in[i + 1]);
                pos = put_cp(out, cap, pos, cp ? cp : 0xFFFD);
                i += 2;
            } else {
                pos = put_cp(out, cap, pos, 0xFFFD);
                i++;
            }
            if (pos >= cap) {
                break;
            }
        }
        return pos;
    }
    if (kind == CHARSET_EUCJP) {
        for (size_t i = 0; i < in_len;) {
            uint8_t c = in[i];
            if (c < 0x80) {
                pos = put_cp(out, cap, pos, c);
                i++;
            } else if (c == 0x8E && i + 1 < in_len) {
                uint8_t k = in[i + 1];
                if (k >= 0xA1 && k <= 0xDF) {
                    pos = put_cp(out, cap, pos,
                                 0xFF61 + (uint32_t)(k - 0xA1));
                } else {
                    pos = put_cp(out, cap, pos, 0xFFFD);
                }
                i += 2;
            } else if (c == 0x8F && i + 2 < in_len) {
                pos = put_cp(out, cap, pos, 0xFFFD);
                i += 3; /* JIS X 0212 は未対応 */
            } else if (i + 1 < in_len) {
                uint32_t cp = map_lookup(
                    EUCJP_map, EUCJP_MAP_LEN,
                    ((uint32_t)c << 8) | in[i + 1]);
                pos = put_cp(out, cap, pos, cp ? cp : 0xFFFD);
                i += 2;
            } else {
                pos = put_cp(out, cap, pos, 0xFFFD);
                i++;
            }
            if (pos >= cap) {
                break;
            }
        }
        return pos;
    }
    /* ISO-2022-JP: エスケープで状態を切り替える。 */
    int jis_mode = 0;
    for (size_t i = 0; i < in_len;) {
        uint8_t c = in[i];
        if (c == 0x1B && i + 2 < in_len && in[i + 1] == '$' &&
            (in[i + 2] == '@' || in[i + 2] == 'B')) {
            jis_mode = 1;
            i += 3;
            continue;
        }
        if (c == 0x1B && i + 2 < in_len && in[i + 1] == '(') {
            jis_mode = 0;
            i += 3;
            continue;
        }
        if (jis_mode && i + 1 < in_len && c >= 0x21 && c <= 0x7E &&
            in[i + 1] >= 0x21 && in[i + 1] <= 0x7E) {
            uint32_t cp = map_lookup(JIS_map, JIS_MAP_LEN,
                                     ((uint32_t)c << 8) | in[i + 1]);
            pos = put_cp(out, cap, pos, cp ? cp : 0xFFFD);
            i += 2;
        } else if (!jis_mode && c < 0x80) {
            pos = put_cp(out, cap, pos, c);
            i++;
        } else {
            pos = put_cp(out, cap, pos, 0xFFFD);
            i++;
        }
        if (pos >= cap) {
            break;
        }
    }
    return pos;
}
