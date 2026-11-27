/* PrismOS — 複数ユーザー (users.h 参照)。 */
#include "users.h"

#include "account.h"
#include "crypto.h"
#include "string.h"

static int is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int users_name_ok(const char *name) {
    if (name == NULL) {
        return 0;
    }
    size_t n = 0;
    while (name[n] != '\0') {
        if (!is_name_char(name[n]) || n >= PRISM_USER_NAME_MAX) {
            return 0;
        }
        n++;
    }
    if (n == 0) {
        return 0;
    }
    if (strcmp(name, "guest") == 0 || strcmp(name, "admin") == 0) {
        return 0;
    }
    return 1;
}

/* 1 行を取り出す。戻り値: 行の長さ (改行含まず)、
 * 次行があれば *next に進んだオフセット。 */
static size_t line_at(const char *text, size_t len, size_t off,
                      size_t *next) {
    size_t i = off;
    while (i < len && text[i] != '\n') {
        i++;
    }
    if (next != NULL) {
        *next = (i < len) ? i + 1 : i;
    }
    return i - off;
}

/* 行が "<kind> <name> " で始まるか。 */
static int line_is(const char *line, size_t llen, const char *kind,
                   const char *name) {
    size_t k = 0;
    while (kind[k] != '\0') {
        if (k >= llen || line[k] != kind[k]) {
            return 0;
        }
        k++;
    }
    if (k >= llen || line[k] != ' ') {
        return 0;
    }
    k++;
    size_t n = 0;
    while (name[n] != '\0') {
        if (k + n >= llen || line[k + n] != name[n]) {
            return 0;
        }
        n++;
    }
    size_t after = k + n;
    return after >= llen || line[after] == ' ';
}

/* 行の単語を取り出す (idx は 0 始まり)。成功 0。 */
static int word_at(const char *line, size_t llen, int idx, char *out,
                   size_t cap) {
    size_t i = 0;
    for (int w = 0; w <= idx; w++) {
        while (i < llen && line[i] == ' ') {
            i++;
        }
        size_t start = i;
        while (i < llen && line[i] != ' ') {
            i++;
        }
        if (w == idx) {
            size_t n = i - start;
            if (n == 0 || n + 1 > cap) {
                return -1;
            }
            memcpy(out, line + start, n);
            out[n] = '\0';
            return 0;
        }
        if (i >= llen) {
            return -1;
        }
    }
    return -1;
}

static const char *find_line(const char *text, size_t len,
                             const char *kind, const char *name,
                             size_t *llen_out) {
    size_t off = 0;
    while (off < len) {
        size_t next = off;
        size_t ll = line_at(text, len, off, &next);
        if (ll > 0 && line_is(text + off, ll, kind, name)) {
            if (llen_out != NULL) {
                *llen_out = ll;
            }
            return text + off;
        }
        off = next;
    }
    return NULL;
}

int users_count(const char *text, size_t len) {
    if (text == NULL) {
        return 0;
    }
    int count = 0;
    size_t off = 0;
    while (off < len) {
        size_t next = off;
        size_t ll = line_at(text, len, off, &next);
        if (ll > 5 && memcmp(text + off, "user ", 5) == 0) {
            count++;
        }
        off = next;
    }
    return count;
}

int users_exists(const char *text, size_t len, const char *name) {
    if (text == NULL || name == NULL) {
        return 0;
    }
    return find_line(text, len, "user", name, NULL) != NULL;
}

static int hexval(char c) {
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

static int hex_decode(const char *hex, size_t hexlen, uint8_t *out,
                      size_t cap) {
    if (hexlen % 2 != 0 || hexlen / 2 > cap) {
        return -1;
    }
    for (size_t i = 0; i < hexlen / 2; i++) {
        int hi = hexval(hex[i * 2]);
        int lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return (int)(hexlen / 2);
}

static void hex_encode(const uint8_t *in, size_t len, char *out) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 15];
    }
    out[len * 2] = '\0';
}

int users_verify(const char *text, size_t len, const char *name,
                 const char *pin) {
    size_t ll = 0;
    const char *line = find_line(text, len, "user", name, &ll);
    if (line == NULL) {
        return 0;
    }
    char salt_hex[40], iter_s[16], hash_hex[80];
    if (word_at(line, ll, 2, salt_hex, sizeof(salt_hex)) != 0 ||
        word_at(line, ll, 3, iter_s, sizeof(iter_s)) != 0 ||
        word_at(line, ll, 4, hash_hex, sizeof(hash_hex)) != 0) {
        return -1;
    }
    /* v2 の .account 形式に組み立てて検証を委ねる。 */
    char acct[PRISM_ACCOUNT_TEXT_MAX];
    size_t p = 0;
    const char *parts[6] = { "v2\nsalt=", salt_hex, "\niter=", iter_s,
                             "\nhash=", hash_hex };
    for (int i = 0; i < 6; i++) {
        const char *s = parts[i];
        if (i == 5) {
            /* hash の後に改行 */
        }
        while (*s != '\0' && p + 1 < sizeof(acct)) {
            acct[p++] = *s++;
        }
    }
    if (p + 2 >= sizeof(acct)) {
        return -1;
    }
    acct[p++] = '\n';
    acct[p] = '\0';
    return account_verify(pin, acct, p);
}

int users_make_line(const char *name, const uint8_t *salt,
                    uint32_t iterations, const char *pin,
                    const char *role, char *out, size_t cap) {
    char acct[PRISM_ACCOUNT_TEXT_MAX];
    if (account_make_text(salt, iterations, pin, acct,
                          sizeof(acct)) != 0) {
        return -1;
    }
    /* acct = "v2\nsalt=<32>\niter=<n>\nhash=<64>\n" から
     * 値を抜く。 */
    char salt_hex[40], hash_hex[80], iter_s[16];
    const char *sp = strstr(acct, "salt=");
    const char *ip = strstr(acct, "iter=");
    const char *hp = strstr(acct, "hash=");
    if (sp == NULL || ip == NULL || hp == NULL) {
        return -1;
    }
    memcpy(salt_hex, sp + 5, 32);
    salt_hex[32] = '\0';
    size_t in = 0;
    const char *q = ip + 5;
    while (*q >= '0' && *q <= '9' && in + 1 < sizeof(iter_s)) {
        iter_s[in++] = *q++;
    }
    iter_s[in] = '\0';
    memcpy(hash_hex, hp + 5, 64);
    hash_hex[64] = '\0';
    size_t p = 0;
    const char *parts[9] = { "user ", name, " ", salt_hex, " ",
                             iter_s, " ", hash_hex, NULL };
    for (int i = 0; parts[i] != NULL; i++) {
        const char *s = parts[i];
        while (*s != '\0') {
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = *s++;
        }
    }
    if (p + 2 >= cap) {
        return -1;
    }
    out[p++] = ' ';
    const char *r = role != NULL ? role : "user";
    while (*r != '\0') {
        if (p + 1 >= cap) {
            return -1;
        }
        out[p++] = *r++;
    }
    out[p] = '\0';
    return 0;
}

int users_salt_iter(const char *text, size_t len, const char *name,
                    uint8_t *salt_out, uint32_t *iter_out) {
    size_t ll = 0;
    const char *line = find_line(text, len, "user", name, &ll);
    if (line == NULL) {
        return -1;
    }
    char salt_hex[40], iter_s[16];
    if (word_at(line, ll, 2, salt_hex, sizeof(salt_hex)) != 0 ||
        word_at(line, ll, 3, iter_s, sizeof(iter_s)) != 0) {
        return -1;
    }
    if (hex_decode(salt_hex, 32, salt_out, PRISM_ACCOUNT_SALT_LEN) !=
        PRISM_ACCOUNT_SALT_LEN) {
        return -1;
    }
    uint32_t v = 0;
    for (size_t i = 0; iter_s[i] != '\0'; i++) {
        if (iter_s[i] < '0' || iter_s[i] > '9') {
            return -1;
        }
        v = v * 10 + (uint32_t)(iter_s[i] - '0');
    }
    if (iter_out != NULL) {
        *iter_out = v;
    }
    return 0;
}

int users_role(const char *text, size_t len, const char *name,
               char *out, size_t cap) {
    size_t ll = 0;
    const char *line = find_line(text, len, "user", name, &ll);
    if (line == NULL) {
        return -1;
    }
    return word_at(line, ll, 5, out, cap);
}

int users_get_wrap(const char *text, size_t len, const char *name,
                   uint8_t *wrap_out, uint8_t *check_out) {
    size_t ll = 0;
    const char *line = find_line(text, len, "wrap", name, &ll);
    if (line == NULL) {
        return -1;
    }
    char whex[80], chex[24];
    if (word_at(line, ll, 2, whex, sizeof(whex)) != 0 ||
        word_at(line, ll, 3, chex, sizeof(chex)) != 0) {
        return -1;
    }
    if (hex_decode(whex, 64, wrap_out, 32) != 32 ||
        hex_decode(chex, 16, check_out, 8) != 8) {
        return -1;
    }
    return 0;
}

static void wrap_check(const uint8_t *user_key, const uint8_t *mk,
                       uint8_t check_out[8]) {
    uint8_t buf[64];
    memcpy(buf, user_key, 32);
    memcpy(buf + 32, mk, 32);
    uint8_t digest[32];
    crypto_sha256(buf, sizeof(buf), digest);
    memcpy(check_out, digest, 8);
    memset(buf, 0, sizeof(buf));
    memset(digest, 0, sizeof(digest));
}

int users_make_wrap_line(const char *name, const uint8_t *mk,
                         const uint8_t *user_key, char *out,
                         size_t cap) {
    uint8_t wrap[32];
    uint8_t iv[16];
    memset(iv, 0, sizeof(iv));
    crypto_aes256_ctr(user_key, iv, mk, wrap, 32);
    uint8_t check[8];
    wrap_check(user_key, mk, check);
    char whex[80], chex[24];
    hex_encode(wrap, 32, whex);
    hex_encode(check, 8, chex);
    size_t p = 0;
    const char *parts[6] = { "wrap ", name, " ", whex, " ", chex };
    for (int i = 0; i < 6; i++) {
        const char *s = parts[i];
        while (*s != '\0') {
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = *s++;
        }
    }
    out[p] = '\0';
    memset(wrap, 0, sizeof(wrap));
    return 0;
}

/* チェック値の第 2 形式: 主鍵だけから作る (タグ付き
 * SHA-256 の先頭 8 バイト)。プライマリ PIN 変更時の
 * 再ラップでは利用者の鍵を知らないため、第 1 形式
 * (利用者の鍵と主鍵) を作り直せない。第 2 形式は
 * 主鍵の真正性の確認だけに使い、包み自体の秘匿は
 * 従来どおり利用者の鍵の鍵ストリームが担う。
 * 主鍵はプライマリ PIN の KDF 出力そのものなので、
 * 第 2 形式に対する総当たりの強さは .account の
 * 検証子と同等 (PIN の強さが上限) で、弱くはならない。 */
static void wrap_check_v2(const uint8_t *mk, uint8_t check_out[8]) {
    uint8_t buf[10 + 32];
    memcpy(buf, "PRISMWRAP2", 10);
    memcpy(buf + 10, mk, 32);
    uint8_t digest[32];
    crypto_sha256(buf, sizeof(buf), digest);
    memcpy(check_out, digest, 8);
    memset(buf, 0, sizeof(buf));
    memset(digest, 0, sizeof(digest));
}

static int check_matches(const uint8_t *a, const uint8_t *b) {
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

int users_unwrap(const uint8_t *wrap, const uint8_t *check,
                 const uint8_t *user_key, uint8_t *mk_out) {
    uint8_t iv[16];
    memset(iv, 0, sizeof(iv));
    crypto_aes256_ctr(user_key, iv, wrap, mk_out, 32);
    uint8_t expect[8];
    wrap_check(user_key, mk_out, expect);
    if (check_matches(expect, check)) {
        return 0;
    }
    /* 再ラップされた包み (第 2 形式) も受ける。 */
    wrap_check_v2(mk_out, expect);
    if (check_matches(expect, check)) {
        return 0;
    }
    memset(mk_out, 0, 32);
    return -1;
}

/* プライマリ PIN 変更時の全ユーザー再ラップ。
 * 包みは AES-CTR の鍵ストリームと主鍵の XOR なので、
 * 旧主鍵と新主鍵があれば利用者の鍵を知らなくても
 * 包み直せる: 新包み = 旧包み XOR 旧主鍵 XOR 新主鍵。
 * チェック値は第 2 形式で付け直す。users テキストの
 * wrap 行だけを書き換え、他はそのまま写す。 */
int users_rewrap_all(const char *text, size_t len,
                     const uint8_t *old_mk, const uint8_t *new_mk,
                     char *out, size_t cap, int *count_out) {
    size_t p = 0;
    size_t off = 0;
    int count = 0;
    if (count_out != NULL) {
        *count_out = 0;
    }
    while (off < len) {
        size_t eol = off;
        while (eol < len && text[eol] != '\n') {
            eol++;
        }
        size_t ll = eol - off;
        const char *line = text + off;
        int is_wrap = ll > 5 && line[0] == 'w' && line[1] == 'r' &&
                      line[2] == 'a' && line[3] == 'p' &&
                      line[4] == ' ';
        if (is_wrap) {
            char name[PRISM_USER_NAME_MAX + 1];
            char whex[80], chex[24];
            uint8_t wrap[32], checkb[8];
            if (word_at(line, ll, 1, name, sizeof(name)) != 0 ||
                word_at(line, ll, 2, whex, sizeof(whex)) != 0 ||
                word_at(line, ll, 3, chex, sizeof(chex)) != 0 ||
                hex_decode(whex, 64, wrap, 32) != 32 ||
                hex_decode(chex, 16, checkb, 8) != 8) {
                return -1;
            }
            uint8_t nwrap[32];
            for (int i = 0; i < 32; i++) {
                nwrap[i] = (uint8_t)(wrap[i] ^ old_mk[i] ^
                                     new_mk[i]);
            }
            uint8_t ncheck[8];
            wrap_check_v2(new_mk, ncheck);
            char nhex[80], nchex[24];
            hex_encode(nwrap, 32, nhex);
            hex_encode(ncheck, 8, nchex);
            const char *parts[6] = { "wrap ", name, " ", nhex,
                                     " ",    nchex };
            for (int i = 0; i < 6; i++) {
                const char *s = parts[i];
                while (*s != '\0') {
                    if (p + 1 >= cap) {
                        return -1;
                    }
                    out[p++] = *s++;
                }
            }
            memset(nwrap, 0, sizeof(nwrap));
            count++;
        } else {
            for (size_t i = 0; i < ll; i++) {
                if (p + 1 >= cap) {
                    return -1;
                }
                out[p++] = line[i];
            }
        }
        if (p + 1 >= cap) {
            return -1;
        }
        out[p++] = '\n';
        off = eol + 1;
    }
    if (p >= cap) {
        return -1;
    }
    out[p] = '\0';
    if (count_out != NULL) {
        *count_out = count;
    }
    return 0;
}

int users_upsert(const char *text, size_t len, const char *line,
                 char *out, size_t cap) {
    /* line の種類と名前 */
    char kind[8], name[PRISM_USER_NAME_MAX + 1];
    if (word_at(line, strlen(line), 0, kind, sizeof(kind)) != 0 ||
        word_at(line, strlen(line), 1, name, sizeof(name)) != 0) {
        return -1;
    }
    size_t p = 0;
    int replaced = 0;
    size_t off = 0;
    if (text == NULL) {
        len = 0;
    }
    while (off < len) {
        size_t next = off;
        size_t ll = line_at(text, len, off, &next);
        int is_target =
            ll > 0 && line_is(text + off, ll, kind, name);
        if (is_target && !replaced) {
            replaced = 1;
            const char *s = line;
            while (*s != '\0') {
                if (p + 1 >= cap) {
                    return -1;
                }
                out[p++] = *s++;
            }
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = '\n';
        } else if (!is_target) {
            if (p + ll + 1 >= cap) {
                return -1;
            }
            memcpy(out + p, text + off, ll);
            p += ll;
            out[p++] = '\n';
        }
        off = next;
    }
    if (!replaced) {
        const char *s = line;
        while (*s != '\0') {
            if (p + 1 >= cap) {
                return -1;
            }
            out[p++] = *s++;
        }
        if (p + 1 >= cap) {
            return -1;
        }
        out[p++] = '\n';
    }
    if (p >= cap) {
        return -1;
    }
    out[p] = '\0';
    return 0;
}

int users_remove_user(const char *text, size_t len,
                      const char *name, char *out, size_t cap) {
    size_t p = 0;
    size_t off = 0;
    if (text == NULL) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    while (off < len) {
        size_t next = off;
        size_t ll = line_at(text, len, off, &next);
        int drop = ll > 0 &&
                   (line_is(text + off, ll, "user", name) ||
                    line_is(text + off, ll, "wrap", name) ||
                    line_is(text + off, ll, "totp", name));
        if (!drop) {
            if (p + ll + 1 >= cap) {
                return -1;
            }
            memcpy(out + p, text + off, ll);
            p += ll;
            out[p++] = '\n';
        }
        off = next;
    }
    if (p >= cap) {
        return -1;
    }
    out[p] = '\0';
    return 0;
}

int users_totp(const char *text, size_t len, const char *name,
               char *out, size_t cap) {
    size_t ll = 0;
    const char *line = find_line(text, len, "totp", name, &ll);
    if (line == NULL) {
        return -1;
    }
    return word_at(line, ll, 2, out, cap);
}
