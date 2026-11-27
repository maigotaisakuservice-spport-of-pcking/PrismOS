/* PrismOS — Wi-Fi マネージャ (状態機械と保存済みネットワーク)
 *
 * 無線 LAN への接続を管理する中核ロジック。ハードウェアに
 * 直接触らない純粋な状態機械で、実ドライバは下のバックエンド
 * テーブル (probe / scan / connect / disconnect) を登録して
 * 差し込む設計。カーネルとホストテストで同じソースを使う。
 *
 * 正直な現状: 実機ドライバはまだ無い。製品ビルドでは
 * バックエンドが未登録 = 「ハードウェアなし」となり、UI は
 * その旨を表示する (接続できたフリはしない)。モックの
 * バックエンドはホストテスト (tests/test_wifi.c) 専用で、
 * 製品には持ち込まない。
 *
 * 保存済みネットワークのパスワードは平文で持たない。
 * 保存時は登録されたラップ関数 (デスクトップでは
 * persist_wrap_secret = ディスク鍵による AES-256-CTR) で
 * 包んだ文字列だけを保持し、接続時に unwrap して使う。
 * ラップできない環境では SSID だけを覚え、パスワードの
 * 再入力を求める (WIFI_ERR_NEED_PSK)。
 */
#ifndef PRISM_WIFI_H
#define PRISM_WIFI_H

#include <stddef.h>

#define WIFI_SSID_MAX 32   /* SSID の最大バイト数 (終端含まず) */
#define WIFI_PSK_MAX 63    /* パスフレーズの最大文字数 */
#define WIFI_MAX_NETS 16   /* スキャン結果の保持数 */
#define WIFI_MAX_SAVED 8   /* 保存済みネットワークの保持数 */
#define WIFI_WRAPPED_MAX 200 /* ラップ済みパスワード文字列の上限 */

typedef enum {
    WIFI_ST_OFF = 0,  /* オフ */
    WIFI_ST_NO_HW,    /* ハードウェアが見つからない */
    WIFI_ST_IDLE,     /* オン・未接続 */
    WIFI_ST_SCANNING, /* スキャン中 */
    WIFI_ST_CONNECTING, /* 接続処理中 */
    WIFI_ST_CONNECTED,  /* 接続済み */
    WIFI_ST_ERROR       /* 直前の操作が失敗した */
} wifi_state;

enum {
    WIFI_OK = 0,
    WIFI_ERR_NO_HW = -1,    /* ハードウェアなし */
    WIFI_ERR_OFF = -2,      /* Wi-Fi がオフ */
    WIFI_ERR_AUTH = -3,     /* 認証失敗 (パスワード違い等) */
    WIFI_ERR_NOT_FOUND = -4,/* ネットワークが見つからない */
    WIFI_ERR_BAD_ARG = -5,  /* 引数が不正 */
    WIFI_ERR_BACKEND = -6,  /* バックエンド側の失敗 */
    WIFI_ERR_NEED_PSK = -7  /* パスワード入力が必要 */
};

/* スキャンで見つかったネットワーク。 */
typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    int signal;  /* 0..100 */
    int secured; /* 0 = 暗号化なし */
} wifi_net;

/* 保存済みネットワーク。wrapped はラップ済みパスワード
 * (平文ではない)。空ならパスワードは保存されていない。 */
typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    char wrapped[WIFI_WRAPPED_MAX];
    int secured;
} wifi_saved;

/* 実ドライバが実装するバックエンド。connect の戻り値は
 * WIFI_OK または WIFI_ERR_* (認証失敗は WIFI_ERR_AUTH、
 * 見つからない場合は WIFI_ERR_NOT_FOUND)。 */
typedef struct wifi_backend {
    int (*probe)(void);
    int (*scan)(wifi_net *out, int max);
    int (*connect)(const char *ssid, const char *psk);
    void (*disconnect)(void);
} wifi_backend;

/* パスワードのラップ/展開 (persist 層が登録する)。
 * 成功で 0、失敗で 0 以外。 */
typedef int (*wifi_wrap_fn)(const char *plain, char *out,
                            size_t cap);
typedef int (*wifi_unwrap_fn)(const char *wrapped, char *out,
                              size_t cap);

/* バックエンドを登録して初期化する。be == NULL は
 * 「ドライバ未登録 = ハードウェアなし」。全状態は
 * リセットされ、オン/オフの既定はオン。 */
void wifi_init(const wifi_backend *be);

/* ラップ関数を登録する (どちらか一方だけでもよい)。 */
void wifi_set_secret_fns(wifi_wrap_fn wrap, wifi_unwrap_fn unwrap);

int wifi_hw_present(void);
wifi_state wifi_get_state(void);
int wifi_last_error(void); /* WIFI_ST_ERROR の理由 (WIFI_ERR_*) */
int wifi_get_enabled(void);

/* オン/オフを切り替える。オフにすると切断する。
 * ハードウェアが無くてもオンという設定自体は保持される
 * (状態は WIFI_ST_NO_HW になる)。 */
void wifi_set_enabled(int on);

/* スキャンを実行し、結果を信号の強い順に保持する。
 * 戻り値は見つかった数、または WIFI_ERR_*。 */
int wifi_scan(void);
int wifi_scan_count(void);
const wifi_net *wifi_scan_at(int i);

/* ssid へ接続する。暗号化ありで psk が空なら、保存済みの
 * パスワードを展開して使う。それも無ければ
 * WIFI_ERR_NEED_PSK を返し、UI にパスワード入力を求める。
 * 成功すると保存済み一覧へ登録/更新される (パスワードは
 * ラップできた場合だけ保存される)。 */
int wifi_connect(const char *ssid, const char *psk);

/* 保存済みネットワークへ、保存パスワードで接続する。 */
int wifi_connect_saved(const char *ssid);

void wifi_disconnect(void);
const char *wifi_current_ssid(void); /* 未接続なら "" */
int wifi_current_signal(void);

/* 保存済み一覧。 */
int wifi_saved_count(void);
const wifi_saved *wifi_saved_at(int i);
int wifi_saved_find(const char *ssid); /* index または -1 */
int wifi_saved_has_psk(const char *ssid);

/* 保存済みから削除する。接続中なら切断もする。 */
int wifi_forget(const char *ssid);

/* 永続化形式への変換。書式は
 *   PRISMWIFI1 <enabled>\n
 *   <ssid>\t<secured>\t<wrapped>\n  (保存済みごと)
 * SSID にタブや改行を含むものは登録自体を拒否する。
 * wifi_serialize は書き込んだバイト数 (NUL 含まず)、
 * バッファ不足なら 0 を返す。wifi_parse は成功で 0。 */
size_t wifi_serialize(char *buf, size_t cap);
int wifi_parse(const char *buf, size_t len);

/* 保存内容 (一覧とオン/オフ) が変わっていたら 1。
 * 永続化したら wifi_saved_mark_clean を呼ぶ。 */
int wifi_saved_dirty(void);
void wifi_saved_mark_clean(void);

#endif /* PRISM_WIFI_H */
