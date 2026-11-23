/* PrismOS - MIME logic host test (shell/mime.c). */
#include <stdio.h>
#include <string.h>

#include "mime.h"

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

static const char *MULTI =
    "From: a@example.jp\r\n"
    "Subject: =?UTF-8?B?44OG44K544OI?=\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"BB\"\r\n"
    "\r\n"
    "--BB\r\n"
    "Content-Type: text/plain; charset=UTF-8\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n"
    "\r\n"
    "Hello =E3=81=82\r\n"
    "--BB\r\n"
    "Content-Type: application/octet-stream; name=\"data.bin\"\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "Content-Disposition: attachment; filename=\"data.bin\"\r\n"
    "\r\n"
    "AAECAwQF\r\n"
    "--BB--\r\n";

int main(void) {
    /* base64 roundtrip incl. binary bytes */
    {
        uint8_t data[256];
        for (int i = 0; i < 256; i++) {
            data[i] = (uint8_t)i;
        }
        char enc[600];
        int en = mime_b64_encode(data, 256, enc, sizeof(enc), 1);
        CHECK(en > 0);
        uint8_t dec[300];
        int dn = mime_b64_decode(enc, (size_t)en, dec, sizeof(dec));
        CHECK(dn == 256);
        CHECK(memcmp(dec, data, 256) == 0);
        /* known vector */
        char e2[16];
        int n2 = mime_b64_encode((const uint8_t *)"Hi", 2, e2,
                                 sizeof(e2), 0);
        CHECK(n2 == 4 && memcmp(e2, "SGk=", 4) == 0);
        uint8_t d2[8];
        int m2 = mime_b64_decode("SGk=", 4, d2, sizeof(d2));
        CHECK(m2 == 2 && d2[0] == 'H' && d2[1] == 'i');
        /* unpadded + whitespace tolerant */
        int m3 = mime_b64_decode("S G\tk", 5, d2, sizeof(d2));
        CHECK(m3 == 2);
    }
    /* quoted-printable */
    {
        uint8_t out[64];
        const char *qin = "abc=20=3D\r\nd=e2=82=AC";
        int n = mime_qp_decode(qin, strlen(qin), out,
                               sizeof(out));
        CHECK(n == 11);
        CHECK(memcmp(out, "abc =\r\nd\xe2\x82\xac", 11) == 0);
        int bad = mime_qp_decode("x=ZZ", 4, out, sizeof(out));
        CHECK(bad == -1);
    }
    /* header + param parsing */
    {
        char v[128];
        CHECK(mime_header(MULTI, strlen(MULTI), "Content-Type",
                          v, sizeof(v)) == 0);
        char b[32];
        CHECK(mime_param(v, "boundary", b, sizeof(b)) == 0);
        CHECK(strcmp(b, "BB") == 0);
    }
    /* encoded word decode (UTF-8 B and Q) */
    {
        char out[64];
        mime_decode_header("=?UTF-8?B?44OG44K544OI?=", out,
                           sizeof(out));
        CHECK(strcmp(out, "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88") ==
              0);
        mime_decode_header("Re: =?UTF-8?Q?caf=C3=A9?= end", out,
                           sizeof(out));
        CHECK(strcmp(out, "Re: caf\xc3\xa9 end") == 0);
        /* unknown charsets are decoded as UTF-8 bytes */
        mime_decode_header("=?ISO-8859-1?B?QQ==?=", out,
                           sizeof(out));
        CHECK(strcmp(out, "A") == 0);
        /* Shift_JIS encoded word: decoded and converted */
        mime_decode_header("=?Shift_JIS?B?k/qWe4zq?=", out,
                           sizeof(out));
        CHECK(strcmp(out, "\u65e5\u672c\u8a9e") == 0);
    }
    /* single-part body with charset=shift_jis (8bit) */
    {
        char body[256];
        const char *msg =
            "Content-Type: text/plain; charset=shift_jis\r\n"
            "Content-Transfer-Encoding: 8bit\r\n"
            "\r\n"
            "\x93\xfa\x96\x7b\x8c\xea";
        int dn = mime_text_body(msg, strlen(msg), body,
                                sizeof(body));
        CHECK(dn == 9);
        CHECK(memcmp(body, "\u65e5\u672c\u8a9e", 9) == 0);
    }
    /* multipart: text body is the decoded text/plain part */
    {
        char body[256];
        int n = mime_text_body(MULTI, strlen(MULTI), body,
                               sizeof(body));
        CHECK(n > 0);
        CHECK(memcmp(body, "Hello \xe3\x81\x82", 9) == 0);
    }
    /* multipart: attachment extraction */
    {
        size_t cursor = 0;
        char name[64];
        uint8_t data[64];
        size_t dlen = 0;
        int rc = mime_next_attachment(MULTI, strlen(MULTI),
                                      &cursor, name, sizeof(name),
                                      data, sizeof(data), &dlen);
        CHECK(rc == 1);
        CHECK(strcmp(name, "data.bin") == 0);
        CHECK(dlen == 6);
        CHECK(data[0] == 0 && data[1] == 1 && data[5] == 5);
        rc = mime_next_attachment(MULTI, strlen(MULTI), &cursor,
                                  name, sizeof(name), data,
                                  sizeof(data), &dlen);
        CHECK(rc == 0);
    }
    /* multipart が境界行のまま (改行も後続も無く) 終わる
     * 細工入力: next_part が len - hs の下溢れでバッファ外を
     * 読まないこと (ファジング検出の回帰)。 */
    {
        const char *msg =
            "Content-Type: multipart/mixed; boundary=\"BB\"\r\n"
            "\r\n"
            "--BB";
        char body[64];
        int n = mime_text_body(msg, strlen(msg), body,
                               sizeof(body));
        CHECK(n <= 0);
        size_t cursor = 0;
        char name[64];
        uint8_t data[64];
        size_t dlen = 0;
        int rc = mime_next_attachment(msg, strlen(msg), &cursor,
                                      name, sizeof(name), data,
                                      sizeof(data), &dlen);
        CHECK(rc == 0);
    }
    /* single-part base64 body */
    {
        const char *msg =
            "From: x@y\r\nContent-Transfer-Encoding: base64\r\n"
            "\r\nSGVsbG8=\r\n";
        char body[64];
        int n = mime_text_body(msg, strlen(msg), body,
                               sizeof(body));
        CHECK(n == 5 && memcmp(body, "Hello", 5) == 0);
    }
    /* filename sanitising strips directories */
    {
        const char *msg =
            "Content-Type: multipart/mixed; boundary=Z\r\n\r\n"
            "--Z\r\nContent-Type: text/plain\r\n\r\nhi\r\n"
            "--Z\r\nContent-Disposition: attachment; "
            "filename=\"../evil/x.bin\"\r\n"
            "Content-Transfer-Encoding: base64\r\n\r\nQUJD\r\n"
            "--Z--\r\n";
        size_t cursor = 0;
        char name[64];
        uint8_t data[16];
        size_t dlen = 0;
        int rc = mime_next_attachment(msg, strlen(msg), &cursor,
                                      name, sizeof(name), data,
                                      sizeof(data), &dlen);
        CHECK(rc == 1);
        CHECK(strcmp(name, "x.bin") == 0);
        CHECK(dlen == 3 && memcmp(data, "ABC", 3) == 0);
    }
    printf("test_mime: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
