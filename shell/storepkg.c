/* PrismOS — Prism Store パッケージ形式 (storepkg.h)。 */
#include "storepkg.h"

#include "string.h"

static int name_char_ok(char c, int allow_dot) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-') {
        return 1;
    }
    return allow_dot && c == '.';
}

int storepkg_name_ok(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    size_t n = 0;
    while (name[n] != '\0') {
        if (!name_char_ok(name[n], 0) || n >= STOREPKG_NAME_MAX) {
            return 0;
        }
        n++;
    }
    return n > 0;
}

int storepkg_file_ok(const char *file) {
    if (file == NULL || file[0] == '\0' || file[0] == '.') {
        return 0;
    }
    size_t n = 0;
    while (file[n] != '\0') {
        if (!name_char_ok(file[n], 1) || n >= STOREPKG_FILE_MAX) {
            return 0;
        }
        n++;
    }
    return n > 0;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ヘッダから "key=value" を取り出す。 */
static int header_value(const char *hdr, size_t hlen, const char *key,
                        char *out, size_t cap) {
    size_t klen = 0;
    while (key[klen] != '\0') {
        klen++;
    }
    size_t off = 0;
    while (off < hlen) {
        size_t eol = off;
        while (eol < hlen && hdr[eol] != '\n') {
            eol++;
        }
        size_t ll = eol - off;
        if (ll > klen && memcmp(hdr + off, key, klen) == 0 &&
            hdr[off + klen] == '=') {
            size_t vlen = ll - klen - 1;
            if (vlen + 1 > cap) {
                return -1;
            }
            memcpy(out, hdr + off + klen + 1, vlen);
            out[vlen] = '\0';
            return 0;
        }
        off = eol + 1;
    }
    return -1;
}

int storepkg_parse(const uint8_t *data, size_t len,
                   storepkg_info *info, const uint8_t **sig_out,
                   const uint8_t **payload_out) {
    if (data == NULL || info == NULL || len < 16) {
        return -1;
    }
    if (memcmp(data, STOREPKG_MAGIC, 8) != 0) {
        return -1;
    }
    uint32_t hlen = rd32(data + 8);
    uint32_t plen = rd32(data + 12);
    if (hlen == 0 || hlen > 1024) {
        return -1;
    }
    size_t need = 16 + (size_t)hlen + STOREPKG_SIG_LEN + plen;
    if (need > len) {
        return -1;
    }
    const char *hdr = (const char *)(data + 16);
    if (header_value(hdr, hlen, "name", info->name,
                     sizeof(info->name)) != 0 ||
        header_value(hdr, hlen, "file", info->file,
                     sizeof(info->file)) != 0 ||
        header_value(hdr, hlen, "version", info->version,
                     sizeof(info->version)) != 0) {
        return -1;
    }
    if (!storepkg_name_ok(info->name) ||
        !storepkg_file_ok(info->file)) {
        return -1;
    }
    info->header_len = hlen;
    info->payload_len = plen;
    if (sig_out != NULL) {
        *sig_out = data + 16 + hlen;
    }
    if (payload_out != NULL) {
        *payload_out = data + 16 + hlen + STOREPKG_SIG_LEN;
    }
    return 0;
}

int storepkg_build(const char *name, const char *file,
                   const char *version, const uint8_t *payload,
                   size_t payload_len, const uint8_t *sig,
                   uint8_t *out, size_t cap, size_t *out_len) {
    if (!storepkg_name_ok(name) || !storepkg_file_ok(file) ||
        version == NULL || payload == NULL || sig == NULL ||
        out == NULL) {
        return -1;
    }
    char hdr[512];
    size_t p = 0;
    const char *parts[6] = { "name=", name, "\nfile=", file,
                             "\nversion=", version };
    for (int i = 0; i < 6; i += 2) {
        const char *k = parts[i];
        const char *v = parts[i + 1];
        while (*k != '\0' && p + 1 < sizeof(hdr)) {
            hdr[p++] = *k++;
        }
        while (*v != '\0' && p + 1 < sizeof(hdr)) {
            hdr[p++] = *v++;
        }
    }
    if (p + 1 < sizeof(hdr)) {
        hdr[p++] = '\n';
    }
    size_t need = 16 + p + STOREPKG_SIG_LEN + payload_len;
    if (need > cap) {
        return -1;
    }
    memcpy(out, STOREPKG_MAGIC, 8);
    wr32(out + 8, (uint32_t)p);
    wr32(out + 12, (uint32_t)payload_len);
    memcpy(out + 16, hdr, p);
    memcpy(out + 16 + p, sig, STOREPKG_SIG_LEN);
    memcpy(out + 16 + p + STOREPKG_SIG_LEN, payload, payload_len);
    if (out_len != NULL) {
        *out_len = need;
    }
    return 0;
}

/* ---- store-installed.txt ---- */

/* 行が "name " で始まるか (名前の完全一致)。 */
static int line_name_is(const char *line, size_t ll,
                        const char *name) {
    size_t n = 0;
    while (name[n] != '\0') {
        if (n >= ll || line[n] != name[n]) {
            return 0;
        }
        n++;
    }
    return n < ll && line[n] == ' ';
}

int store_installed_contains(const char *text, size_t len,
                             const char *name) {
    if (text == NULL) {
        return 0;
    }
    size_t off = 0;
    while (off < len) {
        size_t eol = off;
        while (eol < len && text[eol] != '\n') {
            eol++;
        }
        if (line_name_is(text + off, eol - off, name)) {
            return 1;
        }
        off = eol + 1;
    }
    return 0;
}

int store_installed_add(const char *text, size_t len,
                        const char *name, const char *version,
                        const char *file, char *out, size_t cap) {
    size_t p = 0;
    if (text != NULL) {
        /* 同名があれば除いてから足す。 */
        size_t off = 0;
        while (off < len) {
            size_t eol = off;
            while (eol < len && text[eol] != '\n') {
                eol++;
            }
            if (!line_name_is(text + off, eol - off, name)) {
                size_t ll = eol - off;
                if (p + ll + 1 >= cap) {
                    return -1;
                }
                memcpy(out + p, text + off, ll);
                p += ll;
                out[p++] = '\n';
            }
            off = eol + 1;
        }
    }
    const char *parts[5] = { name, " ", version, " ", file };
    for (int i = 0; i < 5; i++) {
        const char *s = parts[i];
        while (*s != '\0') {
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = *s++;
        }
    }
    if (p + 1 >= cap) {
        return -1;
    }
    out[p++] = '\n';
    out[p] = '\0';
    return 0;
}

int store_installed_remove(const char *text, size_t len,
                           const char *name, char *out, size_t cap,
                           char *file_out, size_t file_cap) {
    if (text == NULL) {
        return -1;
    }
    size_t p = 0;
    int found = 0;
    size_t off = 0;
    while (off < len) {
        size_t eol = off;
        while (eol < len && text[eol] != '\n') {
            eol++;
        }
        size_t ll = eol - off;
        if (line_name_is(text + off, ll, name)) {
            found = 1;
            if (file_out != NULL) {
                /* 3 番目の単語がファイル名。 */
                size_t i = 0;
                int words = 0;
                size_t start = 0;
                while (i < ll) {
                    while (i < ll && text[off + i] == ' ') {
                        i++;
                    }
                    start = i;
                    while (i < ll && text[off + i] != ' ') {
                        i++;
                    }
                    words++;
                    if (words == 3) {
                        size_t fl = i - start;
                        if (fl + 1 > file_cap) {
                            return -1;
                        }
                        memcpy(file_out, text + off + start, fl);
                        file_out[fl] = '\0';
                    }
                }
            }
        } else {
            if (p + ll + 1 >= cap) {
                return -1;
            }
            memcpy(out + p, text + off, ll);
            p += ll;
            out[p++] = '\n';
        }
        off = eol + 1;
    }
    if (p >= cap) {
        return -1;
    }
    out[p] = '\0';
    return found ? 0 : -1;
}
