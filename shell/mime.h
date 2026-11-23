/* PrismOS - MIME support for mail (pure logic).
 *
 * What real mail needs beyond plain text:
 *   - Content-Transfer-Encoding: base64 and
 *     quoted-printable decoding (and base64 encoding
 *     for sending attachments),
 *   - multipart/mixed messages: finding the text/plain
 *     body part and the attachment parts,
 *   - RFC 2047 encoded words in headers
 *     (=?UTF-8?B?...?= / =?UTF-8?Q?...?=) decoded for
 *     display,
 *   - building a multipart message with one attachment.
 *
 * Charsets: bodies and headers are treated as UTF-8.
 * Encoded words in other charsets are passed through
 * undecoded (the charset conversion work is separate).
 */
#ifndef PRISM_MIME_H
#define PRISM_MIME_H

#include <stddef.h>
#include <stdint.h>

/* Base64-encode. Returns the byte count written
 * (without NUL) or -1 when out is too small. Lines are
 * wrapped at 76 columns with CRLF when wrap != 0. */
int mime_b64_encode(const uint8_t *in, size_t len, char *out,
                    size_t cap, int wrap);

/* Base64-decode (whitespace ignored, padding optional).
 * Returns the byte count or -1 on invalid input /
 * small output. */
int mime_b64_decode(const char *in, size_t len, uint8_t *out,
                    size_t cap);

/* Quoted-printable decode. Returns the byte count or
 * -1 when out is too small. */
int mime_qp_decode(const char *in, size_t len, uint8_t *out,
                   size_t cap);

/* Decode a transfer encoding ("base64",
 * "quoted-printable", anything else = identity) into
 * out. Returns the byte count or -1. */
int mime_decode_body(const char *encoding, const char *in,
                     size_t len, uint8_t *out, size_t cap);

/* Copy a header's raw value (unfolded) from a message:
 * field is "Content-Type" etc. Returns 0 on success. */
int mime_header(const char *msg, size_t len,
                const char *field, char *out, size_t cap);

/* Extract the parameter of a header value, e.g.
 * boundary from "multipart/mixed; boundary=XYZ".
 * Returns 0 on success. */
int mime_param(const char *header_value, const char *param,
               char *out, size_t cap);

/* Decode RFC 2047 encoded words in a header value for
 * display. Non-UTF-8 charsets and malformed words are
 * copied through unchanged. */
void mime_decode_header(const char *in, char *out, size_t cap);

/* Find the displayable text of a message: the first
 * text/plain part (or the whole body when the message
 * is not multipart), transfer-decoded into out
 * (NUL-terminated). Returns the length or -1. */
int mime_text_body(const char *msg, size_t len, char *out,
                   size_t cap);

/* Attachment iteration over a message. Call with
 * *cursor = 0 first; each call fills name (the part's
 * filename, sanitised to a VFS-safe name) and decodes
 * the part into data. Returns 1 when an attachment was
 * produced, 0 when there are no more, -1 when the next
 * attachment does not fit in data. */
int mime_next_attachment(const char *msg, size_t len,
                         size_t *cursor, char *name,
                         size_t name_cap, uint8_t *data,
                         size_t data_cap, size_t *data_len);

#endif /* PRISM_MIME_H */
