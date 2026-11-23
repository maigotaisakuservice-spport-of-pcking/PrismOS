/* PrismOS - host tests for the TLS 1.2 client's pure
 * parts: AES-128-GCM (NIST vectors), the TLS PRF
 * (cross-checked against Python hmac/hashlib), RSA
 * public operations (cross-checked against Python's
 * pow()) and the X.509 RSA key parser (a real openssl
 * certificate, embedded below). The handshake itself is
 * exercised end to end by scripts/mail-test.py against
 * local TLS servers. */
#include <stdio.h>
#include <string.h>

#include "crypto.h"
#include "security.h"
#include "serial.h"
#include "tls.h"


/* ---- Stubs for tls.c's kernel-side dependencies (the
 * host test only exercises the pure helpers). ---- */
int net_tcp_open(const char *host, uint16_t port,
                 net_tcp_stream *s) {
    (void)host; (void)port; (void)s;
    return -1;
}
int net_tcp_send(net_tcp_stream *s, const char *data,
                 size_t len) {
    (void)s; (void)data; (void)len;
    return -1;
}
int net_tcp_recv(net_tcp_stream *s, uint8_t *out, size_t cap,
                 uint32_t timeout_sec) {
    (void)s; (void)out; (void)cap; (void)timeout_sec;
    return -1;
}
void net_tcp_close(net_tcp_stream *s) { (void)s; }
void serial_write_line(const char *s) { (void)s; }
void entropy_reseed(void) {
}

void entropy_add(uint32_t value) {
    (void)value;
}

void entropy_bytes(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)(0xA5 + i);
    }
}

static int checks;
static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            failures++;                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                              \
    } while (0)

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t unhex(const char *s, uint8_t *out) {
    size_t n = 0;
    while (s[2 * n] != '\0') {
        out[n] = (uint8_t)((hexval(s[2 * n]) << 4) | hexval(s[2 * n + 1]));
        n++;
    }
    return n;
}

static const uint8_t CERT_DER[] = {
    0x30, 0x82, 0x02, 0x04, 0x30, 0x82, 0x01, 0x6D, 0xA0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x14, 0x0B, 0x1A, 0x1F, 0x4E, 0xDE, 0xA3, 0x10, 0xC7, 0x33,
    0x42, 0x9F, 0x5D, 0x4D, 0x52, 0x41, 0xC2, 0x07, 0xA1, 0x8D, 0x0D, 0x30,
    0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B,
    0x05, 0x00, 0x30, 0x14, 0x31, 0x12, 0x30, 0x10, 0x06, 0x03, 0x55, 0x04,
    0x03, 0x0C, 0x09, 0x6D, 0x61, 0x69, 0x6C, 0x2E, 0x74, 0x65, 0x73, 0x74,
    0x30, 0x1E, 0x17, 0x0D, 0x32, 0x36, 0x31, 0x31, 0x32, 0x32, 0x30, 0x32,
    0x31, 0x32, 0x35, 0x39, 0x5A, 0x17, 0x0D, 0x33, 0x36, 0x31, 0x31, 0x31,
    0x39, 0x30, 0x32, 0x31, 0x32, 0x35, 0x39, 0x5A, 0x30, 0x14, 0x31, 0x12,
    0x30, 0x10, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0C, 0x09, 0x6D, 0x61, 0x69,
    0x6C, 0x2E, 0x74, 0x65, 0x73, 0x74, 0x30, 0x81, 0x9F, 0x30, 0x0D, 0x06,
    0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01, 0x05, 0x00,
    0x03, 0x81, 0x8D, 0x00, 0x30, 0x81, 0x89, 0x02, 0x81, 0x81, 0x00, 0xD3,
    0xDE, 0xB7, 0xD2, 0x97, 0xC5, 0xEB, 0x71, 0xE9, 0x5D, 0x3E, 0x61, 0xE2,
    0x26, 0xD3, 0xD4, 0x1F, 0xA9, 0x86, 0x37, 0x22, 0x0A, 0x9F, 0x98, 0xAA,
    0x62, 0xE5, 0x63, 0xF0, 0xCF, 0xC3, 0x04, 0xA0, 0x2D, 0x69, 0x98, 0xF8,
    0x85, 0x70, 0x19, 0xBC, 0xD8, 0x09, 0x83, 0x25, 0x7F, 0x24, 0x41, 0x9E,
    0xB7, 0x42, 0x13, 0x9F, 0xD2, 0x8A, 0xDD, 0xDF, 0x1F, 0x21, 0x19, 0x28,
    0x61, 0x18, 0x83, 0xCB, 0x68, 0x06, 0x1D, 0x0B, 0xCB, 0xED, 0x96, 0xB3,
    0x7E, 0x15, 0x5E, 0xEF, 0x7D, 0x58, 0x2D, 0x73, 0x6D, 0x51, 0xE0, 0xCE,
    0xB0, 0x94, 0x38, 0xD9, 0xBB, 0x1D, 0xCD, 0xFA, 0xAB, 0x20, 0x31, 0xDD,
    0xC8, 0x77, 0xBC, 0x94, 0x3B, 0xAC, 0x27, 0x5E, 0x65, 0xA4, 0x01, 0x33,
    0x15, 0xD3, 0xE1, 0x0D, 0xFA, 0x28, 0x1C, 0xDD, 0xC8, 0x23, 0x6A, 0x81,
    0x1B, 0xBA, 0xF6, 0x07, 0x34, 0x37, 0xDD, 0x02, 0x03, 0x01, 0x00, 0x01,
    0xA3, 0x53, 0x30, 0x51, 0x30, 0x1D, 0x06, 0x03, 0x55, 0x1D, 0x0E, 0x04,
    0x16, 0x04, 0x14, 0xE1, 0x1D, 0x57, 0xA1, 0xE5, 0xC6, 0x62, 0xFC, 0xC5,
    0x79, 0xBC, 0x1F, 0x25, 0xD2, 0x38, 0x19, 0x11, 0xE9, 0x6E, 0x72, 0x30,
    0x1F, 0x06, 0x03, 0x55, 0x1D, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14,
    0xE1, 0x1D, 0x57, 0xA1, 0xE5, 0xC6, 0x62, 0xFC, 0xC5, 0x79, 0xBC, 0x1F,
    0x25, 0xD2, 0x38, 0x19, 0x11, 0xE9, 0x6E, 0x72, 0x30, 0x0F, 0x06, 0x03,
    0x55, 0x1D, 0x13, 0x01, 0x01, 0xFF, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01,
    0xFF, 0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01,
    0x01, 0x0B, 0x05, 0x00, 0x03, 0x81, 0x81, 0x00, 0x72, 0x94, 0x94, 0xCA,
    0xF4, 0xE1, 0x13, 0xFC, 0x43, 0xBE, 0x6F, 0xD5, 0x21, 0x44, 0x72, 0x68,
    0x3B, 0xEE, 0x95, 0x5D, 0xA9, 0xD8, 0xA1, 0x99, 0x58, 0x42, 0xF2, 0xEE,
    0x9E, 0x24, 0xB8, 0xD7, 0x9F, 0x88, 0xBD, 0x4B, 0x82, 0xD2, 0x19, 0x9F,
    0x8E, 0x5A, 0xDC, 0xA4, 0xC5, 0xBA, 0xF8, 0xA9, 0x72, 0x24, 0xA8, 0x40,
    0xCA, 0x98, 0x94, 0x0C, 0x2B, 0x1F, 0xAA, 0xDA, 0x05, 0x86, 0xA6, 0x85,
    0x57, 0x24, 0x6F, 0x34, 0x62, 0xC5, 0x5E, 0x68, 0xA6, 0x2E, 0xB0, 0xFF,
    0xAB, 0xED, 0xDD, 0xFB, 0x5F, 0xEC, 0x8B, 0x72, 0x30, 0x82, 0x68, 0xE0,
    0x1B, 0x81, 0xB2, 0xEF, 0x46, 0xC4, 0xE7, 0x20, 0x4A, 0xD6, 0x5C, 0xCA,
    0xBF, 0x2B, 0x31, 0x94, 0xB9, 0x17, 0x02, 0xCA, 0xC6, 0x8B, 0x8E, 0xB1,
    0x60, 0xB6, 0xE0, 0x64, 0x28, 0x24, 0xA3, 0x7D, 0x0D, 0xC3, 0x61, 0xA7,
    0x9A, 0x1C, 0x8A, 0x6A,
};
#define CERT_DER_LEN (sizeof(CERT_DER))

int main(void) {
    /* ---- AES-128-GCM: NIST GCM test cases ---- */
    {
        uint8_t key[16] = {0};
        uint8_t nonce[12] = {0};
        uint8_t pt[16] = {0};
        uint8_t ct[16];
        uint8_t tag[16];
        uint8_t want_ct[16], want_tag[16];
        /* Case: K=0, IV=0, P=0^128, no AAD. */
        tls_gcm_encrypt(key, nonce, pt, 16, NULL, 0, ct, tag);
        unhex("0388dace60b6a392f328c2b971b2fe78", want_ct);
        unhex("ab6e47d42cec13bdf53a67b21257bddf", want_tag);
        CHECK(memcmp(ct, want_ct, 16) == 0);
        CHECK(memcmp(tag, want_tag, 16) == 0);
        uint8_t back[16];
        CHECK(tls_gcm_decrypt(key, nonce, ct, 16, NULL, 0, tag,
                              back) == 0);
        CHECK(memcmp(back, pt, 16) == 0);
        tag[0] ^= 1;
        CHECK(tls_gcm_decrypt(key, nonce, ct, 16, NULL, 0, tag,
                              back) != 0);
        /* Empty plaintext and AAD. */
        tls_gcm_encrypt(key, nonce, NULL, 0, NULL, 0, ct, tag);
        unhex("58e2fccefa7e3061367f1d57a4e7455a", want_tag);
        CHECK(memcmp(tag, want_tag, 16) == 0);
        /* With AAD (NIST case with AAD 20 bytes, P 60). */
        uint8_t k2[16], iv2[12], p2[60], a2[20], c2[60], t2[16];
        unhex("feffe9928665731c6d6a8f9467308308", k2);
        unhex("cafebabefacedbaddecaf888", iv2);
        unhex("d9313225f88406e5a55909c5aff5269a"
              "86a7a9531534f7da2e4c303d8a318a72"
              "1c3c0c95956809532fcf0e2449a6b525"
              "b16aedf5aa0de657ba637b39", p2);
        unhex("feedfacedeadbeeffeedfacedeadbeef"
              "abaddad2", a2);
        tls_gcm_encrypt(k2, iv2, p2, 60, a2, 20, c2, t2);
        uint8_t want_c2[60], want_t2[16];
        unhex("42831ec2217774244b7221b784d0d49c"
              "e3aa212f2c02a4e035c17e2329aca12e"
              "21d514b25466931c7d8f6a5aac84aa05"
              "1ba30b396a0aac973d58e091", want_c2);
        /* Tag for the 60-byte form of this vector
         * (cross-checked with Python's AESGCM). */
        unhex("5bc94fbc3221a5db94fae95ae7121a47", want_t2);
        CHECK(memcmp(c2, want_c2, 60) == 0);
        CHECK(memcmp(t2, want_t2, 16) == 0);
        uint8_t back2[60];
        CHECK(tls_gcm_decrypt(k2, iv2, c2, 60, a2, 20, t2,
                              back2) == 0);
        CHECK(memcmp(back2, p2, 60) == 0);
    }

    /* ---- TLS PRF: Python hmac/hashlib cross-check ---- */
    {
        uint8_t secret[16], seed[16], out[64], want[64];
        unhex("9bbe436ba940f017b17652849a71db35", secret);
        unhex("a0ba9f936cda311827a6f796ffd5198c", seed);
        unhex("e3f229ba727be17b8d122620557cd453c2aab21d07c3d495"
              "329b52d4e61edb5a6b301791e90d35c9c9a46b4e14baf9af"
              "0fa022f7077def17abfd3797c0564bab", want);
        tls_prf(secret, 16, "test label", seed, 16, out, 64);
        CHECK(memcmp(out, want, 64) == 0);
    }

    /* ---- RSA public op: Python pow() cross-check ---- */
    {
        uint8_t n[128], m[128], c[128], out[128];
        uint8_t e[3] = {0x01, 0x00, 0x01};
        unhex("d3deb7d297c5eb71e95d3e61e226d3d41fa98637220a9f98"
              "aa62e563f0cfc304a02d6998f8857019bcd80983257f244"
              "19eb742139fd28adddf1f211928611883cb68061d0bcbed"
              "96b37e155eef7d582d736d51e0ceb09438d9bb1dcdfaab2"
              "031ddc877bc943bac275e65a4013315d3e10dfa281cddc8"
              "236a811bbaf6073437dd", n);
        unhex("000102030405060708090a0b0c0d0e0f1011121314151617"
              "18191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
              "303132333435363738393a3b3c3d3e3f4041424344454647"
              "48494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
              "606162636465666768696a6b6c6d6e6f7071727374757677"
              "78797a7b7c7d7e7f", m);
        unhex("a9c546df6fb11e2149c84aae7cf602d6c6b23c29fab420da"
              "e126c5b1bae0d249d6ae633a530f9819b4d46d9a85a127e"
              "91171939993e58a713e911e42fe5e7c1f007468c640ff40"
              "d02a60fea0317fde9a045580b51a9032a9fccefeb24c517"
              "b87f9df59544699e0ba3fb5d78b672518641f2790bf16f26"
              "b0355145fc15ce8ddec", c);
        CHECK(tls_rsa_public(n, 128, e, 3, m, out) == 0);
        CHECK(memcmp(out, c, 128) == 0);
        /* Input >= modulus is rejected. */
        CHECK(tls_rsa_public(n, 128, e, 3, n, out) != 0);
    }

    /* ---- X.509: the embedded openssl certificate ---- */
    {
        const uint8_t *mod, *exp;
        size_t mod_len, exp_len;
        CHECK(tls_x509_rsa_key(CERT_DER, CERT_DER_LEN, &mod,
                               &mod_len, &exp, &exp_len) == 0);
        CHECK(mod_len == 128);
        uint8_t want_mod[128];
        unhex("d3deb7d297c5eb71e95d3e61e226d3d41fa98637220a9f98"
              "aa62e563f0cfc304a02d6998f8857019bcd80983257f244"
              "19eb742139fd28adddf1f211928611883cb68061d0bcbed"
              "96b37e155eef7d582d736d51e0ceb09438d9bb1dcdfaab2"
              "031ddc877bc943bac275e65a4013315d3e10dfa281cddc8"
              "236a811bbaf6073437dd", want_mod);
        CHECK(memcmp(mod, want_mod, 128) == 0);
        CHECK(exp_len == 3 && exp[0] == 0x01 && exp[1] == 0x00 &&
              exp[2] == 0x01);
        /* The pin fingerprint of this certificate. */
        uint8_t digest[32], want_fp[32];
        crypto_sha256(CERT_DER, CERT_DER_LEN, digest);
        unhex("12aee4758707f249beeeb4179f002f92d7d4e88fb704293"
              "c873ab4a1ee938ae1", want_fp);
        CHECK(memcmp(digest, want_fp, 32) == 0);
        /* Truncated input is rejected, not crashed on. */
        CHECK(tls_x509_rsa_key(CERT_DER, 40, &mod, &mod_len,
                               &exp, &exp_len) != 0);
    }

    printf("tls tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
