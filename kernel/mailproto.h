/* PrismOS - mail over the network (SMTP send, POP3 and
 * IMAP receive).
 *
 * Real protocols over the line-oriented TCP stream in
 * kernel/netstack.c: SMTP (EHLO, MAIL FROM, RCPT TO, DATA,
 * QUIT), POP3 (USER, PASS, STAT, LIST, RETR, DELE, QUIT)
 * and IMAP (LOGIN, SELECT, FETCH BODY.PEEK[], LOGOUT;
 * IMAP deletes nothing on the server).
 *
 * TLS is supported through kernel/tls.c (TLS 1.2,
 * certificate pinning): when the config enables TLS for
 * a protocol, the whole session runs inside it (implicit
 * TLS from the first byte). The password is stored
 * wrapped (pass_enc) whenever a disk key is active, so it
 * is not kept in plain text on disk.
 *
 * The server settings come from "mail.conf" (see
 * shell/mail.h). The ports default to 25/465 (SMTP),
 * 110/995 (POP3) and 143/993 (IMAP) when the config
 * leaves them at 0.
 *
 * All progress is logged to the serial line, so the test
 * scripts can wait for it ("mail: sent ...",
 * "mail: received N message(s)").
 */
#ifndef PRISM_MAILPROTO_H
#define PRISM_MAILPROTO_H

#include <stddef.h>

#include "../shell/mail.h"

/* Send one message. Returns 0 on success. */
int mail_smtp_send(const mail_config *cfg, const char *to,
                   const char *subject, const char *body);

/* mail_smtp_send with one attachment (mail.h's
 * mail_attach) and an optional copy of the built
 * message (for the Sent folder). */
int mail_smtp_send_att(const mail_config *cfg, const char *to,
                       const char *subject, const char *body,
                       const mail_attach *att, char *built_out,
                       size_t built_cap, int *built_len);

/* Archive a built message as sent-NNNN.txt (the
 * Sent folder). Returns 0 on success. */
int mail_archive_sent(const char *built, size_t len);

/* Save every attachment of a received message into
 * the VFS (file names sanitised by the MIME layer).
 * Returns how many were saved. */
int mail_save_attachments(const char *msg, size_t len);

/* Fetch all messages from the POP3 server. For each
 * message, cb is called with the full message text (NUL
 * terminated, at most 4096 bytes); the callback returns 0
 * to keep going, non-zero to stop early. A message the
 * callback accepted is deleted from the server (DELE)
 * right away, so an interrupted fetch only ever
 * re-delivers the one message in flight. Returns the
 * number of messages fetched, or -1 on error. */
typedef int (*mail_fetch_cb)(const char *msg, size_t len,
                             void *ctx);
int mail_pop3_fetch(const mail_config *cfg, mail_fetch_cb cb,
                    void *ctx);

/* Fetch all messages from the IMAP INBOX. Same callback
 * contract as POP3, except nothing is deleted on the
 * server. Returns the number of messages fetched, or -1
 * on error. */
int mail_imap_fetch(const mail_config *cfg, mail_fetch_cb cb,
                    void *ctx);

#endif /* PRISM_MAILPROTO_H */
