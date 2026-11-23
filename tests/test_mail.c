/* PrismOS - mail logic host test.
 *
 * Runs shell/mail.c on the host (no network, no card).
 * `make test` で走る。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mail.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(void) {
    /* Reply codes: SMTP numbers, POP3 +OK/-ERR. */
    CHECK(mail_reply_code("220 mail ready") == 220);
    CHECK(mail_reply_code("250 OK") == 250);
    CHECK(mail_reply_code("250-hello") == 250);
    CHECK(mail_reply_code("354 End data") == 354);
    CHECK(mail_reply_code("221 Bye") == 221);
    CHECK(mail_reply_code("550 No such user") == 550);
    CHECK(mail_reply_code("+OK mailbox ready") == 200);
    CHECK(mail_reply_code("+OK 2 512") == 200);
    CHECK(mail_reply_code("-ERR no such message") == -1);
    CHECK(mail_reply_code("hello") == -1);
    CHECK(mail_reply_code("") == -1);
    CHECK(mail_reply_ok(220) == 1);
    CHECK(mail_reply_ok(250) == 1);
    CHECK(mail_reply_ok(354) == 1);
    CHECK(mail_reply_ok(550) == 0);
    CHECK(mail_reply_ok(-1) == 0);
    /* Multi-line replies end only on "250 ...". */
    CHECK(mail_reply_done("250-hello", 250) == 0);
    CHECK(mail_reply_done("250 hello", 250) == 1);
    CHECK(mail_reply_done("250", 250) == 1);
    CHECK(mail_reply_done("+OK done", 200) == 1);

    /* Message build: headers, blank line, CRLF body,
     * dot-stuffing. */
    char msg[1024];
    int n = mail_build("prism@prismos.local", "friend@example.com",
                       "Hello", "line one\n.line two\nlast",
                       msg, sizeof(msg));
    CHECK(n > 0);
    CHECK(str_eq(msg,
                 "From: prism@prismos.local\r\n"
                 "To: friend@example.com\r\n"
                 "Subject: Hello\r\n"
                 "Date: \r\n"
                 "\r\n"
                 "line one\r\n"
                 "..line two\r\n"
                 "last\r\n"));
    /* Too small a buffer is reported, not silently cut. */
    char tiny[16];
    CHECK(mail_build("a@b", "c@d", "S", "body", tiny,
                     sizeof(tiny)) == -1);

    /* Header parsing. */
    char val[64];
    CHECK(mail_parse_header(msg, "From", val, sizeof(val)) == 0);
    CHECK(str_eq(val, "prism@prismos.local"));
    CHECK(mail_parse_header(msg, "To", val, sizeof(val)) == 0);
    CHECK(str_eq(val, "friend@example.com"));
    CHECK(mail_parse_header(msg, "Subject", val, sizeof(val)) == 0);
    CHECK(str_eq(val, "Hello"));
    CHECK(mail_parse_header(msg, "Date", val, sizeof(val)) == 0);
    CHECK(str_eq(val, ""));
    CHECK(mail_parse_header(msg, "X-Nope", val, sizeof(val)) == -1);
    /* A field name must match whole ("Sub" is not "Subject"). */
    CHECK(mail_parse_header(msg, "Sub", val, sizeof(val)) == -1);
    /* Headers stop at the blank line: a body line that
     * looks like a header is not one. */
    const char *tricky = "Subject: Real\r\n\r\nFrom: fake@x\r\n";
    CHECK(mail_parse_header(tricky, "From", val, sizeof(val)) == -1);

    /* POP3 dot-unstuffing. */
    char stuffed[] = "..dot\r\nnormal\r\n...three\r\n";
    size_t unlen = mail_dot_unstuff(stuffed, strlen(stuffed));
    CHECK(str_eq(stuffed, ".dot\r\nnormal\r\n..three\r\n"));
    CHECK(unlen == strlen(".dot\r\nnormal\r\n..three\r\n"));

    /* Message file names. */
    char name[16];
    CHECK(mail_file_name(1, name, sizeof(name)) == 0);
    CHECK(str_eq(name, "mail-0001.txt"));
    CHECK(mail_file_name(42, name, sizeof(name)) == 0);
    CHECK(str_eq(name, "mail-0042.txt"));
    CHECK(mail_file_name(9999, name, sizeof(name)) == 0);
    CHECK(str_eq(name, "mail-9999.txt"));
    CHECK(mail_file_name(0, name, sizeof(name)) == -1);
    CHECK(mail_file_name(10000, name, sizeof(name)) == -1);
    CHECK(mail_index_from_name("mail-0001.txt") == 1);
    CHECK(mail_index_from_name("mail-0042.txt") == 42);
    CHECK(mail_index_from_name("mail-9999.txt") == 9999);
    CHECK(mail_index_from_name("mail-0000.txt") == -1);
    CHECK(mail_index_from_name("mail-1.txt") == -1);
    CHECK(mail_index_from_name("README.txt") == -1);
    CHECK(mail_index_from_name("mail-00a1.txt") == -1);
    CHECK(mail_index_from_name(NULL) == -1);

    /* Config parse / serialize round trip. */
    mail_config cfg;
    mail_config_defaults(&cfg);
    CHECK(mail_config_ready(&cfg) == 0);
    const char *conf =
        "smtp_host=10.0.2.2\n"
        "smtp_port=2525\n"
        "pop3_host=10.0.2.2\n"
        "pop3_port=1110\n"
        "user=prism\n"
        "pass=secret\n"
        "from=prism@prismos.local\n"
        "unknown_key=ignored\n";
    CHECK(mail_config_parse(&cfg, conf, strlen(conf)) == 1);
    CHECK(str_eq(cfg.smtp_host, "10.0.2.2"));
    CHECK(cfg.smtp_port == 2525);
    CHECK(str_eq(cfg.pop3_host, "10.0.2.2"));
    CHECK(cfg.pop3_port == 1110);
    CHECK(str_eq(cfg.user, "prism"));
    CHECK(str_eq(cfg.pass, "secret"));
    CHECK(str_eq(cfg.from, "prism@prismos.local"));
    CHECK(mail_config_ready(&cfg) == 1);
    char buf[512];
    size_t w = mail_config_serialize(&cfg, buf, sizeof(buf));
    CHECK(w > 0);
    mail_config cfg2;
    mail_config_defaults(&cfg2);
    CHECK(mail_config_parse(&cfg2, buf, w) == 1);
    CHECK(str_eq(cfg2.smtp_host, "10.0.2.2"));
    CHECK(cfg2.smtp_port == 2525);
    CHECK(cfg2.pop3_port == 1110);
    CHECK(str_eq(cfg2.pass, "secret"));
    /* Ports default to 0 (unset) when missing. */
    mail_config cfg3;
    mail_config_defaults(&cfg3);
    const char *bare = "smtp_host=h\npop3_host=p\nfrom=f@x\n";
    CHECK(mail_config_parse(&cfg3, bare, strlen(bare)) == 1);
    CHECK(cfg3.smtp_port == 0 && cfg3.pop3_port == 0);
    CHECK(mail_config_parse(&cfg3, "nothing here\n", 13) == 0);

    /* TLS / IMAP / pin / wrapped-password keys. */
    mail_config cfg4;
    mail_config_defaults(&cfg4);
    const char *conf4 =
        "smtp_host=s\npop3_host=p\nimap_host=im\nimap_port=993\n"
        "smtp_tls=1\npop3_tls=0\nimap_tls=1\n"
        "cert_pin=0123456789abcdef\npass_enc=aa:bb\nfrom=f@x\n";
    CHECK(mail_config_parse(&cfg4, conf4, strlen(conf4)) == 1);
    CHECK(str_eq(cfg4.imap_host, "im"));
    CHECK(cfg4.imap_port == 993);
    CHECK(cfg4.smtp_tls == 1 && cfg4.pop3_tls == 0 &&
          cfg4.imap_tls == 1);
    CHECK(str_eq(cfg4.cert_pin, "0123456789abcdef"));
    CHECK(str_eq(cfg4.pass_enc, "aa:bb"));
    CHECK(mail_config_ready(&cfg4) == 1);
    /* Serialize writes pass_enc and NOT a plain pass line. */
    char buf4[1024];
    size_t w4 = mail_config_serialize(&cfg4, buf4, sizeof(buf4));
    CHECK(w4 > 0);
    CHECK(strstr(buf4, "pass_enc=aa:bb\n") != NULL);
    CHECK(strstr(buf4, "\npass=") == NULL &&
          strncmp(buf4, "pass=", 5) != 0);
    mail_config cfg5;
    mail_config_defaults(&cfg5);
    CHECK(mail_config_parse(&cfg5, buf4, w4) == 1);
    CHECK(str_eq(cfg5.imap_host, "im") && cfg5.imap_port == 993);
    CHECK(cfg5.smtp_tls == 1 && cfg5.imap_tls == 1);
    CHECK(str_eq(cfg5.pass_enc, "aa:bb"));
    /* IMAP alone also counts as configured for receiving. */
    mail_config cfg6;
    mail_config_defaults(&cfg6);
    const char *conf6 = "smtp_host=s\nimap_host=im\nfrom=f@x\n";
    CHECK(mail_config_parse(&cfg6, conf6, strlen(conf6)) == 1);
    CHECK(mail_config_ready(&cfg6) == 1);

    /* --- mail_build2: date + MIME, no attachment --- */
    {
        char d[64];
        mail_format_date(d, sizeof(d), 2026, 11, 22, 6, 10, 30,
                         5, -10000);
        CHECK(str_eq(d, "Sat, 22 Nov 2026 10:30:05 -0000"));
        mail_format_date(d, sizeof(d), 2026, 11, 22, 6, 10, 30,
                         5, 540);
        CHECK(str_eq(d, "Sat, 22 Nov 2026 10:30:05 +0900"));
        char m[1024];
        int n = mail_build2("a@b", "c@d", "Sub", d, "Body\n.x",
                            NULL, m, sizeof(m));
        CHECK(n > 0);
        CHECK(strstr(m, "Date: Sat, 22 Nov 2026") != NULL);
        CHECK(strstr(m, "MIME-Version: 1.0\r\n") != NULL);
        CHECK(strstr(m, "Content-Type: text/plain") != NULL);
        CHECK(strstr(m, "\r\n..x") != NULL); /* dot-stuffed */
    }
    /* --- mail_build2: attachment becomes multipart --- */
    {
        static const unsigned char bin[4] = { 0, 1, 2, 250 };
        mail_attach att;
        att.name = "f.bin";
        att.data = bin;
        att.len = 4;
        char m[2048];
        int n = mail_build2("a@b", "c@d", "S", "", "Hello",
                            &att, m, sizeof(m));
        CHECK(n > 0);
        CHECK(strstr(m, "multipart/mixed; boundary=") != NULL);
        CHECK(strstr(m, "filename=\"f.bin\"") != NULL);
        CHECK(strstr(m, "AAEC+g==") != NULL);
        CHECK(strstr(m, "--\r\n") != NULL);
    }
    /* --- folders --- */
    {
        char f[64];
        const char *fl = "3=Work\n7=Trash\n";
        CHECK(mail_folder_of(fl, strlen(fl), 3, f, sizeof(f)) ==
              0);
        CHECK(str_eq(f, "Work"));
        CHECK(mail_folder_of(fl, strlen(fl), 1, f, sizeof(f)) ==
              0);
        CHECK(str_eq(f, "Inbox"));
        char out[128];
        int n = mail_folder_set(fl, strlen(fl), 1, "Work", out,
                                sizeof(out));
        CHECK(n > 0);
        CHECK(strstr(out, "1=Work\n") != NULL);
        CHECK(mail_folder_of(out, (size_t)n, 1, f, sizeof(f)) ==
              0);
        CHECK(str_eq(f, "Work"));
        n = mail_folder_set(out, (size_t)n, 1, "Inbox", out,
                            sizeof(out));
        CHECK(n > 0);
        CHECK(strstr(out, "1=") == NULL);
    }

    printf("mail tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
