/* PrismOS — Wi-Fi マネージャ (kernel/wifi.c) のホストテスト。
 *
 * モックバックエンドはこのテスト専用 (製品には入らない)。
 * 状態機械・スキャン結果の取り込み・接続成功/失敗
 * (パスワード違い)・保存と復元の往復・切断・削除を、
 * カーネルと同じ kernel/wifi.c で検証する。実無線の接続は
 * ハードウェアが無いため検証できない (ドライバは未実装で、
 * 製品ビルドでは「ハードウェアなし」になる)。
 */
#include <stdio.h>
#include <string.h>

#include "wifi.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

/* ---- モックバックエンド (テスト専用) ---- */

static wifi_state state_seen_in_connect;

static int mock_probe(void) {
    return 0;
}

static int mock_scan(wifi_net *out, int max) {
    static const wifi_net nets[3] = {
        {"CafeNet", 55, 0},
        {"HomeNet", 80, 1},
        {"OfficeNet", 30, 1},
    };
    int n = 3;
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        out[i] = nets[i];
    }
    return n;
}

static int mock_connect(const char *ssid, const char *psk) {
    state_seen_in_connect = wifi_get_state();
    if (strcmp(ssid, "HomeNet") == 0) {
        return strcmp(psk, "correct-horse") == 0 ? WIFI_OK
                                                 : WIFI_ERR_AUTH;
    }
    if (strcmp(ssid, "OfficeNet") == 0) {
        return strcmp(psk, "office-pass") == 0 ? WIFI_OK
                                               : WIFI_ERR_AUTH;
    }
    if (strcmp(ssid, "CafeNet") == 0) {
        return WIFI_OK;
    }
    return WIFI_ERR_NOT_FOUND;
}

static int mock_disconnects = 0;

static void mock_disconnect(void) {
    mock_disconnects++;
}

static const wifi_backend mock_backend = {
    mock_probe, mock_scan, mock_connect, mock_disconnect
};

/* ---- テスト用のラップ (可逆な偽ラップ。製品では
 * persist_wrap_secret が使われる。平文と異なる文字列に
 * なることと、往復できることだけを担う) ---- */

static int fake_wrap(const char *plain, char *out, size_t cap) {
    size_t n = strlen(plain);
    if (cap < n + 3) {
        return -1;
    }
    out[0] = 'W';
    out[1] = ':';
    for (size_t i = 0; i < n; i++) {
        out[2 + i] = (char)(plain[i] ^ 0x5A);
    }
    out[2 + n] = '\0';
    return 0;
}

static int fake_unwrap(const char *wrapped, char *out, size_t cap) {
    if (wrapped[0] != 'W' || wrapped[1] != ':') {
        return -1;
    }
    size_t n = strlen(wrapped + 2);
    if (cap < n + 1) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)(wrapped[2 + i] ^ 0x5A);
    }
    out[n] = '\0';
    return 0;
}

static void fresh(void) {
    wifi_init(&mock_backend);
    wifi_set_secret_fns(fake_wrap, fake_unwrap);
}

int main(void) {
    /* ハードウェアなし (バックエンド未登録)。 */
    wifi_init(NULL);
    CHECK(!wifi_hw_present());
    CHECK(wifi_get_state() == WIFI_ST_NO_HW);
    CHECK(wifi_scan() == WIFI_ERR_NO_HW);
    CHECK(wifi_connect("HomeNet", "x") == WIFI_ERR_NO_HW);
    wifi_set_enabled(0);
    CHECK(wifi_get_state() == WIFI_ST_OFF);
    CHECK(wifi_scan() == WIFI_ERR_OFF);
    wifi_set_enabled(1);
    CHECK(wifi_get_state() == WIFI_ST_NO_HW);

    /* 通常系: 起動直後はオン・未接続。 */
    fresh();
    CHECK(wifi_hw_present());
    CHECK(wifi_get_state() == WIFI_ST_IDLE);
    CHECK(wifi_get_enabled() == 1);
    CHECK(strcmp(wifi_current_ssid(), "") == 0);

    /* スキャン: 信号の強い順に並ぶ。 */
    CHECK(wifi_scan() == 3);
    CHECK(wifi_scan_count() == 3);
    CHECK(strcmp(wifi_scan_at(0)->ssid, "HomeNet") == 0);
    CHECK(wifi_scan_at(0)->signal == 80);
    CHECK(wifi_scan_at(0)->secured == 1);
    CHECK(strcmp(wifi_scan_at(1)->ssid, "CafeNet") == 0);
    CHECK(wifi_scan_at(1)->secured == 0);
    CHECK(strcmp(wifi_scan_at(2)->ssid, "OfficeNet") == 0);
    CHECK(wifi_scan_at(3) == NULL);
    CHECK(wifi_get_state() == WIFI_ST_IDLE);

    /* オープンなネットワークへパスワードなしで接続。 */
    CHECK(wifi_connect("CafeNet", NULL) == WIFI_OK);
    CHECK(wifi_get_state() == WIFI_ST_CONNECTED);
    CHECK(strcmp(wifi_current_ssid(), "CafeNet") == 0);
    CHECK(state_seen_in_connect == WIFI_ST_CONNECTING);
    /* 接続できると保存済みに入る。 */
    CHECK(wifi_saved_find("CafeNet") >= 0);
    CHECK(wifi_saved_dirty() == 1);
    wifi_disconnect();
    CHECK(wifi_get_state() == WIFI_ST_IDLE);
    CHECK(mock_disconnects >= 1);

    /* 暗号化あり + パスワード未入力 = 入力が必要。 */
    CHECK(wifi_connect("HomeNet", NULL) == WIFI_ERR_NEED_PSK);
    CHECK(wifi_connect("HomeNet", "") == WIFI_ERR_NEED_PSK);
    CHECK(wifi_get_state() == WIFI_ST_IDLE);

    /* パスワード違い = 認証エラーで理由が残る。 */
    CHECK(wifi_connect("HomeNet", "wrong") == WIFI_ERR_AUTH);
    CHECK(wifi_get_state() == WIFI_ST_ERROR);
    CHECK(wifi_last_error() == WIFI_ERR_AUTH);
    CHECK(strcmp(wifi_current_ssid(), "") == 0);

    /* 正しいパスワードで接続。保存され、平文では残らない。 */
    CHECK(wifi_connect("HomeNet", "correct-horse") == WIFI_OK);
    CHECK(wifi_get_state() == WIFI_ST_CONNECTED);
    int hi = wifi_saved_find("HomeNet");
    CHECK(hi >= 0);
    CHECK(wifi_saved_has_psk("HomeNet") == 1);
    CHECK(strstr(wifi_saved_at(hi)->wrapped, "correct-horse") ==
          NULL);
    CHECK(wifi_saved_at(hi)->secured == 1);
    wifi_disconnect();

    /* 保存済みから接続 (パスワードの再入力なし)。 */
    CHECK(wifi_connect_saved("HomeNet") == WIFI_OK);
    CHECK(wifi_get_state() == WIFI_ST_CONNECTED);
    wifi_disconnect();
    /* 保存されていないものは connect_saved で弾く。 */
    CHECK(wifi_connect_saved("OfficeNet") == WIFI_ERR_NOT_FOUND);

    /* 存在しない SSID はバックエンドが NOT_FOUND。 */
    CHECK(wifi_connect("Nowhere", "pw") == WIFI_ERR_NOT_FOUND);
    CHECK(wifi_last_error() == WIFI_ERR_NOT_FOUND);

    /* 不正な SSID は拒否。 */
    CHECK(wifi_connect("", NULL) == WIFI_ERR_BAD_ARG);
    CHECK(wifi_connect("bad\tssid", NULL) == WIFI_ERR_BAD_ARG);
    CHECK(wifi_connect(NULL, NULL) == WIFI_ERR_BAD_ARG);

    /* 永続化の往復: 保存 → 直列化 → 初期化 → 復元。 */
    char buf[4096];
    size_t len = wifi_serialize(buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "PRISMWIFI1 1\n") == buf);
    CHECK(strstr(buf, "correct-horse") == NULL);
    fresh();
    CHECK(wifi_saved_count() == 0);
    CHECK(wifi_parse(buf, len) == 0);
    CHECK(wifi_saved_count() == 2);
    CHECK(wifi_saved_find("HomeNet") >= 0);
    CHECK(wifi_saved_find("CafeNet") >= 0);
    CHECK(wifi_saved_has_psk("HomeNet") == 1);
    CHECK(wifi_saved_has_psk("CafeNet") == 0);
    CHECK(wifi_get_enabled() == 1);
    CHECK(wifi_saved_dirty() == 0);
    /* 復元後も保存パスワードで接続できる。 */
    CHECK(wifi_connect_saved("HomeNet") == WIFI_OK);
    wifi_disconnect();

    /* オフ状態の往復。 */
    wifi_set_enabled(0);
    CHECK(wifi_get_state() == WIFI_ST_OFF);
    len = wifi_serialize(buf, sizeof(buf));
    CHECK(len > 0);
    fresh();
    CHECK(wifi_parse(buf, len) == 0);
    CHECK(wifi_get_enabled() == 0);
    CHECK(wifi_get_state() == WIFI_ST_OFF);
    CHECK(wifi_connect("CafeNet", NULL) == WIFI_ERR_OFF);
    wifi_set_enabled(1);

    /* 壊れたデータは拒否、ゴミ行は読み飛ばす。 */
    CHECK(wifi_parse("GARBAGE\n", 8) != 0);
    const char *mixed =
        "PRISMWIFI1 1\n"
        "junk line without tabs\n"
        "GoodNet\t1\tW:abc\n"
        "\t1\tW:x\n";
    fresh();
    CHECK(wifi_parse(mixed, strlen(mixed)) == 0);
    CHECK(wifi_saved_count() == 1);
    CHECK(wifi_saved_find("GoodNet") >= 0);

    /* 削除: 接続中に消すと切断される。 */
    fresh();
    CHECK(wifi_connect("OfficeNet", "office-pass") == WIFI_OK);
    CHECK(wifi_get_state() == WIFI_ST_CONNECTED);
    CHECK(wifi_forget("OfficeNet") == WIFI_OK);
    CHECK(wifi_get_state() == WIFI_ST_IDLE);
    CHECK(wifi_saved_find("OfficeNet") < 0);
    CHECK(wifi_forget("OfficeNet") == WIFI_ERR_NOT_FOUND);

    /* ラップ関数が無い環境では SSID だけ覚える。 */
    wifi_init(&mock_backend);
    CHECK(wifi_connect("HomeNet", "correct-horse") == WIFI_OK);
    CHECK(wifi_saved_find("HomeNet") >= 0);
    CHECK(wifi_saved_has_psk("HomeNet") == 0);
    wifi_disconnect();
    CHECK(wifi_connect_saved("HomeNet") == WIFI_ERR_NEED_PSK);

    printf("test_wifi: %d checks, %d failures\n", checks,
           failures);
    return failures != 0;
}
