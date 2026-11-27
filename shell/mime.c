/* PrismOS - MIME support. See mime.h. */

#include "mime.h"
#include "charset.h"

#include "string.h"

/* ---------- base64 ---------- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int mime_b64_encode(const uint8_t *in, size_t len, char *out,
                    size_t cap, int wrap) {
    size_t p = 0;
    int col = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int have2 = i + 1 < len;
        int have3 = i + 2 < len;
        if (have2) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (have3) {
            v |= in[i + 2];
        }
        char quad[4];
        quad[0] = B64[(v >> 18) & 63];
        quad[1] = B64[(v >> 12) & 63];
        quad[2] = have2 ? B64[(v >> 6) & 63] : '=';
        quad[3] = have3 ? B64[v & 63] : '=';
        for (int k = 0; k < 4; k++) {
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = quad[k];
            col++;
            if (wrap && col == 76 && i + 3 < len) {
                if (p + 2 >= cap) {
                    return -1;
                }
                out[p++] = '\r';
                out[p++] = '\n';
                col = 0;
            }
        }
    }
    if (p >= cap) {
        return -1;
    }
    out[p] = '\0';
    return (int)p;
}

static int b64_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

int mime_b64_decode(const char *in, size_t len, uint8_t *out,
                    size_t cap) {
    size_t p = 0;
    uint32_t acc = 0;
    int nacc = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = in[i];
        if (ch == '=') {
            break;
        }
        int v = b64_value(ch);
        if (v < 0) {
            continue; /* whitespace and stray chars */
        }
        acc = (acc << 6) | (uint32_t)v;
        nacc++;
        if (nacc == 4) {
            if (p + 3 > cap) {
                return -1;
            }
            out[p++] = (uint8_t)(acc >> 16);
            out[p++] = (uint8_t)(acc >> 8);
            out[p++] = (uint8_t)acc;
            acc = 0;
            nacc = 0;
        }
    }
    if (nacc == 2) {
        if (p + 1 > cap) {
            return -1;
        }
        out[p++] = (uint8_t)(acc >> 4);
    } else if (nacc == 3) {
        if (p + 2 > cap) {
            return -1;
        }
        out[p++] = (uint8_t)(acc >> 10);
        out[p++] = (uint8_t)(acc >> 2);
    } else if (nacc == 1) {
        return -1;
    }
    return (int)p;
}

/* ---------- quoted-printable ---------- */

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

int mime_qp_decode(const char *in, size_t len, uint8_t *out,
                   size_t cap) {
    size_t p = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b;
        if (in[i] == '=') {
            if (i + 1 < len && in[i + 1] == '\r' &&
                i + 2 < len && in[i + 2] == '\n') {
                i += 2; /* soft line break */
                continue;
            }
            if (i + 1 < len && in[i + 1] == '\n') {
                i += 1;
                continue;
            }
            if (i + 2 >= len) {
                return -1;
            }
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            b = (uint8_t)(hi * 16 + lo);
            i += 2;
        } else {
            b = (uint8_t)in[i];
        }
        if (p + 1 > cap) {
            return -1;
        }
        out[p++] = b;
    }
    return (int)p;
}

int mime_decode_body(const char *encoding, const char *in,
                     size_t len, uint8_t *out, size_t cap) {
    if (encoding != NULL) {
        if (strcmp(encoding, "base64") == 0) {
            return mime_b64_decode(in, len, out, cap);
        }
        if (strcmp(encoding, "quoted-printable") == 0) {
            return mime_qp_decode(in, len, out, cap);
        }
    }
    if (len > cap) {
        return -1;
    }
    memcpy(out, in, len);
    return (int)len;
}

/* ---------- headers ---------- */

/* Offset just past the header block (after the blank
 * line that separates headers from the body). */
static size_t header_block_len(const char *msg, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (msg[i] == '\n' && msg[i + 1] == '\n') {
            return i + 2;
        }
        if (i + 3 < len && msg[i] == '\r' && msg[i + 1] == '\n' &&
            msg[i + 2] == '\r' && msg[i + 3] == '\n') {
            return i + 4;
        }
    }
    return len;
}

int mime_header(const char *msg, size_t len, const char *field,
                char *out, size_t cap) {
    size_t hlen = header_block_len(msg, len);
    size_t flen = strlen(field);
    size_t i = 0;
    while (i < hlen) {
        size_t ls = i;
        while (i < hlen && msg[i] != '\n') {
            i++;
        }
        size_t le = i; /* line end (before \n) */
        i++;
        if (le > ls && msg[le - 1] == '\r') {
            le--;
        }
        if (le - ls > flen && memcmp(msg + ls, field, flen) == 0 &&
            msg[ls + flen] == ':') {
            /* Value: after ':', unfolded (continuation
             * lines start with space/tab). */
            size_t p = 0;
            size_t v = ls + flen + 1;
            while (v < le && (msg[v] == ' ' || msg[v] == '\t')) {
                v++;
            }
            for (size_t k = v; k < le; k++) {
                if (p + 1 < cap) {
                    out[p++] = msg[k];
                }
            }
            /* Continuation lines. */
            size_t ci = i;
            while (ci < hlen && (msg[ci] == ' ' || msg[ci] == '\t')) {
                size_t cs = ci;
                while (ci < hlen && msg[ci] != '\n') {
                    ci++;
                }
                size_t ce = ci;
                ci++;
                if (ce > cs && msg[ce - 1] == '\r') {
                    ce--;
                }
                if (p + 1 < cap) {
                    out[p++] = ' ';
                }
                for (size_t k = cs; k < ce; k++) {
                    if (msg[k] != ' ' && msg[k] != '\t') {
                        for (size_t m = k; m < ce; m++) {
                            if (p + 1 < cap) {
                                out[p++] = msg[m];
                            }
                        }
                        break;
                    }
                }
            }
            out[p < cap ? p : cap - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

int mime_param(const char *header_value, const char *param,
               char *out, size_t cap) {
    size_t pl = strlen(param);
    const char *p = header_value;
    while (*p != '\0') {
        if (strncmp(p, param, pl) == 0 && p[pl] == '=') {
            p += pl + 1;
            if (*p == '"') {
                p++;
                size_t n = 0;
                while (*p != '\0' && *p != '"' && n + 1 < cap) {
                    out[n++] = *p++;
                }
                out[n] = '\0';
                return 0;
            }
            size_t n = 0;
            while (*p != '\0' && *p != ';' && *p != ' ' &&
                   *p != '\t' && n + 1 < cap) {
                out[n++] = *p++;
            }
            out[n] = '\0';
            return 0;
        }
        p++;
    }
    return -1;
}

/* ---------- encoded words ---------- */

void mime_decode_header(const char *in, char *out, size_t cap) {
    size_t p = 0;
    size_t i = 0;
    while (in[i] != '\0') {
        if (in[i] == '=' && in[i + 1] == '?') {
            /* =?charset?X?text?= */
            size_t q1 = i + 2;
            size_t cs_end = q1;
            while (in[cs_end] != '\0' && in[cs_end] != '?') {
                cs_end++;
            }
            if (in[cs_end] == '?' && in[cs_end + 1] != '\0' &&
                in[cs_end + 2] == '?') {
                char enc = in[cs_end + 1];
                size_t ts = cs_end + 3;
                size_t te = ts;
                while (in[te] != '\0' &&
                       !(in[te] == '?' && in[te + 1] == '=')) {
                    te++;
                }
                if (in[te] == '?' && in[te + 1] == '=' &&
                    cs_end - q1 <= 23) {
                    uint8_t dec[256];
                    int dn = -1;
                    if (enc == 'B' || enc == 'b') {
                        dn = mime_b64_decode(in + ts, te - ts, dec,
                                             sizeof(dec) - 1);
                    } else if (enc == 'Q' || enc == 'q') {
                        /* Q-encoding: '_' is a space. */
                        char tmp[256];
                        size_t tn = 0;
                        for (size_t k = ts; k < te &&
                                            tn + 1 < sizeof(tmp);
                             k++) {
                            tmp[tn++] = in[k] == '_' ? ' ' : in[k];
                        }
                        tmp[tn] = '\0';
                        dn = mime_qp_decode(tmp, tn, dec,
                                            sizeof(dec) - 1);
                    }
                    if (dn >= 0) {
                        /* UTF-8 以外 (Shift_JIS 等) は
                         * UTF-8 へ変換してから出す。 */
                        char csname[24];
                        size_t cn = cs_end - q1;
                        if (cn + 1 > sizeof(csname)) {
                            cn = sizeof(csname) - 1;
                        }
                        for (size_t k = 0; k < cn; k++) {
                            csname[k] = in[q1 + k];
                        }
                        csname[cn] = '\0';
                        p += charset_to_utf8(
                            charset_from_name(csname), dec,
                            (size_t)dn, out + p,
                            p < cap ? cap - p : 0);
                        i = te + 2;
                        /* Whitespace between two encoded
                         * words is dropped (RFC 2047). */
                        size_t save = i;
                        while (in[i] == ' ' || in[i] == '\t') {
                            i++;
                        }
                        if (!(in[i] == '=' && in[i + 1] == '?')) {
                            i = save;
                        }
                        continue;
                    }
                }
            }
        }
        if (p + 1 < cap) {
            out[p++] = in[i];
        }
        i++;
    }
    out[p < cap ? p : cap - 1] = '\0';
}

/* ---------- multipart walking ---------- */

/* A part view into the message. */
typedef struct {
    size_t hdr_off;
    size_t hdr_len;
    size_t body_off;
    size_t body_len;
} mime_part;

/* Find the next part in a multipart body. boundary
 * includes the leading "--". state: *cursor is the
 * search offset. Returns 1 when a part was found. */
static int next_part(const char *msg, size_t len,
                     const char *boundary, size_t *cursor,
                     mime_part *part) {
    size_t bl = strlen(boundary);
    size_t i = *cursor;
    while (i + bl <= len) {
        /* A boundary must start at a line start. */
        if ((i == 0 || msg[i - 1] == '\n') &&
            memcmp(msg + i, boundary, bl) == 0) {
            size_t after = i + bl;
            if (after + 1 < len && msg[after] == '-' &&
                msg[after + 1] == '-') {
                return 0; /* closing boundary */
            }
            /* Skip to the end of the boundary line. */
            size_t hs = after;
            while (hs < len && msg[hs] != '\n') {
                hs++;
            }
            if (hs >= len) {
                /* 境界行のまま入力が終わっている。hs を
                 * 進めると len - hs が下溢れし、バッファの
                 * 外を読んでしまう (ファジングで検出)。 */
                return 0;
            }
            hs++;
            size_t hlen = header_block_len(msg + hs, len - hs);
            part->hdr_off = hs;
            part->hdr_len = hlen;
            part->body_off = hs + hlen;
            /* Body ends at the next boundary line. */
            size_t j = part->body_off;
            size_t body_end = len;
            while (j + bl <= len) {
                if ((j == 0 || msg[j - 1] == '\n') &&
                    memcmp(msg + j, boundary, bl) == 0) {
                    body_end = j;
                    break;
                }
                j++;
            }
            /* Trim the CRLF that precedes the boundary. */
            if (body_end >= part->body_off + 2 &&
                msg[body_end - 2] == '\r' &&
                msg[body_end - 1] == '\n') {
                body_end -= 2;
            } else if (body_end >= part->body_off + 1 &&
                       msg[body_end - 1] == '\n') {
                body_end -= 1;
            }
            part->body_len = body_end - part->body_off;
            *cursor = body_end;
            return 1;
        }
        i++;
    }
    return 0;
}

static void part_header(const char *msg, const mime_part *part,
                        const char *field, char *out, size_t cap) {
    out[0] = '\0';
    /* mime_header works on a header block; the part's
     * headers end at its blank line by construction. */
    char save = 0;
    size_t end = part->hdr_off + part->hdr_len;
    if (end < 65536) {
        /* mime_header scans for the blank line itself;
         * the part slice is already exactly that. */
        (void)save;
        if (mime_header(msg + part->hdr_off, part->hdr_len,
                        field, out, cap) != 0) {
            out[0] = '\0';
        }
    }
}


/* 転送デコード済みの本文を、そのパートの charset に
 * 従って UTF-8 へ変換する (charset.h)。 */
static int convert_body(const char *ctype, char *out, int dn,
                        size_t cap) {
    char cs[32];
    if (mime_param(ctype, "charset", cs, sizeof(cs)) != 0) {
        return dn;
    }
    charset_kind k = charset_from_name(cs);
    if (k == CHARSET_UTF8 || k == CHARSET_ASCII) {
        return dn;
    }
    static uint8_t tmp[16384];
    if (dn < 0 || (size_t)dn > sizeof(tmp)) {
        return dn;
    }
    memcpy(tmp, out, (size_t)dn);
    size_t w = charset_to_utf8(k, tmp, (size_t)dn, out, cap - 1);
    out[w] = '\0';
    return (int)w;
}

int mime_text_body(const char *msg, size_t len, char *out,
                   size_t cap) {
    char ctype[128];
    if (mime_header(msg, len, "Content-Type", ctype,
                    sizeof(ctype)) != 0) {
        ctype[0] = '\0';
    }
    char boundary[80];
    if (mime_param(ctype, "boundary", boundary,
                   sizeof(boundary)) == 0) {
        char bnd[84];
        bnd[0] = '-';
        bnd[1] = '-';
        size_t n = 0;
        while (boundary[n] != '\0' && n + 3 < sizeof(bnd)) {
            bnd[2 + n] = boundary[n];
            n++;
        }
        bnd[2 + n] = '\0';
        size_t cursor = header_block_len(msg, len);
        mime_part part;
        while (next_part(msg, len, bnd, &cursor, &part)) {
            char pct[96];
            part_header(msg, &part, "Content-Type", pct,
                        sizeof(pct));
            if (pct[0] == '\0' ||
                strncmp(pct, "text/plain", 10) == 0) {
                char enc[48];
                part_header(msg, &part,
                            "Content-Transfer-Encoding", enc,
                            sizeof(enc));
                int dn = mime_decode_body(
                    enc[0] ? enc : NULL, msg + part.body_off,
                    part.body_len, (uint8_t *)out, cap - 1);
                if (dn < 0) {
                    return -1;
                }
                out[dn] = '\0';
                return convert_body(pct, out, dn, cap);
            }
        }
        return -1;
    }
    /* Single part: decode the body after the headers. */
    size_t hoff = header_block_len(msg, len);
    char enc[48];
    if (mime_header(msg, len, "Content-Transfer-Encoding",
                    enc, sizeof(enc)) != 0) {
        enc[0] = '\0';
    }
    int dn = mime_decode_body(enc[0] ? enc : NULL, msg + hoff,
                              len - hoff, (uint8_t *)out,
                              cap - 1);
    if (dn < 0) {
        return -1;
    }
    out[dn] = '\0';
    return convert_body(ctype, out, dn, cap);
}

/* Sanitise a filename to a VFS-safe base name. */
static void sanitise_name(const char *in, char *out,
                          size_t cap) {
    /* Strip any directory part. */
    const char *base = in;
    for (const char *p = in; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t n = 0;
    for (const char *p = base; *p != '\0' && n + 1 < cap; p++) {
        char ch = *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
            ch == '_') {
            out[n++] = ch;
        } else if (ch == ' ') {
            out[n++] = '_';
        }
    }
    out[n] = '\0';
    if (n == 0) {
        const char *dflt = "attachment.bin";
        size_t i = 0;
        while (dflt[i] != '\0' && i + 1 < cap) {
            out[i] = dflt[i];
            i++;
        }
        out[i] = '\0';
    }
}

int mime_next_attachment(const char *msg, size_t len,
                         size_t *cursor, char *name,
                         size_t name_cap, uint8_t *data,
                         size_t data_cap, size_t *data_len) {
    char ctype[128];
    if (mime_header(msg, len, "Content-Type", ctype,
                    sizeof(ctype)) != 0) {
        return 0;
    }
    char boundary[80];
    if (mime_param(ctype, "boundary", boundary,
                   sizeof(boundary)) != 0) {
        return 0;
    }
    char bnd[84];
    bnd[0] = '-';
    bnd[1] = '-';
    size_t n = 0;
    while (boundary[n] != '\0' && n + 3 < sizeof(bnd)) {
        bnd[2 + n] = boundary[n];
        n++;
    }
    bnd[2 + n] = '\0';
    if (*cursor == 0) {
        *cursor = header_block_len(msg, len);
    }
    mime_part part;
    while (next_part(msg, len, bnd, cursor, &part)) {
        char disp[160];
        part_header(msg, &part, "Content-Disposition", disp,
                    sizeof(disp));
        char fname[96];
        int is_att = strncmp(disp, "attachment", 10) == 0;
        if (mime_param(disp, "filename", fname,
                       sizeof(fname)) != 0) {
            char pct[96];
            part_header(msg, &part, "Content-Type", pct,
                        sizeof(pct));
            if (mime_param(pct, "name", fname,
                           sizeof(fname)) != 0) {
                fname[0] = '\0';
            }
        }
        if (!is_att && fname[0] == '\0') {
            continue; /* body part */
        }
        if (!is_att) {
            /* A named non-text part also counts. */
            char pct[96];
            part_header(msg, &part, "Content-Type", pct,
                        sizeof(pct));
            if (strncmp(pct, "text/plain", 10) == 0) {
                continue;
            }
        }
        char enc[48];
        part_header(msg, &part, "Content-Transfer-Encoding",
                    enc, sizeof(enc));
        int dn = mime_decode_body(enc[0] ? enc : NULL,
                                  msg + part.body_off,
                                  part.body_len, data,
                                  data_cap);
        if (dn < 0) {
            return -1;
        }
        *data_len = (size_t)dn;
        sanitise_name(fname[0] ? fname : "attachment.bin",
                      name, name_cap);
        return 1;
    }
    return 0;
}
