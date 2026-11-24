/* PrismOS - mail over the network (see mailproto.h). */
#include "mailproto.h"
#include "../shell/vfs.h"

#include "../shell/mime.h"
#include "netstack.h"
#include "rtc.h"
#include "settings.h"
#include "serial.h"
#include "tls.h"

/* One stream and one line buffer, static: a 4KB stream
 * plus a message buffer must not live on the kernel stack. */
static net_tcp_stream mail_stream;
static char mail_line[512];
static char mail_msg[PRISM_VFS_MAX_APP_FILE_SIZE];

/* ---- transport: plain TCP or implicit TLS ----
 * One connection at a time (the TLS state is a static,
 * like the stream). TLS policy is certificate pinning:
 * cfg->cert_pin must hold the server certificate's
 * SHA-256 fingerprint and match, else the handshake
 * refuses (see tls.h). */
typedef struct {
    net_tcp_stream *tcp;
    tls_conn tls;
    int use_tls;
} mail_io;

static mail_io mail_io_inst;

static int io_open(mail_io *io, const mail_config *cfg,
                   const char *host, uint16_t port, int use_tls) {
    io->tcp = &mail_stream;
    io->use_tls = use_tls;
    if (use_tls) {
        /* tls_open makes its own TCP connection (the
         * stream lives inside tls_conn). */
        if (tls_open(&io->tls, host, port, cfg->cert_pin) != 0) {
            serial_write("mail: TLS failed: ");
            serial_write_line(tls_last_error(&io->tls));
            return -1;
        }
        serial_write_line("mail: TLS session established.");
        return 0;
    }
    if (net_tcp_open(host, port, &mail_stream) != 0) {
        return -1;
    }
    return 0;
}

static int io_send(mail_io *io, const char *data, size_t len) {
    if (io->use_tls) {
        return tls_send(&io->tls, data, len);
    }
    return net_tcp_send(io->tcp, data, len);
}

static int io_recv_line(mail_io *io, char *out, size_t cap,
                        uint32_t timeout_sec) {
    if (io->use_tls) {
        return tls_recv_line(&io->tls, out, cap, timeout_sec);
    }
    return net_tcp_recv_line(io->tcp, out, cap, timeout_sec);
}

/* Read exactly n raw bytes (IMAP literals). Returns 0
 * on success. Bytes land in out (n <= cap is the
 * caller's contract; larger literals are drained into
 * a scratch by the caller in chunks). */
static int io_recv_exact(mail_io *io, uint8_t *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r;
        if (io->use_tls) {
            r = tls_recv(&io->tls, out + got, n - got);
        } else {
            r = net_tcp_recv(io->tcp, out + got, n - got, 15);
        }
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static void io_close(mail_io *io) {
    if (io->use_tls) {
        tls_close(&io->tls); /* closes its TCP too */
    } else {
        net_tcp_close(io->tcp);
    }
}

/* Read one reply, following multi-line replies ("250-..."
 * then "250 ..."). Returns the code, or -1 on error. */
static int read_reply(mail_io *s) {
    int code = -1;
    for (int i = 0; i < 10; i++) {
        if (io_recv_line(s, mail_line, sizeof(mail_line), 10) < 0) {
            return -1;
        }
        int c = mail_reply_code(mail_line);
        if (c < 0) {
            return -1;
        }
        code = c;
        if (mail_reply_done(mail_line, code)) {
            return code;
        }
    }
    return -1;
}

/* Send one command line and read its reply. Returns the
 * reply code, or -1 on error. */
static int command(mail_io *s, const char *line) {
    char buf[256];
    size_t p = 0;
    while (line[p] != '\0' && p + 3 < sizeof(buf)) {
        buf[p] = line[p];
        p++;
    }
    buf[p++] = '\r';
    buf[p++] = '\n';
    buf[p] = '\0';
    if (io_send(s, buf, p) != 0) {
        return -1;
    }
    return read_reply(s);
}

static void log_code(const char *what, int code) {
    serial_write("mail: ");
    serial_write(what);
    serial_write(" failed (code ");
    char num[8];
    int ni = 0;
    int v = code < 0 ? 0 : code;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[8];
        int ri = 0;
        while (v > 0 && ri < 7) {
            rev[ri++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    serial_write(num);
    serial_write_line(").");
}

static void log_received(int fetched) {
    char num[8];
    int ni = 0;
    int v = fetched;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[8];
        int ri = 0;
        while (v > 0 && ri < 7) {
            rev[ri++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    serial_write("mail: received ");
    serial_write(num);
    serial_write_line(" message(s).");
}

/* Extract every attachment of a stored message into
 * the VFS under its (sanitised) file name. Returns the
 * number saved. Static scratch: attachments can be up
 * to one full file each. */
static uint8_t mail_att_data[PRISM_VFS_MAX_APP_FILE_SIZE];

int mail_save_attachments(const char *msg, size_t len) {
    size_t cursor = 0;
    int saved = 0;
    for (;;) {
        char name[64];
        size_t dlen = 0;
        int rc = mime_next_attachment(msg, len, &cursor, name,
                                      sizeof(name),
                                      mail_att_data,
                                      sizeof(mail_att_data),
                                      &dlen);
        if (rc <= 0) {
            break;
        }
        if (vfs_write(name, (const char *)mail_att_data,
                      dlen) == 0) {
            saved++;
            serial_write("mail: attachment saved ");
            serial_write_line(name);
        }
    }
    return saved;
}

int mail_smtp_send_att(const mail_config *cfg, const char *to,
                       const char *subject, const char *body,
                       const mail_attach *att, char *built_out,
                       size_t built_cap, int *built_len) {
    if (cfg == NULL || to == NULL || subject == NULL ||
        body == NULL || cfg->smtp_host[0] == '\0') {
        return -1;
    }
    mail_io *io = &mail_io_inst;
    uint16_t port = cfg->smtp_port != 0
                        ? cfg->smtp_port
                        : (uint16_t)(cfg->smtp_tls ? 465 : 25);
    if (io_open(io, cfg, cfg->smtp_host, port, cfg->smtp_tls) != 0) {
        serial_write_line("mail: SMTP connect failed.");
        return -1;
    }
    int rc = -1;
    int code = read_reply(io);
    if (!mail_reply_ok(code)) {
        log_code("SMTP greeting", code);
        goto done;
    }
    code = command(io, "EHLO PrismOS");
    if (!mail_reply_ok(code)) {
        log_code("SMTP EHLO", code);
        goto done;
    }
    {
        char line[192];
        size_t p = 0;
        const char *pre = "MAIL FROM:<";
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        for (size_t i = 0; cfg->from[i] != '\0' && p + 1 < sizeof(line);
             i++) {
            line[p++] = cfg->from[i];
        }
        if (p + 1 < sizeof(line)) {
            line[p++] = '>';
        }
        line[p] = '\0';
        code = command(io, line);
        if (!mail_reply_ok(code)) {
            log_code("SMTP MAIL FROM", code);
            goto done;
        }
    }
    {
        char line[192];
        size_t p = 0;
        const char *pre = "RCPT TO:<";
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        /* Copy by index, not by advancing `to`: the address
         * is needed again for the message headers below. */
        for (size_t i = 0; to[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = to[i];
        }
        if (p + 1 < sizeof(line)) {
            line[p++] = '>';
        }
        line[p] = '\0';
        code = command(io, line);
        if (!mail_reply_ok(code)) {
            log_code("SMTP RCPT TO", code);
            goto done;
        }
    }
    code = command(io, "DATA");
    if (code != 354) {
        log_code("SMTP DATA", code);
        goto done;
    }
    /* The message itself, then "." on a line of its own.
     * Built with a real Date from the RTC and MIME
     * structure (multipart when an attachment rides). */
    {
        char date[48];
        prism_rtc_time rt;
        if (rtc_read(&rt) == 0) {
            int tz = -10000;
            {
                prism_settings st;
                settings_defaults(&st);
                size_t slen = 0;
                const char *stext =
                    vfs_read_ptr("settings.txt", &slen);
                if (stext != NULL) {
                    settings_parse(&st, stext, slen);
                    tz = st.tz_offset;
                }
            }
            mail_format_date(date, sizeof(date), 2000 + rt.year,
                             rt.month, rt.day,
                             rtc_weekday(2000 + rt.year, rt.month,
                                         rt.day),
                             rt.hour, rt.minute, rt.second, tz);
        } else {
            date[0] = '\0';
        }
        if (mail_build2(cfg->from, to, subject, date, body,
                        att, mail_msg, sizeof(mail_msg)) < 0) {
            serial_write_line("mail: message too large to send.");
            goto done;
        }
        if (built_out != NULL && built_cap > 0) {
            size_t len = 0;
            while (mail_msg[len] != '\0' &&
                   len + 1 < built_cap) {
                built_out[len] = mail_msg[len];
                len++;
            }
            built_out[len] = '\0';
            if (built_len != NULL) {
                *built_len = (int)len;
            }
        }
    }
    {
        size_t len = 0;
        while (mail_msg[len] != '\0') {
            len++;
        }
        if (io_send(io, mail_msg, len) != 0) {
            serial_write_line("mail: failed to send message body.");
            goto done;
        }
    }
    code = command(io, ".");
    if (!mail_reply_ok(code)) {
        log_code("SMTP message", code);
        goto done;
    }
    (void)command(io, "QUIT");
    rc = 0;
    serial_write("mail: sent to ");
    serial_write_line(to);
done:
    io_close(io);
    return rc;
}

/* Archive a built message as the next sent-NNNN.txt
 * (the Sent folder). Returns 0 on success. */
int mail_archive_sent(const char *built, size_t len) {
    int mx = 0;
    int cnt = vfs_file_count();
    for (int i = 0; i < cnt; i++) {
        const char *nm = vfs_file_name(i);
        if (nm[0] == 's' && nm[1] == 'e' && nm[2] == 'n' &&
            nm[3] == 't' && nm[4] == '-') {
            int v = 0;
            int digits = 0;
            for (int k = 5; nm[k] >= '0' && nm[k] <= '9'; k++) {
                v = v * 10 + (nm[k] - '0');
                digits++;
            }
            if (digits > 0 && v > mx) {
                mx = v;
            }
        }
    }
    int v = mx + 1;
    if (v > 9999) {
        return -1;
    }
    char name[20];
    size_t p = 0;
    const char *pre = "sent-";
    while (*pre != '\0') {
        name[p++] = *pre++;
    }
    name[p++] = (char)('0' + (v / 1000) % 10);
    name[p++] = (char)('0' + (v / 100) % 10);
    name[p++] = (char)('0' + (v / 10) % 10);
    name[p++] = (char)('0' + v % 10);
    const char *suf = ".txt";
    while (*suf != '\0') {
        name[p++] = *suf++;
    }
    name[p] = '\0';
    return vfs_write(name, built, len);
}

int mail_smtp_send(const mail_config *cfg, const char *to,
                   const char *subject, const char *body) {
    return mail_smtp_send_att(cfg, to, subject, body, NULL,
                              NULL, 0, NULL);
}

int mail_pop3_fetch(const mail_config *cfg, mail_fetch_cb cb,
                    void *ctx) {
    if (cfg == NULL || cb == NULL || cfg->pop3_host[0] == '\0') {
        return -1;
    }
    mail_io *io = &mail_io_inst;
    uint16_t port = cfg->pop3_port != 0
                        ? cfg->pop3_port
                        : (uint16_t)(cfg->pop3_tls ? 995 : 110);
    if (io_open(io, cfg, cfg->pop3_host, port, cfg->pop3_tls) != 0) {
        serial_write_line("mail: POP3 connect failed.");
        return -1;
    }
    int fetched = -1;
    int code = read_reply(io);
    if (!mail_reply_ok(code)) {
        log_code("POP3 greeting", code);
        goto done;
    }
    {
        char line[160];
        size_t p = 0;
        const char *pre = "USER ";
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        const char *u = cfg->user;
        while (*u != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *u++;
        }
        line[p] = '\0';
        code = command(io, line);
        if (!mail_reply_ok(code)) {
            log_code("POP3 USER", code);
            goto done;
        }
    }
    {
        char line[160];
        size_t p = 0;
        const char *pre = "PASS ";
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        const char *pw = cfg->pass;
        while (*pw != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pw++;
        }
        line[p] = '\0';
        code = command(io, line);
        if (!mail_reply_ok(code)) {
            log_code("POP3 PASS", code);
            goto done;
        }
    }
    /* STAT: "+OK <count> <bytes>". */
    int count = 0;
    {
        code = command(io, "STAT");
        if (!mail_reply_ok(code)) {
            log_code("POP3 STAT", code);
            goto done;
        }
        /* Parse the count after "+OK ". */
        int i = 0;
        while (mail_line[i] != '\0' && mail_line[i] != ' ') {
            i++;
        }
        while (mail_line[i] == ' ') {
            i++;
        }
        while (mail_line[i] >= '0' && mail_line[i] <= '9') {
            count = count * 10 + (mail_line[i] - '0');
            i++;
        }
    }
    fetched = 0;
    for (int msg_no = 1; msg_no <= count; msg_no++) {
        char line[32];
        size_t p = 0;
        const char *pre = "RETR ";
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        char num[8];
        int ni = 0;
        int v = msg_no;
        char rev[8];
        int ri = 0;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            while (v > 0 && ri < 7) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = num[i];
        }
        line[p] = '\0';
        /* Send RETR and read its reply: the reply is
         * followed by the multi-line message. */
        code = command(io, line);
        if (!mail_reply_ok(code)) {
            log_code("POP3 RETR", code);
            goto done;
        }
        /* Read lines until a line that is just ".". */
        size_t mlen = 0;
        int truncated = 0;
        for (;;) {
            if (io_recv_line(io, mail_line, sizeof(mail_line),
                             10) < 0) {
                serial_write_line("mail: message cut off.");
                goto done;
            }
            if (mail_line[0] == '.' && mail_line[1] == '\0') {
                break;
            }
            /* Dot-stuffed lines: "..x" is really ".x". */
            const char *lp = mail_line;
            if (lp[0] == '.' && lp[1] == '.') {
                lp++;
            }
            size_t ll = 0;
            while (lp[ll] != '\0') {
                ll++;
            }
            if (mlen + ll + 1 >= sizeof(mail_msg)) {
                truncated = 1;
                continue; /* keep reading to the "." line */
            }
            for (size_t i = 0; i < ll; i++) {
                mail_msg[mlen++] = lp[i];
            }
            mail_msg[mlen++] = '\n';
        }
        mail_msg[mlen] = '\0';
        if (truncated) {
            serial_write_line(
                "mail: message truncated to the file size limit.");
        }
        if (cb(mail_msg, mlen, ctx) != 0) {
            /* The callback refused the message: stop, and
             * do not delete this one from the server. */
            fetched = -1;
            goto done;
        }
        fetched++;
        /* Delete this message from the server right away,
         * so an interrupted fetch only ever re-delivers
         * the one message in flight. */
        {
            char line[32];
            size_t p = 0;
            const char *pre = "DELE ";
            while (*pre != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *pre++;
            }
            for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line);
                 i++) {
                line[p++] = num[i];
            }
            line[p] = '\0';
            (void)command(io, line);
        }
    }
    (void)command(io, "QUIT");
    log_received(fetched);
done:
    io_close(io);
    return fetched;
}

/* ---- IMAP receive ----
 * LOGIN, SELECT INBOX, FETCH each message with
 * BODY.PEEK[] (read-only: nothing is deleted or marked
 * seen server-side, unlike POP3), LOGOUT. Literals are
 * read byte-exactly, so bodies keep their CRLF. */
static int imap_tag_no;

static void imap_tag(char *out) {
    imap_tag_no++;
    out[0] = 'a';
    out[1] = (char)('0' + (imap_tag_no / 100) % 10);
    out[2] = (char)('0' + (imap_tag_no / 10) % 10);
    out[3] = (char)('0' + imap_tag_no % 10);
    out[4] = '\0';
}

static int str_starts(const char *s, const char *pre) {
    int i = 0;
    while (pre[i] != '\0') {
        if (s[i] != pre[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int str_contains(const char *s, const char *needle) {
    for (int i = 0; s[i] != '\0'; i++) {
        int j = 0;
        while (needle[j] != '\0' && s[i + j] == needle[j]) {
            j++;
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

/* Send one IMAP command line (already tagged). */
static int imap_send(mail_io *io, const char *line) {
    char buf[300];
    size_t p = 0;
    while (line[p] != '\0' && p + 3 < sizeof(buf)) {
        buf[p] = line[p];
        p++;
    }
    buf[p++] = '\r';
    buf[p++] = '\n';
    buf[p] = '\0';
    return io_send(io, buf, p);
}

/* Read lines until the tagged completion for tag.
 * Returns 0 on "<tag> OK", -1 otherwise. When exists_out
 * is non-NULL, "* <n> EXISTS" lines along the way fill
 * it. When literal_out is non-NULL, a FETCH literal is
 * read into mail_msg (truncated to the buffer, the rest
 * drained) and *literal_out set to the stored length. */
static int imap_until_tagged(mail_io *io, const char *tag,
                             int *exists_out, size_t *literal_out) {
    char tagsp[8];
    size_t tp = 0;
    while (tag[tp] != '\0' && tp + 2 < sizeof(tagsp)) {
        tagsp[tp] = tag[tp];
        tp++;
    }
    tagsp[tp++] = ' ';
    tagsp[tp] = '\0';
    for (int lines = 0; lines < 200; lines++) {
        if (io_recv_line(io, mail_line, sizeof(mail_line), 15) < 0) {
            return -1;
        }
        if (str_starts(mail_line, tagsp)) {
            /* 完了行は "<tag> OK" で始まるものだけを成功と
             * する (本文中の " OK" の部分一致では通さない)。
             * OK の後は行末か空白 (説明文) のみ許す。 */
            const char *rest = mail_line + tp;
            if (rest[0] == 'O' && rest[1] == 'K' &&
                (rest[2] == '\0' || rest[2] == ' ')) {
                return 0;
            }
            return -1;
        }
        if (exists_out != NULL && mail_line[0] == '*' &&
            str_contains(mail_line, " EXISTS")) {
            int i = 2;
            int v = 0;
            while (mail_line[i] >= '0' && mail_line[i] <= '9') {
                if (v > 100000) {
                    v = 1000000; /* 溢れ防止の打ち切り */
                    break;
                }
                v = v * 10 + (mail_line[i] - '0');
                i++;
            }
            *exists_out = v;
        }
        if (literal_out != NULL && mail_line[0] == '*' &&
            str_contains(mail_line, "{")) {
            /* "* n FETCH (BODY[] {1234}" — the literal
             * follows immediately. */
            int i = 0;
            while (mail_line[i] != '{' && mail_line[i] != '\0') {
                i++;
            }
            long lit = 0;
            i++;
            while (mail_line[i] >= '0' && mail_line[i] <= '9') {
                lit = lit * 10 + (mail_line[i] - '0');
                if (lit > 1048576) {
                    return -1; /* 上限超えはその場で打ち切る */
                }
                i++;
            }
            size_t keep = (size_t)lit < sizeof(mail_msg) - 1
                              ? (size_t)lit
                              : sizeof(mail_msg) - 1;
            if (io_recv_exact(io, (uint8_t *)mail_msg, keep) != 0) {
                return -1;
            }
            size_t left = (size_t)lit - keep;
            uint8_t scratch[256];
            while (left > 0) {
                size_t chunk = left < sizeof(scratch) ? left
                                                      : sizeof(scratch);
                if (io_recv_exact(io, scratch, chunk) != 0) {
                    return -1;
                }
                left -= chunk;
            }
            if ((size_t)lit > keep) {
                serial_write_line(
                    "mail: message truncated to the file size limit.");
            }
            mail_msg[keep] = '\0';
            *literal_out = keep;
        }
    }
    return -1;
}

/* Append s to buf at *p, quoted for IMAP (" and \ are
 * escaped). */
static void imap_append_quoted(char *buf, size_t cap, size_t *p,
                               const char *s) {
    if (*p + 1 < cap) {
        buf[(*p)++] = '"';
    }
    for (size_t i = 0; s[i] != '\0'; i++) {
        if ((s[i] == '"' || s[i] == '\\') && *p + 2 < cap) {
            buf[(*p)++] = '\\';
        }
        if (*p + 1 < cap) {
            buf[(*p)++] = s[i];
        }
    }
    if (*p + 1 < cap) {
        buf[(*p)++] = '"';
    }
    buf[*p] = '\0';
}

int mail_imap_fetch(const mail_config *cfg, mail_fetch_cb cb,
                    void *ctx) {
    if (cfg == NULL || cb == NULL || cfg->imap_host[0] == '\0') {
        return -1;
    }
    mail_io *io = &mail_io_inst;
    uint16_t port = cfg->imap_port != 0
                        ? cfg->imap_port
                        : (uint16_t)(cfg->imap_tls ? 993 : 143);
    if (io_open(io, cfg, cfg->imap_host, port, cfg->imap_tls) != 0) {
        serial_write_line("mail: IMAP connect failed.");
        return -1;
    }
    int fetched = -1;
    imap_tag_no = 0;
    char tag[8];
    /* Greeting: "* OK ...". */
    if (io_recv_line(io, mail_line, sizeof(mail_line), 15) < 0 ||
        !str_starts(mail_line, "* OK")) {
        serial_write_line("mail: IMAP greeting failed.");
        goto done;
    }
    {
        char line[300];
        size_t p = 0;
        imap_tag(tag);
        for (int i = 0; tag[i] != '\0'; i++) {
            line[p++] = tag[i];
        }
        const char *mid = " LOGIN ";
        while (*mid != '\0') {
            line[p++] = *mid++;
        }
        imap_append_quoted(line, sizeof(line), &p, cfg->user);
        if (p + 1 < sizeof(line)) {
            line[p++] = ' ';
        }
        imap_append_quoted(line, sizeof(line), &p, cfg->pass);
        line[p] = '\0';
        if (imap_send(io, line) != 0 ||
            imap_until_tagged(io, tag, NULL, NULL) != 0) {
            serial_write_line("mail: IMAP login failed.");
            goto done;
        }
    }
    int exists = 0;
    {
        char line[64];
        size_t p = 0;
        imap_tag(tag);
        for (int i = 0; tag[i] != '\0'; i++) {
            line[p++] = tag[i];
        }
        const char *mid = " SELECT INBOX";
        while (*mid != '\0') {
            line[p++] = *mid++;
        }
        line[p] = '\0';
        if (imap_send(io, line) != 0 ||
            imap_until_tagged(io, tag, &exists, NULL) != 0) {
            serial_write_line("mail: IMAP select failed.");
            goto done;
        }
    }
    fetched = 0;
    for (int msg_no = 1; msg_no <= exists; msg_no++) {
        char line[64];
        size_t p = 0;
        imap_tag(tag);
        for (int i = 0; tag[i] != '\0'; i++) {
            line[p++] = tag[i];
        }
        const char *mid = " FETCH ";
        while (*mid != '\0') {
            line[p++] = *mid++;
        }
        {
            char num[8];
            int ni = 0;
            int v = msg_no;
            char rev[8];
            int ri = 0;
            while (v > 0 && ri < 7) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
            num[ni] = '\0';
            for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line);
                 i++) {
                line[p++] = num[i];
            }
        }
        const char *tail = " (BODY.PEEK[])";
        while (*tail != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *tail++;
        }
        line[p] = '\0';
        size_t lit_len = (size_t)-1;
        if (imap_send(io, line) != 0 ||
            imap_until_tagged(io, tag, NULL, &lit_len) != 0) {
            serial_write_line("mail: IMAP fetch failed.");
            fetched = -1;
            goto done;
        }
        if (lit_len == (size_t)-1) {
            /* リテラル (本文) が無いのに OK が来た場合は
             * 空メールとして成功にしない。 */
            serial_write_line(
                "mail: IMAP fetch returned no message body.");
            fetched = -1;
            goto done;
        }
        if (cb(mail_msg, lit_len, ctx) != 0) {
            fetched = -1;
            goto done;
        }
        fetched++;
    }
    {
        char line[32];
        size_t p = 0;
        imap_tag(tag);
        for (int i = 0; tag[i] != '\0'; i++) {
            line[p++] = tag[i];
        }
        const char *mid = " LOGOUT";
        while (*mid != '\0') {
            line[p++] = *mid++;
        }
        line[p] = '\0';
        (void)imap_send(io, line);
        (void)imap_until_tagged(io, tag, NULL, NULL);
    }
    log_received(fetched);
done:
    io_close(io);
    return fetched;
}
