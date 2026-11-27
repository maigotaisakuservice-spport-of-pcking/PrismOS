/* PrismOS — TOTP/HOTP tests: RFC 4226 counters, RFC 6238
 * time vectors (SHA-1), SHA-1 known answers, Base32. */
#include "../kernel/totp.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                        \
    do {                                                   \
        checks++;                                          \
        if (!(cond)) {                                     \
            failures++;                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                 \
        }                                                  \
    } while (0)

static void hex20(const uint8_t *d, char *out) {
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[i * 2] = H[d[i] >> 4];
        out[i * 2 + 1] = H[d[i] & 15];
    }
    out[40] = '\0';
}

int main(void) {
    /* SHA-1 known answers */
    {
        prism_sha1 c;
        uint8_t out[20];
        char hex[41];
        sha1_init(&c);
        sha1_update(&c, (const uint8_t *)"abc", 3);
        sha1_final(&c, out);
        hex20(out, hex);
        CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") ==
              0);
        sha1_init(&c);
        const char *m =
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha1_update(&c, (const uint8_t *)m, strlen(m));
        sha1_final(&c, out);
        hex20(out, hex);
        CHECK(strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") ==
              0);
        /* empty */
        sha1_init(&c);
        sha1_final(&c, out);
        hex20(out, hex);
        CHECK(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") ==
              0);
    }

    const uint8_t seed[] = "12345678901234567890";
    /* RFC 4226 HOTP vectors, 6 digits */
    static const uint32_t hotp_expect[10] = {
        755224, 287082, 359152, 969429, 338314,
        254676, 287922, 162583, 399871, 520489
    };
    for (uint64_t i = 0; i < 10; i++) {
        CHECK(hotp_code(seed, 20, i, 6) == hotp_expect[i]);
    }
    /* RFC 6238 TOTP vectors, 8 digits, period 30 */
    CHECK(totp_code(seed, 20, 59, 30, 8) == 94287082u);
    CHECK(totp_code(seed, 20, 1111111109u, 30, 8) == 7081804u);
    CHECK(totp_code(seed, 20, 1234567890u, 30, 8) == 89005924u);
    CHECK(totp_code(seed, 20, 2000000000u, 30, 8) == 69279037u);

    /* verify: current step and neighbours, wrong code no */
    {
        uint64_t t = 1234567890u;
        uint32_t code = totp_code(seed, 20, t, 30, 6);
        CHECK(totp_verify(seed, 20, t, code, 0) == 1);
        CHECK(totp_verify(seed, 20, t + 30, code, 1) == 1);
        CHECK(totp_verify(seed, 20, t + 60, code, 1) == 0);
        CHECK(totp_verify(seed, 20, t, code ^ 1, 1) == 0);
        CHECK(totp_verify(NULL, 0, t, code, 1) == 0);
    }

    /* Base32: RFC 4648 vectors */
    {
        char enc[64];
        uint8_t dec[64];
        CHECK(base32_encode((const uint8_t *)"f", 1, enc,
                            sizeof(enc)) > 0 &&
              strcmp(enc, "MY") == 0);
        CHECK(base32_encode((const uint8_t *)"fooba", 5, enc,
                            sizeof(enc)) > 0 &&
              strcmp(enc, "MZXW6YTB") == 0);
        int n = base32_decode("MZXW6YTB", dec, sizeof(dec));
        CHECK(n == 5 && memcmp(dec, "fooba", 5) == 0);
        n = base32_decode("mzxw6ytb", dec, sizeof(dec));
        CHECK(n == 5 && memcmp(dec, "fooba", 5) == 0);
        /* roundtrip 10 random-ish bytes */
        uint8_t raw[10] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01,
                            0x23, 0x45, 0x67, 0x89, 0xAB };
        CHECK(base32_encode(raw, 10, enc, sizeof(enc)) == 16);
        n = base32_decode(enc, dec, sizeof(dec));
        CHECK(n == 10 && memcmp(dec, raw, 10) == 0);
        CHECK(base32_decode("MZX!", dec, sizeof(dec)) == -1);
    }

    printf("totp tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
