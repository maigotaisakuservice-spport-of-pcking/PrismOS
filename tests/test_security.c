/* PrismOS — セキュリティのホストテスト (KDF、監査ログ、
 * 許可リスト、PIN ポリシー、リモート操作、エントロピー)。
 * `make test` で走る。 */
#include <stdio.h>
#include <stdint.h>

#include "account.h"
#include "crypto.h"
#include "security.h"
#include "settings.h"
#include "string.h"

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

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void hex64_of_u64(uint64_t v, char *buf) {
    static const char D[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        buf[i] = D[v & 0xF];
        v >>= 4;
    }
    buf[16] = '\0';
}

/* sink のテスト用: 最後に受け取ったイベントを覚える。 */
static char sink_event[32];
static char sink_detail[64];
static int sink_calls;

static void test_sink(const char *event, const char *detail) {
    size_t i = 0;
    while (event[i] != '\0' && i + 1 < sizeof(sink_event)) {
        sink_event[i] = event[i];
        i++;
    }
    sink_event[i] = '\0';
    i = 0;
    while (detail[i] != '\0' && i + 1 < sizeof(sink_detail)) {
        sink_detail[i] = detail[i];
        i++;
    }
    sink_detail[i] = '\0';
    sink_calls++;
}

int main(void) {
    uint8_t salt16[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                          9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t salt16b[16] = {16, 15, 14, 13, 12, 11, 10, 9,
                           8, 7, 6, 5, 4, 3, 2, 1};
    uint8_t k1[32];
    uint8_t k2[32];
    uint8_t k3[32];

    /* ---- KDF: 反復ハッシュ ---- */
    crypto_kdf("1234", salt16, 16, 10000, k1);
    crypto_kdf("1234", salt16, 16, 10000, k2);
    CHECK(bytes_eq(k1, k2, 32)); /* 決定的 */
    crypto_kdf("1235", salt16, 16, 10000, k2);
    CHECK(!bytes_eq(k1, k2, 32)); /* PIN が違うと違う */
    crypto_kdf("1234", salt16b, 16, 10000, k2);
    CHECK(!bytes_eq(k1, k2, 32)); /* ソルトが違うと違う */
    crypto_kdf("1234", salt16, 16, 1, k2);
    CHECK(!bytes_eq(k1, k2, 32)); /* 回数が違うと違う */
    crypto_kdf("1234", salt16, 16, 9999, k3);
    CHECK(!bytes_eq(k1, k3, 32));
    /* 1 回は SHA-256(salt || PIN) と同じ。 */
    {
        uint8_t buf[20];
        for (int i = 0; i < 16; i++) {
            buf[i] = salt16[i];
        }
        buf[16] = '1';
        buf[17] = '2';
        buf[18] = '3';
        buf[19] = '4';
        uint8_t want[32];
        crypto_sha256(buf, 20, want);
        CHECK(bytes_eq(k2, want, 32));
    }
    /* 0 回は 1 回として扱う。 */
    crypto_kdf("1234", salt16, 16, 0, k3);
    CHECK(bytes_eq(k2, k3, 32));

    /* ---- エントロピープール ---- */
    {
        uint8_t a[32];
        uint8_t b[32];
        uint8_t c[64];
        entropy_init();
        entropy_add(0x12345678u);
        entropy_add(0x9ABCDEF0u);
        entropy_bytes(a, 32);
        entropy_init();
        entropy_add(0x12345678u);
        entropy_add(0x9ABCDEF0u);
        entropy_bytes(b, 32);
        CHECK(bytes_eq(a, b, 32)); /* 同じ出来事なら同じ列 */
        entropy_init();
        entropy_add(0x12345678u);
        entropy_add(0x11111111u);
        entropy_bytes(b, 32);
        CHECK(!bytes_eq(a, b, 32)); /* 出来事が違うと違う */
        entropy_init();
        entropy_bytes(b, 32);
        CHECK(!bytes_eq(a, b, 32)); /* 出来事が無いと違う */
        entropy_init();
        entropy_add(42u);
        entropy_bytes(c, 64);
        CHECK(!bytes_eq(c, c + 32, 32)); /* 64 バイトは続く */
    }

    /* ---- アカウント v2 ---- */
    {
        char text[PRISM_ACCOUNT_TEXT_MAX];
        CHECK(account_make_text(salt16, 10000, "1234", text,
                                sizeof(text)) == 0);
        CHECK(account_version(text, strlen(text)) == 2);
        CHECK(account_verify("1234", text, strlen(text)) == 1);
        CHECK(account_verify("9999", text, strlen(text)) == 0);
        uint8_t got_salt[16];
        uint32_t got_iter = 0;
        CHECK(account_get_salt(text, strlen(text), got_salt,
                               &got_iter) == 0);
        CHECK(bytes_eq(got_salt, salt16, 16));
        CHECK(got_iter == 10000);
        CHECK(account_make_text(salt16, 10000, "abc", text,
                                sizeof(text)) == -1);
        CHECK(account_make_text(salt16, 0, "1234", text,
                                sizeof(text)) == -1);
        CHECK(account_verify("1234", "garbage", 7) == -1);
        CHECK(account_version("garbage", 7) == -1);
        /* v1 のファイルも読める (移行用)。 */
        uint8_t salt8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint64_t h = account_hash(salt8, "1234");
        char salt_hex[17];
        char hash_hex[17];
        static const char D[] = "0123456789abcdef";
        for (int i = 0; i < 8; i++) {
            salt_hex[i * 2] = D[(salt8[i] >> 4) & 0xF];
            salt_hex[i * 2 + 1] = D[salt8[i] & 0xF];
        }
        salt_hex[16] = '\0';
        hex64_of_u64(h, hash_hex);
        char v1[64];
        size_t p = 0;
        const char *parts[4] = {"v1\nsalt=", salt_hex, "\nhash=",
                                hash_hex};
        for (int k = 0; k < 4; k++) {
            const char *s = parts[k];
            while (*s != '\0') {
                v1[p++] = *s++;
            }
        }
        v1[p++] = '\n';
        v1[p] = '\0';
        CHECK(account_version(v1, p) == 1);
        CHECK(account_verify("1234", v1, p) == 1);
        CHECK(account_verify("9999", v1, p) == 0);
        uint8_t got8[16];
        uint32_t iter8 = 99;
        CHECK(account_get_salt(v1, p, got8, &iter8) == 0);
        CHECK(bytes_eq(got8, salt8, 8));
        CHECK(iter8 == 0);
        CHECK(account_verify("1234", "v1\nsalt=zz\nhash=00\n", 18) ==
              -1);
    }

    /* ---- PIN ポリシー ---- */
    CHECK(account_pin_ok_policy("1234", 4, 0) == 1);
    CHECK(account_pin_ok_policy("123", 4, 0) == 0);
    CHECK(account_pin_ok_policy("123456", 6, 0) == 1);
    CHECK(account_pin_ok_policy("12345", 6, 0) == 0);
    CHECK(account_pin_ok_policy("abcd", 4, 1) == 0);
    CHECK(account_pin_ok_policy("abc1", 4, 1) == 1);
    CHECK(account_pin_ok_policy("12 4", 4, 0) == 0);
    CHECK(account_pin_ok_policy("12345678901234567", 4, 0) == 0);
    CHECK(sec_pin_policy_ok("1234", 4, 0) == 1);
    CHECK(sec_pin_policy_ok("abcd", 4, 1) == 0);

    /* ---- 監査ログ ---- */
    {
        char log[AUDIT_LOG_CAP];
        log[0] = '\0';
        CHECK(audit_append(log, sizeof(log), "2026-11-21 13:00:00",
                           "login-ok", "PIN accepted") == 0);
        CHECK(audit_append(log, sizeof(log), "2026-11-21 13:01:00",
                           "rm", "secret.txt") == 0);
        CHECK(audit_append(log, sizeof(log), "2026-11-21 13:02:00",
                           "security", "usb=block") == 0);
        CHECK(audit_count(log, strlen(log)) == 3);
        int count = 0;
        int bad = 0;
        CHECK(audit_verify(log, strlen(log), &count, &bad) == 0);
        CHECK(count == 3);
        CHECK(bad == 0);
        /* detail を改ざんすると見つかる。 */
        char tampered[AUDIT_LOG_CAP];
        size_t n = 0;
        while (log[n] != '\0') {
            tampered[n] = log[n];
            n++;
        }
        tampered[n] = '\0';
        /* 2 件目の detail "secret.txt" を "secret.txy" にする。 */
        char *hit = NULL;
        for (size_t i = 0; i + 10 < n; i++) {
            if (tampered[i] == 's' && tampered[i + 1] == 'e' &&
                tampered[i + 2] == 'c' && tampered[i + 3] == 'r' &&
                tampered[i + 4] == 'e' && tampered[i + 5] == 't' &&
                tampered[i + 6] == '.' && tampered[i + 7] == 't') {
                hit = tampered + i;
                break;
            }
        }
        CHECK(hit != NULL);
        if (hit != NULL) {
            hit[8] = 'y'; /* "secret.txt" -> "secret.tyt" */
        }
        CHECK(audit_verify(tampered, strlen(tampered), &count, &bad) ==
              -1);
        CHECK(bad == 2);
        /* 時刻が 19 文字でないと追加できない。 */
        CHECK(audit_append(log, sizeof(log), "2026-11-21", "login-ok",
                           "x") == -1);
        /* たくさん追加しても連鎖は保たれる (古いものから捨てる)。 */
        for (int i = 0; i < 25; i++) {
            CHECK(audit_append(log, sizeof(log),
                               "2026-11-21 13:10:00", "login-ok",
                               "PIN accepted") == 0);
        }
        CHECK(audit_count(log, strlen(log)) <= AUDIT_MAX_ENTRIES);
        CHECK(audit_verify(log, strlen(log), &count, &bad) == 0);
        CHECK(count == audit_count(log, strlen(log)));
        /* 壊れた形式は -2。 */
        CHECK(audit_verify("not an audit line\n", 18, &count, &bad) ==
              -2);
    }

    /* ---- 監査 sink ---- */
    {
        sink_calls = 0;
        audit_set_sink(test_sink);
        audit_log_event("login-ok", "PIN accepted");
        CHECK(sink_calls == 1);
        CHECK(str_eq(sink_event, "login-ok"));
        CHECK(str_eq(sink_detail, "PIN accepted"));
        audit_set_sink(NULL);
        audit_log_event("login-ok", "PIN accepted");
        CHECK(sink_calls == 1); /* 未登録なら何もしない */
    }

    /* ---- 許可リスト ---- */
    {
        int allowed[SEC_APP_COUNT];
        CHECK(sec_app_id("music") == 7);
        CHECK(sec_app_id("docs") == 14);
        CHECK(sec_app_id("nope") == -1);
        CHECK(str_eq(sec_app_name(7), "music"));
        CHECK(str_eq(sec_app_name(99), ""));
        CHECK(allowlist_parse("all\n", 4, allowed) == 1);
        for (int i = 0; i < SEC_APP_COUNT; i++) {
            CHECK(allowed[i] == 1);
        }
        CHECK(allowlist_parse("", 0, allowed) == 0);
        for (int i = 0; i < SEC_APP_COUNT; i++) {
            CHECK(allowed[i] == 1);
        }
        CHECK(allowlist_parse("terminal,files,music\n", 21, allowed) ==
              3);
        CHECK(allowed[7] == 1);
        CHECK(allowed[0] == 1);
        CHECK(allowed[3] == 0);
        CHECK(sec_app_allowed(allowed, 7) == 1);
        CHECK(sec_app_allowed(allowed, 3) == 0);
        /* Terminal / Files / Settings は止められない。 */
        allowed[0] = 0;
        allowed[1] = 0;
        allowed[2] = 0;
        CHECK(sec_app_allowed(allowed, 0) == 1);
        CHECK(sec_app_allowed(allowed, 1) == 1);
        CHECK(sec_app_allowed(allowed, 2) == 1);
        CHECK(sec_app_allowed(NULL, 5) == 1);
        CHECK(sec_app_allowed(allowed, 99) == 0);
        /* 往復。 */
        char buf[256];
        size_t m = allowlist_serialize(allowed, buf, sizeof(buf));
        CHECK(m > 0);
        int back[SEC_APP_COUNT];
        allowlist_parse(buf, m, back);
        for (int i = 3; i < SEC_APP_COUNT; i++) {
            CHECK(back[i] == allowed[i]);
        }
        int all[SEC_APP_COUNT];
        for (int i = 0; i < SEC_APP_COUNT; i++) {
            all[i] = 1;
        }
        m = allowlist_serialize(all, buf, sizeof(buf));
        CHECK(str_eq(buf, "all\n"));
    }

    /* ---- リモート操作 ---- */
    {
        char action[16];
        char token[65];
        const char *body = "action=lock\ntoken=abcdef\n";
        CHECK(remote_parse(body, strlen(body), action, sizeof(action),
                           token, sizeof(token)) == 0);
        CHECK(str_eq(action, "lock"));
        CHECK(str_eq(token, "abcdef"));
        CHECK(remote_parse("token=xx\n", 9, action, sizeof(action),
                           token, sizeof(token)) == -1);
        const char *body2 = "action=wipe\n";
        CHECK(remote_parse(body2, strlen(body2), action, sizeof(action),
                           token, sizeof(token)) == 0);
        CHECK(str_eq(action, "wipe"));
        CHECK(str_eq(token, ""));
        /* 設定の往復。 */
        sec_remote_config cfg;
        remote_config_defaults(&cfg);
        CHECK(cfg.enabled == 0);
        CHECK(str_eq(cfg.path, "/prism-remote.txt"));
        size_t h = 0;
        const char *host = "10.0.2.2";
        while (host[h] != '\0') {
            cfg.host[h] = host[h];
            h++;
        }
        cfg.host[h] = '\0';
        const char *tok =
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        h = 0;
        while (tok[h] != '\0') {
            cfg.token[h] = tok[h];
            h++;
        }
        cfg.token[h] = '\0';
        cfg.enabled = 1;
        char buf[512];
        size_t m = remote_config_serialize(&cfg, buf, sizeof(buf));
        CHECK(m > 0);
        sec_remote_config back;
        remote_config_defaults(&back);
        CHECK(remote_config_parse(&back, buf, m) == 1);
        CHECK(str_eq(back.host, "10.0.2.2"));
        CHECK(str_eq(back.path, "/prism-remote.txt"));
        CHECK(str_eq(back.token, tok));
        CHECK(back.enabled == 1);
        CHECK(remote_config_parse(&back, "nonsense\n", 9) == 0);
    }

    /* ---- 設定の新キー ---- */
    {
        prism_settings s;
        settings_defaults(&s);
        CHECK(s.usb_media == 0);
        CHECK(s.pin_min_len == 4);
        CHECK(s.pin_require_digit == 0);
        CHECK(s.lockout_attempts == 3);
        CHECK(s.lockout_seconds == 300);
        char buf[1024];
        size_t n = settings_serialize(&s, buf, sizeof(buf));
        CHECK(n > 0);
        prism_settings s2;
        settings_defaults(&s2);
        s2.usb_media = 1;
        s2.pin_min_len = 6;
        s2.pin_require_digit = 1;
        s2.lockout_attempts = 5;
        s2.lockout_seconds = 60;
        n = settings_serialize(&s2, buf, sizeof(buf));
        prism_settings s3;
        settings_defaults(&s3);
        CHECK(settings_parse(&s3, buf, n) == 1);
        CHECK(s3.usb_media == 1);
        CHECK(s3.pin_min_len == 6);
        CHECK(s3.pin_require_digit == 1);
        CHECK(s3.lockout_attempts == 5);
        CHECK(s3.lockout_seconds == 60);
        /* 範囲外は無視される。 */
        CHECK(settings_parse(&s3, "pin_min_len=99\n", 15) == 0);
        CHECK(s3.pin_min_len == 6);
        CHECK(settings_parse(&s3, "lockout_seconds=99999\n", 22) == 0);
        CHECK(s3.lockout_seconds == 60);
    }

    printf("security tests: %d checks, %d failures\n", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
