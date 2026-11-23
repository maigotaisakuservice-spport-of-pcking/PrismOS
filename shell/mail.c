/* PrismOS - mail (pure logic). See mail.h. */
#include "mail.h"

#include "mime.h"

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static void copy_capped(char *dst, size_t cap, const char *src,
                        size_t len) {
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

void mail_config_defaults(mail_config *c) {
    c->smtp_host[0] = '\0';
    c->smtp_port = 0;
    c->pop3_host[0] = '\0';
    c->pop3_port = 0;
    c->imap_host[0] = '\0';
    c->imap_port = 0;
    c->smtp_tls = 0;
    c->pop3_tls = 0;
    c->imap_tls = 0;
    c->cert_pin[0] = '\0';
    c->user[0] = '\0';
    c->pass[0] = '\0';
    c->pass_enc[0] = '\0';
    c->from[0] = '\0';
}

/* Key match helper for the newer keys: the line's key
 * part (length eq) equals the given literal. */
static int key_is(const char *line, size_t eq, const char *key) {
    size_t klen = str_len(key);
    if (eq != klen) {
        return 0;
    }
    for (size_t i = 0; i < klen; i++) {
        if (line[i] != key[i]) {
            return 0;
        }
    }
    return 1;
}

static uint16_t parse_port_val(const char *val, size_t val_len) {
    int v = 0;
    for (size_t i = 0; i < val_len && is_digit(val[i]); i++) {
        v = v * 10 + (val[i] - '0');
    }
    if (v > 0 && v <= 65535) {
        return (uint16_t)v;
    }
    return 0;
}

static void set_field(char *dst, size_t cap, const char *val,
                      size_t len) {
    copy_capped(dst, cap, val, len);
}

int mail_config_parse(mail_config *c, const char *text, size_t len) {
    int found = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t line_start = pos;
        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        size_t line_len = pos - line_start;
        if (pos < len) {
            pos++; /* skip '\n' */
        }
        if (line_len > 0 && text[line_start + line_len - 1] == '\r') {
            line_len--;
        }
        const char *line = text + line_start;
        /* Split at the first '='. */
        size_t eq = 0;
        while (eq < line_len && line[eq] != '=') {
            eq++;
        }
        if (eq == 0 || eq >= line_len) {
            continue;
        }
        const char *val = line + eq + 1;
        size_t val_len = line_len - eq - 1;
        /* Match the key by length, then by content. */
        if (eq == 9 && line[0] == 's' && line[1] == 'm' &&
            line[2] == 't' && line[3] == 'p' && line[4] == '_' &&
            line[5] == 'h' && line[6] == 'o' && line[7] == 's' &&
            line[8] == 't') {
            set_field(c->smtp_host, sizeof(c->smtp_host), val, val_len);
            found = 1;
        } else if (eq == 9 && line[0] == 's' && line[1] == 'm' &&
                   line[2] == 't' && line[3] == 'p' && line[4] == '_' &&
                   line[5] == 'p' && line[6] == 'o' && line[7] == 'r' &&
                   line[8] == 't') {
            int v = 0;
            for (size_t i = 0; i < val_len && is_digit(val[i]); i++) {
                v = v * 10 + (val[i] - '0');
            }
            if (v > 0 && v <= 65535) {
                c->smtp_port = (uint16_t)v;
            }
            found = 1;
        } else if (eq == 9 && line[0] == 'p' && line[1] == 'o' &&
                   line[2] == 'p' && line[3] == '3' && line[4] == '_' &&
                   line[5] == 'h' && line[6] == 'o' && line[7] == 's' &&
                   line[8] == 't') {
            set_field(c->pop3_host, sizeof(c->pop3_host), val, val_len);
            found = 1;
        } else if (eq == 9 && line[0] == 'p' && line[1] == 'o' &&
                   line[2] == 'p' && line[3] == '3' && line[4] == '_' &&
                   line[5] == 'p' && line[6] == 'o' && line[7] == 'r' &&
                   line[8] == 't') {
            int v = 0;
            for (size_t i = 0; i < val_len && is_digit(val[i]); i++) {
                v = v * 10 + (val[i] - '0');
            }
            if (v > 0 && v <= 65535) {
                c->pop3_port = (uint16_t)v;
            }
            found = 1;
        } else if (eq == 4 && line[0] == 'u' && line[1] == 's' &&
                   line[2] == 'e' && line[3] == 'r') {
            set_field(c->user, sizeof(c->user), val, val_len);
            found = 1;
        } else if (eq == 4 && line[0] == 'p' && line[1] == 'a' &&
                   line[2] == 's' && line[3] == 's') {
            set_field(c->pass, sizeof(c->pass), val, val_len);
            found = 1;
        } else if (eq == 4 && line[0] == 'f' && line[1] == 'r' &&
                   line[2] == 'o' && line[3] == 'm') {
            set_field(c->from, sizeof(c->from), val, val_len);
            found = 1;
        } else if (key_is(line, eq, "imap_host")) {
            set_field(c->imap_host, sizeof(c->imap_host), val, val_len);
            found = 1;
        } else if (key_is(line, eq, "imap_port")) {
            uint16_t v = parse_port_val(val, val_len);
            if (v != 0) {
                c->imap_port = v;
            }
            found = 1;
        } else if (key_is(line, eq, "smtp_tls")) {
            c->smtp_tls = val_len > 0 && val[0] == '1';
            found = 1;
        } else if (key_is(line, eq, "pop3_tls")) {
            c->pop3_tls = val_len > 0 && val[0] == '1';
            found = 1;
        } else if (key_is(line, eq, "imap_tls")) {
            c->imap_tls = val_len > 0 && val[0] == '1';
            found = 1;
        } else if (key_is(line, eq, "cert_pin")) {
            set_field(c->cert_pin, sizeof(c->cert_pin), val, val_len);
            found = 1;
        } else if (key_is(line, eq, "pass_enc")) {
            set_field(c->pass_enc, sizeof(c->pass_enc), val, val_len);
            found = 1;
        }
    }
    return found;
}

size_t mail_config_serialize(const mail_config *c, char *buf,
                             size_t cap) {
    size_t p = 0;
    /* String fields in a fixed order. The password line
     * is special: a wrapped password replaces the plain
     * one, never both. */
    const char *keys[10];
    const char *vals[10];
    int nstr = 0;
    keys[nstr] = "smtp_host=";
    vals[nstr++] = c->smtp_host;
    keys[nstr] = "pop3_host=";
    vals[nstr++] = c->pop3_host;
    keys[nstr] = "imap_host=";
    vals[nstr++] = c->imap_host;
    keys[nstr] = "cert_pin=";
    vals[nstr++] = c->cert_pin;
    keys[nstr] = "user=";
    vals[nstr++] = c->user;
    if (c->pass_enc[0] != '\0') {
        keys[nstr] = "pass_enc=";
        vals[nstr++] = c->pass_enc;
    } else {
        keys[nstr] = "pass=";
        vals[nstr++] = c->pass;
    }
    keys[nstr] = "from=";
    vals[nstr++] = c->from;
    for (int i = 0; i < nstr; i++) {
        const char *k = keys[i];
        while (*k != '\0' && p + 1 < cap) {
            buf[p++] = *k++;
        }
        const char *v = vals[i];
        while (*v != '\0' && p + 1 < cap) {
            buf[p++] = *v++;
        }
        if (p + 1 < cap) {
            buf[p++] = '\n';
        }
    }
    /* Ports and TLS flags as numbers. */
    const char *nkeys[6];
    int nvals[6];
    nkeys[0] = "smtp_port=";
    nvals[0] = (int)c->smtp_port;
    nkeys[1] = "pop3_port=";
    nvals[1] = (int)c->pop3_port;
    nkeys[2] = "imap_port=";
    nvals[2] = (int)c->imap_port;
    nkeys[3] = "smtp_tls=";
    nvals[3] = c->smtp_tls ? 1 : 0;
    nkeys[4] = "pop3_tls=";
    nvals[4] = c->pop3_tls ? 1 : 0;
    nkeys[5] = "imap_tls=";
    nvals[5] = c->imap_tls ? 1 : 0;
    for (int i = 0; i < 6; i++) {
        const char *k = nkeys[i];
        while (*k != '\0' && p + 1 < cap) {
            buf[p++] = *k++;
        }
        int v = nvals[i];
        char num[8];
        int ni = 0;
        char rev[6];
        if (v == 0) {
            num[ni++] = '0';
        } else {
            int ri = 0;
            while (v > 0 && ri < 5) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int j = 0; num[j] != '\0' && p + 1 < cap; j++) {
            buf[p++] = num[j];
        }
        if (p + 1 < cap) {
            buf[p++] = '\n';
        }
    }
    if (cap > 0) {
        buf[p] = '\0';
    }
    return p;
}

int mail_config_ready(const mail_config *c) {
    return c->smtp_host[0] != '\0' &&
           (c->pop3_host[0] != '\0' || c->imap_host[0] != '\0') &&
           c->from[0] != '\0';
}

static int append_str(char *out, size_t cap, size_t *p,
                      const char *s) {
    while (*s != '\0') {
        if (*p + 1 >= cap) {
            return -1;
        }
        out[(*p)++] = *s++;
    }
    return 0;
}

static int append_crlf(char *out, size_t cap, size_t *p) {
    if (*p + 2 >= cap) {
        return -1;
    }
    out[(*p)++] = '\r';
    out[(*p)++] = '\n';
    return 0;
}

int mail_build(const char *from, const char *to,
               const char *subject, const char *body, char *out,
               size_t cap) {
    size_t p = 0;
    if (append_str(out, cap, &p, "From: ") != 0) {
        return -1;
    }
    if (append_str(out, cap, &p, from) != 0) {
        return -1;
    }
    if (append_crlf(out, cap, &p) != 0) {
        return -1;
    }
    if (append_str(out, cap, &p, "To: ") != 0) {
        return -1;
    }
    if (append_str(out, cap, &p, to) != 0) {
        return -1;
    }
    if (append_crlf(out, cap, &p) != 0) {
        return -1;
    }
    if (append_str(out, cap, &p, "Subject: ") != 0) {
        return -1;
    }
    if (append_str(out, cap, &p, subject) != 0) {
        return -1;
    }
    if (append_crlf(out, cap, &p) != 0) {
        return -1;
    }
    if (append_str(out, cap, &p, "Date: ") != 0) {
        return -1;
    }
    if (append_crlf(out, cap, &p) != 0) {
        return -1;
    }
    if (append_crlf(out, cap, &p) != 0) {
        return -1;
    }
    /* Body: \n becomes \r\n; a line starting with '.'
     * gets one more '.' in front (SMTP DATA rule). */
    int at_line_start = 1;
    for (size_t i = 0; body[i] != '\0'; i++) {
        char ch = body[i];
        if (at_line_start && ch == '.') {
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = '.';
        }
        if (ch == '\n') {
            if (append_crlf(out, cap, &p) != 0) {
                return -1;
            }
            at_line_start = 1;
        } else if (ch == '\r') {
            continue;
        } else {
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = ch;
            at_line_start = 0;
        }
    }
    if (p + 2 >= cap) {
        return -1;
    }
    out[p++] = '\r';
    out[p++] = '\n';
    out[p] = '\0';
    return (int)p;
}

/* ---------- dates, MIME building, folders ---------- */

void mail_format_date(char *out, size_t cap, int year,
                      int month, int day, int weekday, int hour,
                      int minute, int second, int tz_minutes) {
    static const char *wdays[] = { "Sun", "Mon", "Tue", "Wed",
                                   "Thu", "Fri", "Sat" };
    static const char *mons[] = { "Jan", "Feb", "Mar", "Apr",
                                  "May", "Jun", "Jul", "Aug",
                                  "Sep", "Oct", "Nov", "Dec" };
    size_t p = 0;
    const char *w = (weekday >= 0 && weekday <= 6) ? wdays[weekday]
                                                   : "Sun";
    const char *m = (month >= 1 && month <= 12) ? mons[month - 1]
                                                : "Jan";
    char num[12];
    append_str(out, cap, &p, w);
    append_str(out, cap, &p, ", ");
    {
        int n = 0;
        if (day >= 10) {
            num[n++] = (char)('0' + day / 10);
        }
        num[n++] = (char)('0' + day % 10);
        num[n] = '\0';
    }
    append_str(out, cap, &p, num);
    append_str(out, cap, &p, " ");
    append_str(out, cap, &p, m);
    append_str(out, cap, &p, " ");
    {
        int n = 0;
        int v = year;
        char rev[8];
        int r = 0;
        while (v > 0) {
            rev[r++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (r > 0) {
            num[n++] = rev[--r];
        }
        num[n] = '\0';
    }
    append_str(out, cap, &p, num);
    append_str(out, cap, &p, " ");
    {
        int vals[3];
        vals[0] = hour;
        vals[1] = minute;
        vals[2] = second;
        for (int i = 0; i < 3; i++) {
            int n = 0;
            int v = vals[i];
            if (v < 0) {
                v = 0;
            }
            if (v > 59 && i != 0) {
                v = 59;
            }
            num[n++] = (char)('0' + (v / 10) % 10);
            num[n++] = (char)('0' + v % 10);
            num[n] = '\0';
            append_str(out, cap, &p, num);
            if (i < 2) {
                append_str(out, cap, &p, ":");
            }
        }
    }
    if (tz_minutes <= -10000) {
        /* "-0000": local time, offset unknown. */
        append_str(out, cap, &p, " -0000");
    } else {
        char zb[8];
        int v = tz_minutes;
        zb[0] = ' ';
        zb[1] = v < 0 ? '-' : '+';
        if (v < 0) {
            v = -v;
        }
        int hh = v / 60;
        int mm = v % 60;
        zb[2] = (char)('0' + (hh / 10) % 10);
        zb[3] = (char)('0' + hh % 10);
        zb[4] = (char)('0' + (mm / 10) % 10);
        zb[5] = (char)('0' + mm % 10);
        zb[6] = '\0';
        append_str(out, cap, &p, zb);
    }
    if (p < cap) {
        out[p] = '\0';
    } else if (cap > 0) {
        out[cap - 1] = '\0';
    }
}

static int append_body_text(char *out, size_t cap, size_t *p,
                            const char *body) {
    int at_line_start = 1;
    for (size_t i = 0; body[i] != '\0'; i++) {
        char ch = body[i];
        if (at_line_start && ch == '.') {
            if (*p + 1 >= cap) {
                return -1;
            }
            out[(*p)++] = '.';
        }
        if (ch == '\n') {
            if (append_crlf(out, cap, p) != 0) {
                return -1;
            }
            at_line_start = 1;
        } else if (ch != '\r') {
            if (*p + 1 >= cap) {
                return -1;
            }
            out[(*p)++] = ch;
            at_line_start = 0;
        }
    }
    return 0;
}

int mail_build2(const char *from, const char *to,
                const char *subject, const char *date,
                const char *body, const mail_attach *att,
                char *out, size_t cap) {
    size_t p = 0;
    if (append_str(out, cap, &p, "From: ") != 0 ||
        append_str(out, cap, &p, from) != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p, "To: ") != 0 ||
        append_str(out, cap, &p, to) != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p, "Subject: ") != 0 ||
        append_str(out, cap, &p, subject) != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p, "Date: ") != 0 ||
        append_str(out, cap, &p, date != NULL ? date : "") != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p, "MIME-Version: 1.0") != 0 ||
        append_crlf(out, cap, &p) != 0) {
        return -1;
    }
    if (att == NULL) {
        if (append_str(out, cap, &p,
                       "Content-Type: text/plain; charset=UTF-8") !=
                0 ||
            append_crlf(out, cap, &p) != 0 ||
            append_str(out, cap, &p,
                       "Content-Transfer-Encoding: 8bit") != 0 ||
            append_crlf(out, cap, &p) != 0 ||
            append_crlf(out, cap, &p) != 0 ||
            append_body_text(out, cap, &p, body) != 0 ||
            append_crlf(out, cap, &p) != 0) {
            return -1;
        }
        if (p + 1 >= cap) {
            return -1;
        }
        out[p] = '\0';
        return (int)p;
    }
    /* multipart/mixed with one attachment. The boundary
     * mixes the body and attachment lengths so it cannot
     * appear in typical content. */
    char bnd[48];
    {
        size_t bl = 0;
        const char *pre = "----PrismOS-";
        while (*pre != '\0') {
            bnd[bl++] = *pre++;
        }
        size_t v = str_len(body) * 131 + att->len * 17 + 7;
        char rev[12];
        int r = 0;
        if (v == 0) {
            rev[r++] = '0';
        }
        while (v > 0) {
            rev[r++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (r > 0) {
            bnd[bl++] = rev[--r];
        }
        bnd[bl] = '\0';
    }
    if (append_str(out, cap, &p,
                   "Content-Type: multipart/mixed; boundary=\"") !=
            0 ||
        append_str(out, cap, &p, bnd) != 0 ||
        append_str(out, cap, &p, "\"") != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p, "--") != 0 ||
        append_str(out, cap, &p, bnd) != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p,
                   "Content-Type: text/plain; charset=UTF-8") !=
            0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p,
                   "Content-Transfer-Encoding: 8bit") != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_body_text(out, cap, &p, body) != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p, "--") != 0 ||
        append_str(out, cap, &p, bnd) != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p,
                   "Content-Type: application/octet-stream; "
                   "name=\"") != 0 ||
        append_str(out, cap, &p, att->name) != 0 ||
        append_str(out, cap, &p, "\"") != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p,
                   "Content-Transfer-Encoding: base64") != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p,
                   "Content-Disposition: attachment; "
                   "filename=\"") != 0 ||
        append_str(out, cap, &p, att->name) != 0 ||
        append_str(out, cap, &p, "\"") != 0 ||
        append_crlf(out, cap, &p) != 0 ||
        append_crlf(out, cap, &p) != 0) {
        return -1;
    }
    {
        /* base64 body: encode into the remaining space. */
        int enc = mime_b64_encode(att->data, att->len, out + p,
                                  cap - p, 1);
        if (enc < 0) {
            return -1;
        }
        p += (size_t)enc;
    }
    if (append_crlf(out, cap, &p) != 0 ||
        append_str(out, cap, &p, "--") != 0 ||
        append_str(out, cap, &p, bnd) != 0 ||
        append_str(out, cap, &p, "--") != 0 ||
        append_crlf(out, cap, &p) != 0) {
        return -1;
    }
    if (p + 1 >= cap) {
        return -1;
    }
    out[p] = '\0';
    return (int)p;
}

/* ---------- folders (mail-folders.txt) ---------- */

/* The folder of message n: "Inbox" when unassigned. */
int mail_folder_of(const char *folders, size_t len, int n,
                   char *out, size_t cap) {
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && folders[i] != '\n') {
            i++;
        }
        size_t le = i;
        i++;
        /* line: NNNN=folder */
        int v = 0;
        size_t k = ls;
        int digits = 0;
        while (k < le && folders[k] >= '0' && folders[k] <= '9') {
            v = v * 10 + (folders[k] - '0');
            k++;
            digits++;
        }
        if (digits > 0 && k < le && folders[k] == '=' && v == n) {
            size_t p = 0;
            for (size_t m = k + 1; m < le && p + 1 < cap; m++) {
                if (folders[m] != '\r') {
                    out[p++] = folders[m];
                }
            }
            out[p] = '\0';
            return 0;
        }
    }
    if (cap >= 6) {
        const char *d = "Inbox";
        size_t p = 0;
        while (d[p] != '\0') {
            out[p] = d[p];
            p++;
        }
        out[p] = '\0';
        return 0;
    }
    return -1;
}

/* Build the new mail-folders.txt content with message n
 * assigned to folder (or removed when folder is "Inbox").
 * Returns the byte count or -1. */
int mail_folder_set(const char *folders, size_t len, int n,
                    const char *folder, char *out, size_t cap) {
    size_t p = 0;
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && folders[i] != '\n') {
            i++;
        }
        size_t le = i;
        i++;
        int v = 0;
        size_t k = ls;
        int digits = 0;
        while (k < le && folders[k] >= '0' && folders[k] <= '9') {
            v = v * 10 + (folders[k] - '0');
            k++;
            digits++;
        }
        if (digits > 0 && k < le && folders[k] == '=' && v == n) {
            continue; /* drop the old assignment */
        }
        for (size_t m = ls; m < le; m++) {
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = folders[m];
        }
        if (p + 1 >= cap) {
            return -1;
        }
        out[p++] = '\n';
    }
    if (folder != NULL && folder[0] != '\0' &&
        !(folder[0] == 'I' && folder[1] == 'n' &&
          folder[2] == 'b' && folder[3] == 'o' &&
          folder[4] == 'x' && folder[5] == '\0')) {
        char num[12];
        int ni = 0;
        int v = n;
        char rev[12];
        int r = 0;
        if (v == 0) {
            rev[r++] = '0';
        }
        while (v > 0) {
            rev[r++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (r > 0) {
            num[ni++] = rev[--r];
        }
        num[ni] = '\0';
        if (append_str(out, cap, &p, num) != 0 ||
            append_str(out, cap, &p, "=") != 0 ||
            append_str(out, cap, &p, folder) != 0 ||
            append_str(out, cap, &p, "\n") != 0) {
            return -1;
        }
    }
    if (p + 1 >= cap) {
        return -1;
    }
    out[p] = '\0';
    return (int)p;
}

int mail_parse_header(const char *text, const char *field,
                      char *out, size_t cap) {
    size_t field_len = str_len(field);
    size_t pos = 0;
    while (text[pos] != '\0') {
        size_t line_start = pos;
        while (text[pos] != '\0' && text[pos] != '\n') {
            pos++;
        }
        size_t line_len = pos - line_start;
        if (text[pos] == '\n') {
            pos++;
        }
        /* Trim a trailing CR for the comparison; a blank
         * line (empty, or just "\r") ends the headers. */
        size_t cmp_len = line_len;
        if (cmp_len > 0 && text[line_start + cmp_len - 1] == '\r') {
            cmp_len--;
        }
        if (cmp_len == 0) {
            break; /* blank line: end of headers */
        }
        if (cmp_len > field_len && text[line_start + field_len] == ':' &&
            cmp_len >= field_len + 1) {
            int match = 1;
            for (size_t i = 0; i < field_len; i++) {
                if (text[line_start + i] != field[i]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                size_t v = line_start + field_len + 1;
                size_t vend = line_start + cmp_len;
                while (v < vend && text[v] == ' ') {
                    v++;
                }
                copy_capped(out, cap, text + v, vend - v);
                return 0;
            }
        }
    }
    out[0] = '\0';
    return -1;
}

int mail_reply_code(const char *line) {
    if (line == NULL || line[0] == '\0') {
        return -1;
    }
    if (line[0] == '+' && line[1] == 'O' && line[2] == 'K') {
        return 200;
    }
    if (line[0] == '-' && line[1] == 'E' && line[2] == 'R' &&
        line[3] == 'R') {
        return -1;
    }
    if (!is_digit(line[0]) || !is_digit(line[1]) ||
        !is_digit(line[2])) {
        return -1;
    }
    return (line[0] - '0') * 100 + (line[1] - '0') * 10 +
           (line[2] - '0');
}

int mail_reply_done(const char *line, int code) {
    if (line == NULL || code < 0) {
        return 1;
    }
    if (mail_reply_code(line) != code) {
        return 1;
    }
    /* "250 ..." ends the reply; "250-..." continues it. */
    if (line[3] == '\0') {
        return 1;
    }
    return line[3] == ' ';
}

int mail_reply_ok(int code) {
    return code >= 200 && code < 400;
}

size_t mail_dot_unstuff(char *text, size_t len) {
    size_t w = 0;
    size_t r = 0;
    while (r < len) {
        int at_start = (r == 0) || (r > 0 && text[r - 1] == '\n');
        if (at_start && text[r] == '.' && r + 1 < len &&
            text[r + 1] == '.') {
            r++; /* drop one of the two dots */
        }
        text[w++] = text[r++];
    }
    text[w] = '\0';
    return w;
}

int mail_file_name(int n, char *out, size_t cap) {
    if (n < 1 || n > 9999 || cap < 14) {
        return -1;
    }
    out[0] = 'm';
    out[1] = 'a';
    out[2] = 'i';
    out[3] = 'l';
    out[4] = '-';
    out[5] = (char)('0' + (n / 1000) % 10);
    out[6] = (char)('0' + (n / 100) % 10);
    out[7] = (char)('0' + (n / 10) % 10);
    out[8] = (char)('0' + n % 10);
    out[9] = '.';
    out[10] = 't';
    out[11] = 'x';
    out[12] = 't';
    out[13] = '\0';
    return 0;
}

int mail_index_from_name(const char *name) {
    if (name == NULL) {
        return -1;
    }
    if (name[0] != 'm' || name[1] != 'a' || name[2] != 'i' ||
        name[3] != 'l' || name[4] != '-' || name[9] != '.' ||
        name[10] != 't' || name[11] != 'x' || name[12] != 't' ||
        name[13] != '\0') {
        return -1;
    }
    int n = 0;
    for (int i = 5; i < 9; i++) {
        if (!is_digit(name[i])) {
            return -1;
        }
        n = n * 10 + (name[i] - '0');
    }
    if (n < 1 || n > 9999) {
        return -1;
    }
    return n;
}
