/* PrismOS - mail (pure logic, no hardware, no network).
 *
 * A mail message is one plain-text file "mail-NNNN.txt"
 * (NNNN = 0001..9999) in the file system:
 *
 *   From: <address>
 *   To: <address>
 *   Subject: <subject>
 *   Date: <free text, may be empty>
 *
 *   <body, lines separated by \n>
 *
 * The body may hold at most the file size limit (16384
 * bytes) minus the headers; longer messages are stored
 * truncated (the caller logs that). Sending and receiving
 * over the network live in kernel/mailproto.c, which uses
 * the line-oriented TCP stream in kernel/netstack.c; this
 * file only knows the message format, the server settings
 * text ("mail.conf") and the reply-code rules of SMTP and
 * POP3, so the host unit tests can run it without a card.
 *
 * Limits (honest, by design): at most 64 files exist in
 * the file system and 36 of them are built in, so at most
 * 28 messages fit before the box is full.
 */
#ifndef PRISM_MAIL_H
#define PRISM_MAIL_H

#include <stddef.h>
#include <stdint.h>

/* Server settings, stored as text in "mail.conf".
 * Password storage: when a PIN account is logged in
 * (disk key active), the password is stored wrapped
 * with the disk key as pass_enc ("hex(iv):hex(ct)",
 * AES-256-CTR) and the plain "pass" line is not
 * written. With no PIN account there is no key, so
 * the password falls back to plain text in the file;
 * the file itself still sits inside the encrypted
 * user area once a PIN exists. The unwrap happens in
 * the shell/desktop config loaders, not here, so this
 * file stays hardware-free. */
typedef struct {
    char smtp_host[64];
    uint16_t smtp_port; /* 0 = unset, default 25 (465 with TLS) */
    char pop3_host[64];
    uint16_t pop3_port; /* 0 = unset, default 110 (995 with TLS) */
    char imap_host[64]; /* empty = receive over POP3 */
    uint16_t imap_port; /* 0 = unset, default 143 (993 with TLS) */
    int smtp_tls;       /* implicit TLS from connect */
    int pop3_tls;
    int imap_tls;
    /* SHA-256 fingerprint of the server certificate,
     * 64 lower-case hex chars. TLS connects only when
     * a pin is set and matches (pinning policy: there
     * is no CA store, so an unpinned TLS connection
     * would authenticate nobody). */
    char cert_pin[65];
    char user[64];
    char pass[64];
    char pass_enc[160]; /* wrapped password, see above */
    char from[96];
} mail_config;

void mail_config_defaults(mail_config *c);

/* Parse "key=value" lines. Unknown lines are ignored.
 * Returns 1 if at least one known key was read. */
int mail_config_parse(mail_config *c, const char *text, size_t len);

/* Write the settings as text into buf (NUL-terminated).
 * Returns the number of bytes written (without the NUL). */
size_t mail_config_serialize(const mail_config *c, char *buf,
                             size_t cap);

/* 1 when both servers and an address are set. */
int mail_config_ready(const mail_config *c);

/* Build one message (headers + blank line + body) with
 * CRLF line endings, dot-stuffing lines that start with
 * '.' (SMTP DATA rule), into out (NUL-terminated).
 * Returns the number of bytes written (without the NUL),
 * or -1 when the message does not fit. */
int mail_build(const char *from, const char *to,
               const char *subject, const char *body, char *out,
               size_t cap);

/* Read one header field ("From", "To", "Subject", "Date")
 * from a message. Returns 0 on success, -1 when the field
 * is missing. The value is copied without leading spaces
 * and without a trailing CR. */
int mail_parse_header(const char *text, const char *field,
                      char *out, size_t cap);

/* One attachment for mail_build2. */
typedef struct {
    const char *name;
    const uint8_t *data;
    size_t len;
} mail_attach;

/* Format an RFC 2822 date: "Sat, 22 Nov 2026 10:30:00
 * +0900" (weekday 0=Sunday). tz_minutes is the local
 * offset from UTC; pass -10000 for "-0000" (offset
 * unknown). */
void mail_format_date(char *out, size_t cap, int year,
                      int month, int day, int weekday,
                      int hour, int minute, int second,
                      int tz_minutes);

/* Like mail_build, but with an explicit Date value and
 * MIME structure: MIME-Version + Content-Type headers,
 * and a multipart/mixed body when att != NULL (the
 * attachment is base64). Returns bytes written or -1. */
int mail_build2(const char *from, const char *to,
                const char *subject, const char *date,
                const char *body, const mail_attach *att,
                char *out, size_t cap);

/* Folder assignment, stored as text in
 * "mail-folders.txt" with one "NNNN=folder" line per
 * moved message (unassigned = Inbox). mail_folder_of
 * writes the folder name of message n ("Inbox" when
 * unassigned). mail_folder_set builds the updated
 * file content with n assigned to folder ("Inbox"
 * removes the assignment). Both return 0 / the byte
 * count on success, -1 on failure. */
int mail_folder_of(const char *folders, size_t len, int n,
                   char *out, size_t cap);
int mail_folder_set(const char *folders, size_t len, int n,
                    const char *folder, char *out,
                    size_t cap);

/* Parse the 3-digit reply code at the start of a server
 * line ("220 ...", "+OK ...", "-ERR ..."). Returns the
 * code (220, 250, 354, ...) or -1 when the line has no
 * code. "+OK" reads as 200, "-ERR" as -1. */
int mail_reply_code(const char *line);

/* 1 when a reply line ends a (possibly multi-line)
 * reply: the 4th character is a space, or the line is
 * just the code. Multi-line replies look like
 * "250-..." followed by "250 ...". */
int mail_reply_done(const char *line, int code);

/* 1 when the code means success (2xx, or +OK). */
int mail_reply_ok(int code);

/* Remove POP3 dot-stuffing in place: a line "..x" becomes
 * ".x". The terminating line "." itself is left alone;
 * callers stop reading at it. Returns the new length. */
size_t mail_dot_unstuff(char *text, size_t len);

/* File name for message number n (1..9999): "mail-NNNN.txt".
 * Returns 0 on success, -1 when n is out of range. */
int mail_file_name(int n, char *out, size_t cap);

/* The message number in a "mail-NNNN.txt" name, or -1
 * when the name is not a message file. */
int mail_index_from_name(const char *name);

#endif /* PRISM_MAIL_H */
