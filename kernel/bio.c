/* PrismOS — 生体認証の実装 (bio.h を見ること)。 */
#include "bio.h"

#include "../shell/vfs.h"
#include "persist.h"
#include "security.h"
#include "string.h"

#define BIO_DB_FILE "bio.db"
#define BIO_MAGIC "BIO1"

/* 照合の許容: バイト差 24 以下を「近い」とし、32 バイト中
 * 25 バイト以上が近ければ一致とする。 */
#define BIO_BYTE_TOL 24
#define BIO_MATCH_MIN 25

typedef struct {
    int present;
    uint8_t feat[BIO_FEAT_LEN];
} bio_template;

static bio_template g_templates[3]; /* 1 = 指紋、2 = 顔 */
static int g_loaded;
static int g_fails;
static int g_locked;
static int g_mock_id;
static uint32_t g_attempt;

/* ---- 模擬センサー ----
 * 試料 id から決定的に特徴を作る。取り込みのたびに
 * 小さなばらつき (各バイト ±6) を加えるので、登録時と
 * 照合時で完全には一致しない (本物のセンサーと同じ)。
 * 違う id では模様が大きく変わり、一致しない。 */
static uint32_t bio_rand(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int bio_sensor_capture(uint8_t feat[BIO_FEAT_LEN]) {
    if (g_mock_id <= 0) {
        return -1;
    }
    g_attempt++;
    /* 模様は id だけから決まる乱数列 (xorshift) の上位
     * バイト。id が違えば無相関になり、近い値の偶然の
     * 一致は起きない。ばらつきは試行ごとの別列から。 */
    uint32_t pat = (uint32_t)g_mock_id * 2654435761u + 0x9E3779B9u;
    if (pat == 0) {
        pat = 1;
    }
    uint32_t st = pat ^ (g_attempt * 40503u + 1u);
    if (st == 0) {
        st = 1;
    }
    for (int i = 0; i < BIO_FEAT_LEN; i++) {
        uint32_t base = bio_rand(&pat) >> 24;
        uint32_t r = bio_rand(&st);
        int jitter = (int)(r % 13u) - 6;
        feat[i] = (uint8_t)(base + (uint32_t)jitter);
    }
    return 0;
}

void bio_mock_present(int id) {
    g_mock_id = id;
}

int bio_mock_current(void) {
    return g_mock_id;
}

/* ---- テンプレートの保存 ---- */

static void bio_load(void) {
    if (g_loaded) {
        return;
    }
    g_loaded = 1;
    size_t len = 0;
    const char *data = vfs_read_ptr(BIO_DB_FILE, &len);
    if (data == NULL || len < 4 + 1 + BIO_FEAT_LEN + 1 + BIO_FEAT_LEN) {
        return;
    }
    if (memcmp(data, BIO_MAGIC, 4) != 0) {
        return;
    }
    const uint8_t *p = (const uint8_t *)data + 4;
    g_templates[BIO_TYPE_FINGER].present = p[0] != 0;
    memcpy(g_templates[BIO_TYPE_FINGER].feat, p + 1, BIO_FEAT_LEN);
    p += 1 + BIO_FEAT_LEN;
    g_templates[BIO_TYPE_FACE].present = p[0] != 0;
    memcpy(g_templates[BIO_TYPE_FACE].feat, p + 1, BIO_FEAT_LEN);
}

static int bio_save(void) {
    uint8_t buf[4 + 1 + BIO_FEAT_LEN + 1 + BIO_FEAT_LEN];
    memcpy(buf, BIO_MAGIC, 4);
    uint8_t *p = buf + 4;
    *p++ = (uint8_t)g_templates[BIO_TYPE_FINGER].present;
    memcpy(p, g_templates[BIO_TYPE_FINGER].feat, BIO_FEAT_LEN);
    p += BIO_FEAT_LEN;
    *p++ = (uint8_t)g_templates[BIO_TYPE_FACE].present;
    memcpy(p, g_templates[BIO_TYPE_FACE].feat, BIO_FEAT_LEN);
    if (vfs_write(BIO_DB_FILE, (const char *)buf, sizeof(buf)) != 0) {
        return -1;
    }
    persist_maybe_sync();
    return 0;
}

/* ---- 登録・照合 ---- */

int bio_enroll(int type) {
    if (type != BIO_TYPE_FINGER && type != BIO_TYPE_FACE) {
        return -1;
    }
    bio_load();
    uint8_t feat[BIO_FEAT_LEN];
    if (bio_sensor_capture(feat) != 0) {
        return -1;
    }
    memcpy(g_templates[type].feat, feat, BIO_FEAT_LEN);
    g_templates[type].present = 1;
    if (bio_save() != 0) {
        g_templates[type].present = 0;
        return -2;
    }
    audit_log_event("bio-enroll",
                    type == BIO_TYPE_FINGER ? "finger" : "face");
    return 0;
}

static int bio_distance(const uint8_t *a, const uint8_t *b) {
    int close = 0;
    for (int i = 0; i < BIO_FEAT_LEN; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) {
            d = -d;
        }
        if (d <= BIO_BYTE_TOL) {
            close++;
        }
    }
    return close;
}

int bio_verify(int type) {
    if (type != BIO_TYPE_FINGER && type != BIO_TYPE_FACE) {
        return -3;
    }
    bio_load();
    if (g_locked) {
        return -2;
    }
    if (!g_templates[type].present) {
        return -3;
    }
    uint8_t feat[BIO_FEAT_LEN];
    if (bio_sensor_capture(feat) != 0) {
        return -4;
    }
    int close = bio_distance(g_templates[type].feat, feat);
    if (close >= BIO_MATCH_MIN) {
        g_fails = 0;
        audit_log_event("bio-ok",
                        type == BIO_TYPE_FINGER ? "finger" : "face");
        return 0;
    }
    g_fails++;
    audit_log_event("bio-fail",
                    type == BIO_TYPE_FINGER ? "finger" : "face");
    if (g_fails >= BIO_LOCK_THRESHOLD) {
        g_locked = 1;
        audit_log_event("bio-lock", "too many failures");
    }
    return -1;
}

int bio_enrolled(int type) {
    if (type != BIO_TYPE_FINGER && type != BIO_TYPE_FACE) {
        return 0;
    }
    bio_load();
    return g_templates[type].present;
}

int bio_fail_count(void) {
    return g_fails;
}

int bio_locked(void) {
    return g_locked;
}

void bio_unlock_reset(void) {
    g_fails = 0;
    g_locked = 0;
}

int bio_clear(void) {
    bio_load();
    g_templates[BIO_TYPE_FINGER].present = 0;
    g_templates[BIO_TYPE_FACE].present = 0;
    memset(g_templates[BIO_TYPE_FINGER].feat, 0, BIO_FEAT_LEN);
    memset(g_templates[BIO_TYPE_FACE].feat, 0, BIO_FEAT_LEN);
    if (bio_save() != 0) {
        return -1;
    }
    bio_unlock_reset();
    audit_log_event("bio-clear", "templates removed");
    return 0;
}
