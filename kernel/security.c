/* PrismOS — セキュリティの純粋ロジック (フェーズ14)。 */
#include "security.h"

#include "crypto.h"
#include "entropy_hw.h"
#include "string.h"

/* ---- 小物 ---- */

static uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static size_t sec_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void hex_encode(const uint8_t *data, size_t len, char *out) {
    static const char D[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = D[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = D[data[i] & 0xF];
    }
    out[len * 2] = '\0';
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

/* ---- エントロピープール ---- */

static uint32_t ent_pool[8];
static uint32_t ent_pos;
static uint32_t ent_counter;

void entropy_init(void) {
    for (int i = 0; i < 8; i++) {
        ent_pool[i] = 0;
    }
    ent_pos = 0;
    ent_counter = 0;
}

void entropy_add(uint32_t value) {
    uint32_t i = ent_pos % 8;
    ent_pool[i] ^= value + 0x9E3779B9u + (ent_pos << 6);
    ent_pool[i] = rotl32(ent_pool[i], 7) ^ (value >> 3);
    ent_pos++;
}

/* 再種入れ: ハードウェア乱数 (あれば 4 回分) と
 * タイムスタンプカウンタをプールへ混ぜ直す。TLS 接続の
 * 開始時や秘密の生成前に呼び、起動時に 1 回だけ入れた
 * 種へ全秘密がぶら下がらないようにする。RDRAND が無い
 * 環境では TSC と入力イベントのタイミングが頼りで、
 * 強さは落ちる (tls.c の冒頭にも明記)。 */
void entropy_reseed(void) {
    for (int i = 0; i < 4; i++) {
        uint64_t r = 0;
        if (entropy_hw_random(&r) == 0) {
            entropy_add((uint32_t)r);
            entropy_add((uint32_t)(r >> 32));
        }
    }
    entropy_add(entropy_hw_tick());
    entropy_add(entropy_hw_tick() ^ 0xA5A5A5A5u);
}

void entropy_bytes(uint8_t *out, size_t n) {
    if (out == NULL) {
        return;
    }
    size_t done = 0;
    while (done < n) {
        /* SHA-256(プール 32 バイト || カウンタ 4 バイト)。 */
        uint8_t buf[36];
        for (int i = 0; i < 8; i++) {
            buf[i * 4] = (uint8_t)((ent_pool[i] >> 24) & 0xFF);
            buf[i * 4 + 1] = (uint8_t)((ent_pool[i] >> 16) & 0xFF);
            buf[i * 4 + 2] = (uint8_t)((ent_pool[i] >> 8) & 0xFF);
            buf[i * 4 + 3] = (uint8_t)(ent_pool[i] & 0xFF);
        }
        buf[32] = (uint8_t)((ent_counter >> 24) & 0xFF);
        buf[33] = (uint8_t)((ent_counter >> 16) & 0xFF);
        buf[34] = (uint8_t)((ent_counter >> 8) & 0xFF);
        buf[35] = (uint8_t)(ent_counter & 0xFF);
        ent_counter++;
        uint8_t digest[32];
        crypto_sha256(buf, sizeof(buf), digest);
        size_t take = n - done;
        if (take > 32) {
            take = 32;
        }
        for (size_t i = 0; i < take; i++) {
            out[done + i] = digest[i];
        }
        done += take;
    }
}

/* ---- 監査ログ ---- */

/* 1 行を組み立てる。prev_hex は 64 桁の 16 進 (無いときは
 * 64 個の '0')。out には NUL 込みで入る。書いたバイト数
 * (NUL 除く) を返す。入らなければ -1。 */
static int audit_build_line(int seq, const char *time19,
                            const char *event, const char *detail,
                            const char *prev_hex, char *out, size_t cap) {
    /* hash の入力: prev|seq|time|event|detail */
    char hash_in[64 + 1 + 10 + 1 + 19 + 1 + 24 + 1 + 48 + 1];
    size_t p = 0;
    const char *parts[9];
    char seq_buf[12];
    parts[0] = prev_hex;
    parts[1] = "|";
    {
        int v = seq;
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
            seq_buf[q++] = tmp[--d];
        }
        seq_buf[q] = '\0';
    }
    parts[2] = seq_buf;
    parts[3] = "|";
    parts[4] = time19;
    parts[5] = "|";
    parts[6] = event;
    parts[7] = "|";
    parts[8] = detail;
    for (int k = 0; k < 9; k++) {
        const char *s = parts[k];
        while (*s != '\0' && p + 1 < sizeof(hash_in)) {
            hash_in[p++] = *s++;
        }
    }
    hash_in[p] = '\0';
    uint8_t digest[32];
    crypto_sha256((const uint8_t *)hash_in, p, digest);
    char hash_hex[65];
    hex_encode(digest, 32, hash_hex);
    /* 行: seq=<n> time=<19> event=<名前> detail=<内容>
     *     prev=<64> hash=<64>\n */
    char line[256];
    size_t q = 0;
    const char *chunks[11];
    chunks[0] = "seq=";
    chunks[1] = seq_buf;
    chunks[2] = " time=";
    chunks[3] = time19;
    chunks[4] = " event=";
    chunks[5] = event;
    chunks[6] = " detail=";
    chunks[7] = detail;
    chunks[8] = " prev=";
    chunks[9] = prev_hex;
    chunks[10] = " hash=";
    for (int k = 0; k < 11; k++) {
        const char *s = chunks[k];
        while (*s != '\0' && q + 1 < sizeof(line)) {
            line[q++] = *s++;
        }
    }
    for (int k = 0; k < 64 && q + 1 < sizeof(line); k++) {
        line[q++] = hash_hex[k];
    }
    if (q + 2 > sizeof(line)) {
        return -1;
    }
    line[q++] = '\n';
    line[q] = '\0';
    if (q + 1 > cap) {
        return -1;
    }
    for (size_t i = 0; i <= q; i++) {
        out[i] = line[i];
    }
    return (int)q;
}

/* log から最後のエントリの seq と hash を取り出す。
 * 1 件も無ければ seq=0、hash は 64 個の '0'。 */
static void audit_last(const char *log, size_t len, int *seq_out,
                       char *hash_out) {
    *seq_out = 0;
    for (int i = 0; i < 64; i++) {
        hash_out[i] = '0';
    }
    hash_out[64] = '\0';
    if (log == NULL || len == 0) {
        return;
    }
    /* 最後の "seq=" を探す。 */
    size_t last = 0;
    int found = 0;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (log[i] == 's' && log[i + 1] == 'e' && log[i + 2] == 'q' &&
            log[i + 3] == '=') {
            last = i;
            found = 1;
        }
    }
    if (!found) {
        return;
    }
    int v = 0;
    size_t i = last + 4;
    while (i < len && log[i] >= '0' && log[i] <= '9') {
        v = v * 10 + (log[i] - '0');
        i++;
    }
    *seq_out = v;
    /* 同じ行の " hash=" を探す。 */
    for (size_t j = last; j + 6 <= len && log[j] != '\n'; j++) {
        if (log[j] == ' ' && log[j + 1] == 'h' && log[j + 2] == 'a' &&
            log[j + 3] == 's' && log[j + 4] == 'h' && log[j + 5] == '=') {
            size_t k = j + 6;
            for (int n = 0; n < 64 && k + (size_t)n < len; n++) {
                hash_out[n] = log[k + n];
            }
            hash_out[64] = '\0';
            return;
        }
    }
}

static void audit_clean_copy(const char *src, size_t max, char *dst) {
    size_t n = 0;
    while (src[n] != '\0' && n + 1 < max) {
        unsigned char c = (unsigned char)src[n];
        if (c < 0x20 || c > 0x7E) {
            dst[n] = '?';
        } else {
            dst[n] = src[n];
        }
        n++;
    }
    dst[n] = '\0';
}

int audit_append(char *log, size_t cap, const char *time19,
                 const char *event, const char *detail) {
    if (log == NULL || cap == 0 || time19 == NULL || event == NULL ||
        detail == NULL) {
        return -1;
    }
    if (sec_len(time19) != 19) {
        return -1;
    }
    char ev[25];
    char de[49];
    audit_clean_copy(event, sizeof(ev), ev);
    audit_clean_copy(detail, sizeof(de), de);
    size_t len = sec_len(log);
    int last_seq = 0;
    char prev_hex[65];
    audit_last(log, len, &last_seq, prev_hex);
    char line[256];
    int line_len = audit_build_line(last_seq + 1, time19, ev, de,
                                    prev_hex, line, sizeof(line));
    if (line_len < 0) {
        return -1;
    }
    /* 入らなければ古い行を先頭から捨てる。 */
    while (len + (size_t)line_len + 1 > cap) {
        size_t nl = 0;
        while (nl < len && log[nl] != '\n') {
            nl++;
        }
        if (nl >= len) {
            return -1; /* 1 行しか無くて入らない */
        }
        size_t drop = nl + 1;
        for (size_t i = 0; i + drop <= len; i++) {
            log[i] = log[i + drop];
        }
        len -= drop;
    }
    for (int i = 0; i < line_len; i++) {
        log[len + (size_t)i] = line[i];
    }
    log[len + (size_t)line_len] = '\0';
    return 0;
}

/* 1 行を分解する。seq/time/event/detail/prev/hash を取り出す。
 * 戻り値: 0 = 読めた、-1 = 形式が壊れている。 */
static int audit_parse_line(const char *line, size_t len, int *seq_out,
                            char *time_out, char *event_out,
                            char *detail_out, char *prev_out,
                            char *hash_out) {
    if (len < 10 || line[0] != 's' || line[1] != 'e' ||
        line[2] != 'q' || line[3] != '=') {
        return -1;
    }
    int v = 0;
    size_t i = 4;
    if (i >= len || line[i] < '0' || line[i] > '9') {
        return -1;
    }
    while (i < len && line[i] >= '0' && line[i] <= '9') {
        v = v * 10 + (line[i] - '0');
        i++;
    }
    *seq_out = v;
    /* " time=" (19 文字) */
    if (i + 6 > len || line[i] != ' ' || line[i + 1] != 't') {
        return -1;
    }
    i += 6;
    if (i + 19 > len) {
        return -1;
    }
    for (int k = 0; k < 19; k++) {
        time_out[k] = line[i + k];
    }
    time_out[19] = '\0';
    i += 19;
    /* " event=" */
    if (i + 7 > len || line[i] != ' ' || line[i + 1] != 'e') {
        return -1;
    }
    i += 7;
    size_t e = 0;
    while (i < len && line[i] != ' ' && e + 1 < 25) {
        event_out[e++] = line[i++];
    }
    event_out[e] = '\0';
    if (e == 0) {
        return -1;
    }
    /* " detail=" */
    if (i + 8 > len || line[i] != ' ' || line[i + 1] != 'd') {
        return -1;
    }
    i += 8;
    /* detail は " prev=" の直前まで。 */
    size_t d = 0;
    while (i < len) {
        if (line[i] == ' ' && i + 6 <= len && line[i + 1] == 'p' &&
            line[i + 2] == 'r' && line[i + 3] == 'e' &&
            line[i + 4] == 'v' && line[i + 5] == '=') {
            break;
        }
        if (d + 1 < 49) {
            detail_out[d++] = line[i];
        }
        i++;
    }
    detail_out[d] = '\0';
    /* " prev=" (64 桁) */
    if (i + 6 > len) {
        return -1;
    }
    i += 6;
    if (i + 64 > len) {
        return -1;
    }
    for (int k = 0; k < 64; k++) {
        if (hex_val(line[i + k]) < 0) {
            return -1;
        }
        prev_out[k] = line[i + k];
    }
    prev_out[64] = '\0';
    i += 64;
    /* " hash=" (64 桁) */
    if (i + 6 > len || line[i] != ' ' || line[i + 1] != 'h') {
        return -1;
    }
    i += 6;
    if (i + 64 > len) {
        return -1;
    }
    for (int k = 0; k < 64; k++) {
        if (hex_val(line[i + k]) < 0) {
            return -1;
        }
        hash_out[k] = line[i + k];
    }
    hash_out[64] = '\0';
    return 0;
}

int audit_verify(const char *log, size_t len, int *out_count,
                 int *out_bad_seq) {
    int count = 0;
    int bad = 0;
    int rc = 0;
    if (out_count != NULL) {
        *out_count = 0;
    }
    if (out_bad_seq != NULL) {
        *out_bad_seq = 0;
    }
    if (log == NULL) {
        return -2;
    }
    char prev_hash[65];
    for (int i = 0; i < 64; i++) {
        prev_hash[i] = '0';
    }
    prev_hash[64] = '\0';
    size_t pos = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && log[eol] != '\n') {
            eol++;
        }
        if (eol == pos) {
            break; /* 空行は終わり */
        }
        int seq = 0;
        char time19[20];
        char event[25];
        char detail[49];
        char prev[65];
        char hash[65];
        if (audit_parse_line(log + pos, eol - pos, &seq, time19, event,
                             detail, prev, hash) != 0) {
            rc = -2;
            bad = seq;
            break;
        }
        /* 連鎖: 2 件目以降は prev が前の hash と一致すること。 */
        if (count > 0 && strcmp(prev, prev_hash) != 0) {
            rc = -1;
            bad = seq;
            break;
        }
        /* hash の再計算。 */
        char rebuilt[256];
        int rl = audit_build_line(seq, time19, event, detail, prev,
                                  rebuilt, sizeof(rebuilt));
        if (rl < 0) {
            rc = -2;
            bad = seq;
            break;
        }
        /* rebuilt から hash だけ取り出して比べる。 */
        char *hp = NULL;
        for (int k = 0; rebuilt[k] != '\0'; k++) {
            if (rebuilt[k] == ' ' && rebuilt[k + 1] == 'h') {
                hp = rebuilt + k + 6;
                break;
            }
        }
        if (hp == NULL || strncmp(hp, hash, 64) != 0) {
            rc = -1;
            bad = seq;
            break;
        }
        for (int k = 0; k < 64; k++) {
            prev_hash[k] = hash[k];
        }
        prev_hash[64] = '\0';
        count++;
        pos = eol + 1;
    }
    if (out_count != NULL) {
        *out_count = count;
    }
    if (out_bad_seq != NULL) {
        *out_bad_seq = bad;
    }
    return rc;
}

int audit_count(const char *log, size_t len) {
    if (log == NULL) {
        return 0;
    }
    int n = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && log[eol] != '\n') {
            eol++;
        }
        if (eol == pos) {
            break;
        }
        n++;
        pos = eol + 1;
    }
    return n;
}

static audit_sink_fn audit_sink;

void audit_set_sink(audit_sink_fn sink) {
    audit_sink = sink;
}

void audit_log_event(const char *event, const char *detail) {
    if (audit_sink != NULL && event != NULL && detail != NULL) {
        audit_sink(event, detail);
    }
}

/* ---- アプリ許可リスト ---- */

static const char *SEC_APP_NAMES[SEC_APP_COUNT] = {
    "terminal", "files", "settings", "browser",
    "notes", "calendar", "photos", "music",
    "cards", "shooter", "sheet", "video",
    "installer", "mail", "docs", "slides",
    "contacts", "editor", "paint", "store",
    "notifications",
};

int sec_app_id(const char *name) {
    if (name == NULL) {
        return -1;
    }
    for (int i = 0; i < SEC_APP_COUNT; i++) {
        if (strcmp(name, SEC_APP_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *sec_app_name(int id) {
    if (id < 0 || id >= SEC_APP_COUNT) {
        return "";
    }
    return SEC_APP_NAMES[id];
}

int allowlist_parse(const char *text, size_t len, int *allowed) {
    if (allowed == NULL) {
        return 0;
    }
    for (int i = 0; i < SEC_APP_COUNT; i++) {
        allowed[i] = 1;
    }
    if (text == NULL || len == 0) {
        return 0;
    }
    /* "all" だけなら全部許可のまま。 */
    size_t i = 0;
    while (i < len && (text[i] == ' ' || text[i] == '\n' ||
                       text[i] == '\r' || text[i] == '\t')) {
        i++;
    }
    if (i + 3 <= len && text[i] == 'a' && text[i + 1] == 'l' &&
        text[i + 2] == 'l') {
        return 1;
    }
    for (int k = 0; k < SEC_APP_COUNT; k++) {
        allowed[k] = 0;
    }
    int found = 0;
    char name[32];
    size_t n = 0;
    for (size_t p = 0; p <= len; p++) {
        char c = p < len ? text[p] : ',';
        if (c == ',' || c == '\n' || c == '\r' || c == ' ' ||
            c == '\t') {
            if (n > 0) {
                name[n] = '\0';
                int id = sec_app_id(name);
                if (id >= 0) {
                    allowed[id] = 1;
                    found++;
                }
                n = 0;
            }
        } else if (n + 1 < sizeof(name)) {
            name[n++] = c;
        }
    }
    /* Terminal / Files / Settings は常に許可。 */
    allowed[0] = 1;
    allowed[1] = 1;
    allowed[2] = 1;
    return found;
}

size_t allowlist_serialize(const int *allowed, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    int all = 1;
    if (allowed != NULL) {
        for (int i = 0; i < SEC_APP_COUNT; i++) {
            if (!allowed[i]) {
                all = 0;
                break;
            }
        }
    }
    size_t pos = 0;
    if (all) {
        const char *s = "all\n";
        while (*s != '\0' && pos + 1 < cap) {
            buf[pos++] = *s++;
        }
        buf[pos] = '\0';
        return pos;
    }
    for (int i = 0; i < SEC_APP_COUNT; i++) {
        if (!allowed[i]) {
            continue;
        }
        const char *nm = SEC_APP_NAMES[i];
        if (pos > 0 && pos + 1 < cap) {
            buf[pos++] = ',';
        }
        while (*nm != '\0' && pos + 1 < cap) {
            buf[pos++] = *nm++;
        }
    }
    if (pos + 1 < cap) {
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    return pos;
}

int sec_app_allowed(const int *allowed, int app_id) {
    if (app_id < 0 || app_id >= SEC_APP_COUNT) {
        return 0;
    }
    /* Terminal / Files / Settings は常に許可。 */
    if (app_id == 0 || app_id == 1 || app_id == 2) {
        return 1;
    }
    if (allowed == NULL) {
        return 1;
    }
    return allowed[app_id] ? 1 : 0;
}

/* ---- PIN ポリシー ---- */

int sec_pin_policy_ok(const char *pin, int min_len, int require_digit) {
    if (pin == NULL) {
        return 0;
    }
    if (min_len < 4) {
        min_len = 4;
    }
    if (min_len > 16) {
        min_len = 16;
    }
    size_t n = sec_len(pin);
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

/* ---- リモート操作 ---- */

static const char *find_key(const char *text, size_t len,
                            const char *key) {
    size_t klen = sec_len(key);
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

static void copy_value(const char *src, size_t max_len, char *dst,
                       size_t cap) {
    size_t n = 0;
    while (src[n] != '\0' && src[n] != '\n' && src[n] != '\r' &&
           n + 1 < cap && n < max_len) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

int remote_parse(const char *text, size_t len, char *action_out,
                 size_t action_cap, char *token_out, size_t token_cap) {
    if (text == NULL || action_out == NULL || token_out == NULL) {
        return -1;
    }
    action_out[0] = '\0';
    token_out[0] = '\0';
    const char *a = find_key(text, len, "action=");
    if (a == NULL) {
        return -1;
    }
    copy_value(a, 16, action_out, action_cap);
    const char *t = find_key(text, len, "token=");
    if (t != NULL) {
        copy_value(t, 64, token_out, token_cap);
    }
    return 0;
}

void remote_config_defaults(sec_remote_config *cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->host[0] = '\0';
    cfg->path[0] = '\0';
    cfg->token[0] = '\0';
    cfg->enabled = 0;
    const char *def_path = "/prism-remote.txt";
    size_t i = 0;
    while (def_path[i] != '\0') {
        cfg->path[i] = def_path[i];
        i++;
    }
    cfg->path[i] = '\0';
}

static size_t append_str(char *buf, size_t cap, size_t pos,
                         const char *s) {
    while (*s != '\0' && pos + 1 < cap) {
        buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}

size_t remote_config_serialize(const sec_remote_config *cfg, char *buf,
                               size_t cap) {
    size_t pos = 0;
    if (cfg == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    pos = append_str(buf, cap, pos, "host=");
    pos = append_str(buf, cap, pos, cfg->host);
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "path=");
    pos = append_str(buf, cap, pos, cfg->path);
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "token=");
    pos = append_str(buf, cap, pos, cfg->token);
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "enabled=");
    pos = append_str(buf, cap, pos, cfg->enabled ? "on" : "off");
    pos = append_str(buf, cap, pos, "\n");
    return pos;
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (*s != *prefix) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

int remote_config_parse(sec_remote_config *cfg, const char *text,
                        size_t len) {
    int found = 0;
    size_t i = 0;
    if (cfg == NULL || text == NULL) {
        return 0;
    }
    while (i < len) {
        char line[192];
        size_t n = 0;
        while (i < len && text[i] != '\n' && n + 1 < sizeof(line)) {
            line[n++] = text[i++];
        }
        while (i < len && text[i] != '\n') {
            i++;
        }
        if (i < len) {
            i++;
        }
        line[n] = '\0';
        if (starts_with(line, "host=")) {
            copy_value(line + 5, 63, cfg->host, sizeof(cfg->host));
            found = 1;
        } else if (starts_with(line, "path=")) {
            copy_value(line + 5, 127, cfg->path, sizeof(cfg->path));
            found = 1;
        } else if (starts_with(line, "token=")) {
            copy_value(line + 6, 64, cfg->token, sizeof(cfg->token));
            found = 1;
        } else if (starts_with(line, "enabled=")) {
            if (strcmp(line + 8, "on") == 0) {
                cfg->enabled = 1;
                found = 1;
            } else if (strcmp(line + 8, "off") == 0) {
                cfg->enabled = 0;
                found = 1;
            }
        }
    }
    return found;
}
