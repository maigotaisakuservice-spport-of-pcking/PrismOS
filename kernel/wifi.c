/* PrismOS — Wi-Fi マネージャの実装 (kernel/wifi.h 参照)。
 * ハードウェア非依存の状態機械。文字列関数は自前の
 * kernel/string.h を使う (ホストテストでも同じソース)。 */

#include "wifi.h"

#include "string.h"

static const wifi_backend *w_be;
static int w_hw;
static int w_enabled;
static wifi_state w_state;
static int w_last_error;
static wifi_net w_nets[WIFI_MAX_NETS];
static int w_net_count;
static wifi_saved w_saved[WIFI_MAX_SAVED];
static int w_saved_count;
static char w_current[WIFI_SSID_MAX + 1];
static int w_current_signal;
static int w_dirty;
static wifi_wrap_fn w_wrap;
static wifi_unwrap_fn w_unwrap;

/* SSID として許せる文字列か (空・長すぎ・タブ/改行/制御
 * 文字は永続化形式を壊すので拒否する)。 */
static int ssid_valid(const char *s) {
    if (s == NULL) {
        return 0;
    }
    size_t n = strlen(s);
    if (n == 0 || n > WIFI_SSID_MAX) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) {
            return 0;
        }
    }
    return 1;
}

static void copy_ssid(char *dst, const char *src) {
    size_t i = 0;
    while (src[i] != '\0' && i < WIFI_SSID_MAX) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* オンで未接続のときの基本状態。 */
static wifi_state idle_state(void) {
    if (!w_enabled) {
        return WIFI_ST_OFF;
    }
    return w_hw ? WIFI_ST_IDLE : WIFI_ST_NO_HW;
}

void wifi_init(const wifi_backend *be) {
    w_be = be;
    w_hw = be != NULL && be->probe != NULL && be->probe() == 0;
    w_enabled = 1;
    w_last_error = WIFI_OK;
    w_net_count = 0;
    w_saved_count = 0;
    w_current[0] = '\0';
    w_current_signal = 0;
    w_dirty = 0;
    w_wrap = NULL;
    w_unwrap = NULL;
    w_state = idle_state();
}

void wifi_set_secret_fns(wifi_wrap_fn wrap, wifi_unwrap_fn unwrap) {
    w_wrap = wrap;
    w_unwrap = unwrap;
}

int wifi_hw_present(void) {
    return w_hw;
}

wifi_state wifi_get_state(void) {
    return w_state;
}

int wifi_last_error(void) {
    return w_last_error;
}

int wifi_get_enabled(void) {
    return w_enabled;
}

void wifi_set_enabled(int on) {
    on = on != 0;
    if (on == w_enabled && w_state != WIFI_ST_ERROR) {
        /* 同じ値でも状態だけ整える。 */
        if (w_state != WIFI_ST_CONNECTED &&
            w_state != WIFI_ST_CONNECTING &&
            w_state != WIFI_ST_SCANNING) {
            w_state = idle_state();
        }
        return;
    }
    w_enabled = on;
    w_dirty = 1;
    if (!on) {
        if (w_be != NULL && w_be->disconnect != NULL &&
            w_current[0] != '\0') {
            w_be->disconnect();
        }
        w_current[0] = '\0';
        w_current_signal = 0;
        w_net_count = 0;
    }
    w_state = idle_state();
    w_last_error = WIFI_OK;
}

int wifi_scan(void) {
    if (!w_enabled) {
        return WIFI_ERR_OFF;
    }
    if (!w_hw) {
        return WIFI_ERR_NO_HW;
    }
    wifi_state back = w_current[0] != '\0' ? WIFI_ST_CONNECTED
                                           : WIFI_ST_IDLE;
    w_state = WIFI_ST_SCANNING;
    wifi_net tmp[WIFI_MAX_NETS];
    int n = w_be->scan != NULL ? w_be->scan(tmp, WIFI_MAX_NETS)
                               : WIFI_ERR_BACKEND;
    if (n < 0) {
        w_state = back;
        w_last_error = WIFI_ERR_BACKEND;
        return WIFI_ERR_BACKEND;
    }
    if (n > WIFI_MAX_NETS) {
        n = WIFI_MAX_NETS;
    }
    /* 不正な SSID のエントリは捨てる。 */
    int m = 0;
    for (int i = 0; i < n; i++) {
        tmp[i].ssid[WIFI_SSID_MAX] = '\0';
        if (!ssid_valid(tmp[i].ssid)) {
            continue;
        }
        if (tmp[i].signal < 0) {
            tmp[i].signal = 0;
        }
        if (tmp[i].signal > 100) {
            tmp[i].signal = 100;
        }
        tmp[i].secured = tmp[i].secured != 0;
        w_nets[m++] = tmp[i];
    }
    /* 信号の強い順 (安定ソート)。 */
    for (int i = 1; i < m; i++) {
        wifi_net v = w_nets[i];
        int j = i - 1;
        while (j >= 0 && w_nets[j].signal < v.signal) {
            w_nets[j + 1] = w_nets[j];
            j--;
        }
        w_nets[j + 1] = v;
    }
    w_net_count = m;
    w_state = back;
    w_last_error = WIFI_OK;
    return m;
}

int wifi_scan_count(void) {
    return w_net_count;
}

const wifi_net *wifi_scan_at(int i) {
    if (i < 0 || i >= w_net_count) {
        return NULL;
    }
    return &w_nets[i];
}

int wifi_saved_count(void) {
    return w_saved_count;
}

const wifi_saved *wifi_saved_at(int i) {
    if (i < 0 || i >= w_saved_count) {
        return NULL;
    }
    return &w_saved[i];
}

int wifi_saved_find(const char *ssid) {
    if (ssid == NULL) {
        return -1;
    }
    for (int i = 0; i < w_saved_count; i++) {
        if (strcmp(w_saved[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

int wifi_saved_has_psk(const char *ssid) {
    int i = wifi_saved_find(ssid);
    return i >= 0 && w_saved[i].wrapped[0] != '\0';
}

/* 保存済みへ登録/更新する。psk が NULL でない場合は
 * ラップして保存する (失敗したらパスワードなしで覚える)。
 * psk が NULL なら既存のラップを保持する。 */
static void saved_upsert(const char *ssid, int secured,
                         const char *psk) {
    int i = wifi_saved_find(ssid);
    if (i < 0) {
        if (w_saved_count >= WIFI_MAX_SAVED) {
            /* 古いものから押し出す (先頭が最古)。 */
            for (int j = 1; j < w_saved_count; j++) {
                w_saved[j - 1] = w_saved[j];
            }
            w_saved_count--;
        }
        i = w_saved_count++;
        copy_ssid(w_saved[i].ssid, ssid);
        w_saved[i].wrapped[0] = '\0';
    }
    w_saved[i].secured = secured != 0;
    if (psk != NULL) {
        w_saved[i].wrapped[0] = '\0';
        if (secured && psk[0] != '\0' && w_wrap != NULL) {
            char buf[WIFI_WRAPPED_MAX];
            if (w_wrap(psk, buf, sizeof(buf)) == 0) {
                size_t k = 0;
                while (buf[k] != '\0' &&
                       k + 1 < sizeof(w_saved[i].wrapped)) {
                    w_saved[i].wrapped[k] = buf[k];
                    k++;
                }
                w_saved[i].wrapped[k] = '\0';
            }
        }
    }
    w_dirty = 1;
}

static int connect_internal(const char *ssid, const char *psk,
                            int secured, int known_secured) {
    if (!w_enabled) {
        w_last_error = WIFI_ERR_OFF;
        return WIFI_ERR_OFF;
    }
    if (!w_hw) {
        w_last_error = WIFI_ERR_NO_HW;
        w_state = WIFI_ST_NO_HW;
        return WIFI_ERR_NO_HW;
    }
    /* 暗号化の有無が分からない場合はスキャン結果と
     * 保存済みから引く。パスワードが明示された場合は
     * 暗号化ありとして扱う (非表示ネットワークへの
     * 手入力接続がこの経路)。 */
    if (!known_secured) {
        secured = 0;
        for (int i = 0; i < w_net_count; i++) {
            if (strcmp(w_nets[i].ssid, ssid) == 0) {
                secured = w_nets[i].secured;
                break;
            }
        }
        int si = wifi_saved_find(ssid);
        if (si >= 0) {
            secured = w_saved[si].secured;
        }
        if (psk != NULL && psk[0] != '\0') {
            secured = 1;
        }
    }
    char plain[WIFI_PSK_MAX + 1];
    const char *use_psk = psk;
    if (secured && (use_psk == NULL || use_psk[0] == '\0')) {
        /* 保存済みパスワードの展開を試す。 */
        int si = wifi_saved_find(ssid);
        if (si >= 0 && w_saved[si].wrapped[0] != '\0' &&
            w_unwrap != NULL &&
            w_unwrap(w_saved[si].wrapped, plain,
                     sizeof(plain)) == 0) {
            use_psk = plain;
        } else {
            w_last_error = WIFI_ERR_NEED_PSK;
            return WIFI_ERR_NEED_PSK;
        }
    }
    if (!secured) {
        use_psk = "";
    }
    w_state = WIFI_ST_CONNECTING;
    int rc = w_be != NULL && w_be->connect != NULL
                 ? w_be->connect(ssid, use_psk)
                 : WIFI_ERR_BACKEND;
    if (rc != WIFI_OK) {
        w_state = WIFI_ST_ERROR;
        w_last_error = rc;
        return rc;
    }
    copy_ssid(w_current, ssid);
    w_current_signal = 0;
    for (int i = 0; i < w_net_count; i++) {
        if (strcmp(w_nets[i].ssid, ssid) == 0) {
            w_current_signal = w_nets[i].signal;
            break;
        }
    }
    w_state = WIFI_ST_CONNECTED;
    w_last_error = WIFI_OK;
    /* 接続できたネットワークは保存する。パスワードを
     * 明示的に渡されたときだけラップし直す (保存済みの
     * 展開で繋がった場合は既存のラップを保持する)。 */
    saved_upsert(ssid, secured,
                 psk != NULL && psk[0] != '\0' ? psk : NULL);
    return WIFI_OK;
}

int wifi_connect(const char *ssid, const char *psk) {
    if (!ssid_valid(ssid)) {
        w_last_error = WIFI_ERR_BAD_ARG;
        return WIFI_ERR_BAD_ARG;
    }
    if (psk != NULL && strlen(psk) > WIFI_PSK_MAX) {
        w_last_error = WIFI_ERR_BAD_ARG;
        return WIFI_ERR_BAD_ARG;
    }
    return connect_internal(ssid, psk, 0, 0);
}

int wifi_connect_saved(const char *ssid) {
    if (!ssid_valid(ssid)) {
        w_last_error = WIFI_ERR_BAD_ARG;
        return WIFI_ERR_BAD_ARG;
    }
    int si = wifi_saved_find(ssid);
    if (si < 0) {
        w_last_error = WIFI_ERR_NOT_FOUND;
        return WIFI_ERR_NOT_FOUND;
    }
    return connect_internal(ssid, NULL, w_saved[si].secured, 1);
}

void wifi_disconnect(void) {
    if (w_be != NULL && w_be->disconnect != NULL &&
        w_current[0] != '\0') {
        w_be->disconnect();
    }
    w_current[0] = '\0';
    w_current_signal = 0;
    w_state = idle_state();
    w_last_error = WIFI_OK;
}

const char *wifi_current_ssid(void) {
    return w_current;
}

int wifi_current_signal(void) {
    return w_current_signal;
}

int wifi_forget(const char *ssid) {
    int i = wifi_saved_find(ssid);
    if (i < 0) {
        return WIFI_ERR_NOT_FOUND;
    }
    if (w_current[0] != '\0' && strcmp(w_current, ssid) == 0) {
        wifi_disconnect();
    }
    for (int j = i + 1; j < w_saved_count; j++) {
        w_saved[j - 1] = w_saved[j];
    }
    w_saved_count--;
    w_dirty = 1;
    return WIFI_OK;
}

int wifi_saved_dirty(void) {
    return w_dirty;
}

void wifi_saved_mark_clean(void) {
    w_dirty = 0;
}

/* ---- 永続化形式 ---- */

static size_t buf_put(char *buf, size_t cap, size_t p,
                      const char *s) {
    while (*s != '\0' && p + 1 < cap) {
        buf[p++] = *s++;
    }
    return p;
}

size_t wifi_serialize(char *buf, size_t cap) {
    if (buf == NULL || cap < 16) {
        return 0;
    }
    size_t p = 0;
    p = buf_put(buf, cap, p, "PRISMWIFI1 ");
    buf[p++] = w_enabled ? '1' : '0';
    if (p + 1 < cap) {
        buf[p++] = '\n';
    }
    for (int i = 0; i < w_saved_count; i++) {
        size_t need = strlen(w_saved[i].ssid) + 3 +
                      strlen(w_saved[i].wrapped) + 1;
        if (p + need + 1 > cap) {
            buf[p] = '\0';
            return 0; /* 途中まで書いたものは使わせない */
        }
        p = buf_put(buf, cap, p, w_saved[i].ssid);
        buf[p++] = '\t';
        buf[p++] = w_saved[i].secured ? '1' : '0';
        buf[p++] = '\t';
        p = buf_put(buf, cap, p, w_saved[i].wrapped);
        buf[p++] = '\n';
    }
    buf[p] = '\0';
    return p;
}

/* cur が指す行を ls/ll に入れ、次の行の先頭 (なければ
 * NULL) を返す。cur が範囲外なら ls は NULL。 */
static const char *scan_line(const char *cur, const char *end,
                             const char **ls, size_t *ll) {
    if (cur == NULL || cur >= end) {
        *ls = NULL;
        *ll = 0;
        return NULL;
    }
    const char *nl = cur;
    while (nl < end && *nl != '\n') {
        nl++;
    }
    *ls = cur;
    *ll = (size_t)(nl - cur);
    if (*ll > 0 && cur[*ll - 1] == '\r') {
        (*ll)--;
    }
    if (nl >= end) {
        return NULL;
    }
    return nl + 1;
}

int wifi_parse(const char *buf, size_t len) {
    if (buf == NULL) {
        return -1;
    }
    const char *end = buf + len;
    const char *ls;
    size_t ll;
    const char *next = scan_line(buf, end, &ls, &ll);
    if (ls == NULL || ll < 12 ||
        memcmp(ls, "PRISMWIFI1 ", 11) != 0) {
        return -1;
    }
    w_enabled = ls[11] != '0';
    w_saved_count = 0;
    while (next != NULL) {
        next = scan_line(next, end, &ls, &ll);
        if (ll == 0) {
            continue;
        }
        /* フィールド分割: ssid \t secured \t wrapped */
        size_t t1 = 0;
        while (t1 < ll && ls[t1] != '\t') {
            t1++;
        }
        if (t1 >= ll || t1 == 0 || t1 > WIFI_SSID_MAX) {
            continue;
        }
        size_t t2 = t1 + 1;
        while (t2 < ll && ls[t2] != '\t') {
            t2++;
        }
        if (t2 >= ll || t1 + 1 >= t2) {
            continue;
        }
        if (w_saved_count >= WIFI_MAX_SAVED) {
            continue;
        }
        wifi_saved *sv = &w_saved[w_saved_count];
        size_t k = 0;
        while (k < t1 && k < WIFI_SSID_MAX) {
            sv->ssid[k] = ls[k];
            k++;
        }
        sv->ssid[k] = '\0';
        if (!ssid_valid(sv->ssid)) {
            continue;
        }
        sv->secured = ls[t1 + 1] == '1';
        size_t wl = ll - (t2 + 1);
        if (wl >= WIFI_WRAPPED_MAX) {
            wl = WIFI_WRAPPED_MAX - 1;
        }
        size_t m = 0;
        while (m < wl) {
            sv->wrapped[m] = ls[t2 + 1 + m];
            m++;
        }
        sv->wrapped[m] = '\0';
        w_saved_count++;
    }
    w_state = w_current[0] != '\0' ? WIFI_ST_CONNECTED
                                   : idle_state();
    w_dirty = 0;
    return 0;
}
