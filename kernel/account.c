/* PrismOS — ローカルアカウント: v2 は反復ハッシュ、v1 は FNV-1a。 */
#include "account.h"

#include "crypto.h"

static size_t acc_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int account_pin_ok(const char *pin) {
    if (pin == NULL) {
        return 0;
    }
    size_t n = acc_len(pin);
    if (n < 4 || n > 16) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)pin[i];
        if (c < 0x21 || c > 0x7E) {
            return 0;
        }
    }
    return 1;
}

int account_pin_ok_policy(const char *pin, int min_len,
                          int require_digit) {
    if (pin == NULL) {
        return 0;
    }
    if (min_len < 4) {
        min_len = 4;
    }
    if (min_len > 16) {
        min_len = 16;
    }
    size_t n = acc_len(pin);
    if (n < (size_t)min_len || n > 16) {
        return 0;
    }
    int has_digit = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)pin[i];
        if (c < 0x21 || c > 0x7E) {
            return 0;
        }
        if (c >= '0' && c <= '9') {
            has_digit = 1;
        }
    }
    if (require_digit && !has_digit) {
        return 0;
    }
    return 1;
}

uint64_t account_hash(const uint8_t *salt, const char *pin) {
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < 8; i++) {
        h ^= (uint64_t)salt[i];
        h *= 1099511628211ULL;
    }
    for (size_t i = 0; pin[i] != '\0'; i++) {
        h ^= (uint64_t)(unsigned char)pin[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_hex64(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int d = hex_val(s[i]);
        if (d < 0) {
            return -1;
        }
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return 0;
}

static const char *find_key(const char *text, size_t len, const char *key) {
    size_t klen = acc_len(key);
    if (len < klen) {
        return NULL;
    }
    for (size_t i = 0; i + klen <= len; i++) {
        size_t j = 0;
        while (j < klen && text[i + j] == key[j]) {
            j++;
        }
        if (j == klen) {
            return text + i + klen;
        }
    }
    return NULL;
}

static void hex_encode_bytes(const uint8_t *data, size_t len,
                             char *out) {
    static const char D[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = D[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = D[data[i] & 0xF];
    }
    out[len * 2] = '\0';
}

static int parse_hex_bytes(const char *s, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int hi = hex_val(s[i * 2]);
        int lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int parse_decimal(const char *s, uint32_t *out) {
    if (s == NULL || *s < '0' || *s > '9') {
        return -1;
    }
    uint32_t v = 0;
    size_t i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (uint32_t)(s[i] - '0');
        i++;
        if (v > 1000000u) {
            return -1;
        }
    }
    if (i == 0) {
        return -1;
    }
    *out = v;
    return 0;
}

int account_version(const char *text, size_t len) {
    if (text == NULL || len < 3) {
        return -1;
    }
    if (text[0] == 'v' && text[1] == '2') {
        return 2;
    }
    if (text[0] == 'v' && text[1] == '1') {
        return 1;
    }
    return -1;
}

int account_make_text(const uint8_t *salt, uint32_t iterations,
                      const char *pin, char *buf, size_t cap) {
    if (salt == NULL || buf == NULL || !account_pin_ok(pin)) {
        return -1;
    }
    if (iterations == 0 || iterations > 1000000u) {
        return -1;
    }
    uint8_t derived[32];
    crypto_kdf(pin, salt, PRISM_ACCOUNT_SALT_LEN, iterations, derived);
    char salt_hex[33];
    char hash_hex[65];
    char iter_buf[12];
    hex_encode_bytes(salt, PRISM_ACCOUNT_SALT_LEN, salt_hex);
    hex_encode_bytes(derived, 32, hash_hex);
    {
        uint32_t v = iterations;
        int d = 0;
        char tmp[12];
        if (v == 0) {
            tmp[d++] = '0';
        } else {
            while (v > 0) {
                tmp[d++] = (char)('0' + (v % 10));
                v /= 10;
            }
        }
        int q = 0;
        while (d > 0) {
            iter_buf[q++] = tmp[--d];
        }
        iter_buf[q] = '\0';
    }
    /* "v2\nsalt=<32>\niter=<n>\nhash=<64>\n" */
    if (cap < 128) {
        return -1;
    }
    size_t pos = 0;
    const char *parts[7] = { "v2\nsalt=", salt_hex, "\niter=", iter_buf,
                             "\nhash=", hash_hex, "\n" };
    for (int k = 0; k < 7; k++) {
        const char *p = parts[k];
        while (*p != '\0') {
            if (pos + 1 >= cap) {
                return -1;
            }
            buf[pos++] = *p++;
        }
    }
    buf[pos] = '\0';
    for (int i = 0; i < 32; i++) {
        derived[i] = 0;
    }
    return 0;
}

int account_get_salt(const char *text, size_t len, uint8_t *salt_out,
                     uint32_t *iter_out) {
    if (text == NULL || salt_out == NULL || len < 10) {
        return -1;
    }
    int ver = account_version(text, len);
    if (ver == 2) {
        const char *salt_s = find_key(text, len, "salt=");
        const char *iter_s = find_key(text, len, "iter=");
        if (salt_s == NULL || iter_s == NULL) {
            return -1;
        }
        if (parse_hex_bytes(salt_s, salt_out, PRISM_ACCOUNT_SALT_LEN) !=
            0) {
            return -1;
        }
        uint32_t iter = 0;
        if (parse_decimal(iter_s, &iter) != 0) {
            return -1;
        }
        if (iter_out != NULL) {
            *iter_out = iter;
        }
        return 0;
    }
    if (ver == 1) {
        const char *salt_s = find_key(text, len, "salt=");
        if (salt_s == NULL) {
            return -1;
        }
        if (parse_hex_bytes(salt_s, salt_out, 8) != 0) {
            return -1;
        }
        if (iter_out != NULL) {
            *iter_out = 0;
        }
        return 0;
    }
    return -1;
}

int account_verify(const char *pin, const char *text, size_t len) {
    if (pin == NULL || text == NULL || len < 10) {
        return -1;
    }
    int ver = account_version(text, len);
    if (ver == 2) {
        const char *salt_s = find_key(text, len, "salt=");
        const char *iter_s = find_key(text, len, "iter=");
        const char *hash_s = find_key(text, len, "hash=");
        if (salt_s == NULL || iter_s == NULL || hash_s == NULL) {
            return -1;
        }
        uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
        if (parse_hex_bytes(salt_s, salt, PRISM_ACCOUNT_SALT_LEN) != 0) {
            return -1;
        }
        uint32_t iter = 0;
        if (parse_decimal(iter_s, &iter) != 0) {
            return -1;
        }
        uint8_t want[32];
        if (parse_hex_bytes(hash_s, want, 32) != 0) {
            return -1;
        }
        uint8_t derived[32];
        crypto_kdf(pin, salt, PRISM_ACCOUNT_SALT_LEN, iter, derived);
        /* 早く抜けない比較 (タイミングで漏らさないため)。 */
        int diff = 0;
        for (int i = 0; i < 32; i++) {
            diff |= derived[i] ^ want[i];
            derived[i] = 0;
            want[i] = 0;
        }
        return diff == 0 ? 1 : 0;
    }
    if (ver == 1) {
        const char *salt_s = find_key(text, len, "salt=");
        const char *hash_s = find_key(text, len, "hash=");
        if (salt_s == NULL || hash_s == NULL) {
            return -1;
        }
        uint8_t salt[8];
        if (parse_hex_bytes(salt_s, salt, 8) != 0) {
            return -1;
        }
        uint64_t want = 0;
        if (parse_hex64(hash_s, &want) != 0) {
            return -1;
        }
        return account_hash(salt, pin) == want ? 1 : 0;
    }
    return -1;
}
