/* PrismOS — ディスクへの永続化とインストーラ (ATA + diskfs)。 */
#include "persist.h"
#include "account.h"
#include "block.h"
#include "crypto.h"
#include "diskfs.h"
#include "fde.h"
#include "../shell/install.h"
#include "heap.h"
#include "rtc.h"
#include "security.h"
#include "serial.h"
#include "settings.h"
#include "string.h"
#include "../shell/vfs.h"

static int disk_found;
static int disk_installed;
static unsigned long last_sync_seq;
/* 回復モードの要求フラグ。persist_init() でディスクから読み、
 * 読んだら消す (1 回だけ有効)。 */
static int recovery_requested;
/* データ用パーティションの開始 LBA (0 = ディスク全体の従来配置)
 * と、その大きさ (セクタ数)。persist_init() で MBR を読んで決める。 */
static uint32_t disk_base_lba;
static uint32_t disk_part_sectors;

/* 暗号化の鍵 (ログインで確かめた PIN から作る)。起動中の
 * メモリにだけ置き、ディスクには書かない。 */
static uint8_t disk_key[32];
static int sync_blocked;
static int key_active;
/* 外部メディア制限 (フェーズ14)。1 = ディスクの利用者
 * ファイルを読まない・書かない。settings.txt と .account
 * だけは読む・書く (設定とログインに要るため)。
 * persist_load() がディスクの settings.txt を読んで決める。 */
static int media_blocked;
/* IV を作るカウンタ。時計から種を取って起動ごとにずらし、
 * 書くたびに 1 ずつ増やす。同じ鍵で同じ IV を二度使わない
 * ためのもので、ハードウェア乱数ではない。 */
static uint64_t iv_counter;

/* 起動時に暗号文のまま読み込んだファイルの名前と IV。
 * ログイン後に復号するときに使う。 */
static char enc_names[DISKFS_MAX_FILES][DISKFS_MAX_NAME + 1];
static uint8_t enc_ivs[DISKFS_MAX_FILES][16];
static int enc_count;

/* ---- 全ディスク暗号化 (FDE、docs/FDE_DESIGN.md) ----
 * ヘッダセクタはデータ領域の先頭セクタ (相対 0、絶対では
 * disk_base_lba)。diskfs は相対 0 をデータに使わない
 * (全体配置では MBR だった場所で、有効化時にヘッダへ
 * 置き換わる)。セクタレイアウト:
 *   [0..92)    スロット0 (利用者のパスフレーズ)
 *   [128..220) スロット1 (管理者のリカバリキー、任意)
 *   [256..276) 移行ジャーナル "PRISMFP1" + 向き + 次セクタ
 * 暗号シムは diskfs とブロック層の間 (下の wrap 関数) に
 * 入る。起動区画 (FAT16) や MBR は領域の外なので触らない。 */
static int fde_present;  /* ヘッダが見つかった */
static int fde_unlocked; /* マスター鍵がメモリにある */
static uint8_t fde_master[FDE_MASTER_KEY_LEN];
static fde_header fde_slot0;
static fde_header fde_slot1;
static int fde_has_slot1;
static int fde_mig_dir;        /* 0=なし, 1=暗号化中, 2=復号中 */
static uint32_t fde_mig_next;  /* 次に処理する相対セクタ */
static uint32_t fde_mig_total; /* 領域の総セクタ数 */

#define FDE_SLOT1_OFF 128
#define FDE_JOURNAL_OFF 256
/* fde_header の直列化で実際に使う長さ (8+16+4+16+32+16)。 */
#define FDE_HDR_USED 92

static void fde_put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t fde_get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* シムが効く範囲か。ヘッダセクタ自身と領域の外は素通し。
 * 移行の途中は、混在状態を diskfs に見せないため全体を
 * 素通しにする (移行は生セクタで直接行う)。 */
static int fde_shim_covers(uint32_t lba) {
    if (!fde_present || !fde_unlocked || fde_mig_dir != 0) {
        return 0;
    }
    if (lba == disk_base_lba) {
        return 0;
    }
    if (lba < disk_base_lba ||
        lba >= disk_base_lba + disk_part_sectors) {
        return 0;
    }
    return 1;
}

static int block_read_wrap(void *ctx, uint32_t lba, uint8_t *buf) {
    (void)ctx;
    int rc = block_read_sector(lba, buf);
    if (rc == 0 && fde_shim_covers(lba)) {
        uint8_t tmp[BLOCK_SECTOR_SIZE];
        memcpy(tmp, buf, BLOCK_SECTOR_SIZE);
        fde_sector_crypt(fde_master, lba, tmp, buf);
    }
    return rc;
}

static int block_write_wrap(void *ctx, uint32_t lba, const uint8_t *buf) {
    (void)ctx;
    if (fde_shim_covers(lba)) {
        uint8_t tmp[BLOCK_SECTOR_SIZE];
        fde_sector_crypt(fde_master, lba, buf, tmp);
        return block_write_sector(lba, tmp);
    }
    return block_write_sector(lba, buf);
}

/* IV を作る: SHA-256(鍵 || カウンタ) の先頭 16 バイト。
 * カウンタは書くたびに増える。 */
static void make_iv(uint8_t *iv_out) {
    uint8_t buf[32 + 8];
    for (int i = 0; i < 32; i++) {
        buf[i] = disk_key[i];
    }
    for (int i = 0; i < 8; i++) {
        buf[32 + i] = (uint8_t)((iv_counter >> ((7 - i) * 8)) & 0xFF);
    }
    iv_counter++;
    uint8_t hash[32];
    crypto_sha256(buf, sizeof(buf), hash);
    for (int i = 0; i < 16; i++) {
        iv_out[i] = hash[i];
    }
}

static int persist_encrypt_hook(void *ctx, uint8_t *iv_out,
                                const uint8_t *in, uint8_t *out,
                                size_t len) {
    (void)ctx;
    if (!key_active) {
        return -1;
    }
    make_iv(iv_out);
    crypto_aes256_ctr(disk_key, iv_out, in, out, len);
    return 0;
}

static int persist_decrypt_hook(void *ctx, const uint8_t *iv,
                                const uint8_t *in, uint8_t *out,
                                size_t len) {
    (void)ctx;
    if (!key_active) {
        return -1;
    }
    crypto_aes256_ctr(disk_key, iv, in, out, len);
    return 0;
}

static const diskfs_crypto persist_crypto = {
    NULL, persist_encrypt_hook, persist_decrypt_hook,
};

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

/* HMAC-SHA-256 先頭 8 バイト (秘密の包み用 MAC)。
 * ipad/opad は tls.c と同じ組み立て。 */
static void persist_secret_mac(const uint8_t *iv, const uint8_t *ct,
                               size_t len, uint8_t mac[8]) {
    uint8_t pad[64];
    uint8_t inner[32];
    for (int i = 0; i < 64; i++) {
        pad[i] = (uint8_t)(disk_key[i < 32 ? i : 0] ^ 0x36);
        if (i >= 32) {
            pad[i] = (uint8_t)(0 ^ 0x36);
        }
    }
    {
        crypto_sha256_ctx cx;
        crypto_sha256_init(&cx);
        crypto_sha256_update(&cx, pad, 64);
        crypto_sha256_update(&cx, iv, 16);
        crypto_sha256_update(&cx, ct, len);
        crypto_sha256_final(&cx, inner);
    }
    for (int i = 0; i < 64; i++) {
        pad[i] = (uint8_t)(disk_key[i < 32 ? i : 0] ^ 0x5C);
        if (i >= 32) {
            pad[i] = (uint8_t)(0 ^ 0x5C);
        }
    }
    {
        crypto_sha256_ctx cx;
        crypto_sha256_init(&cx);
        crypto_sha256_update(&cx, pad, 64);
        crypto_sha256_update(&cx, inner, 32);
        uint8_t full[32];
        crypto_sha256_final(&cx, full);
        for (int i = 0; i < 8; i++) {
            mac[i] = full[i];
        }
    }
}

int persist_wrap_secret(const char *plain, char *out, size_t cap) {
    if (!key_active || plain == NULL || out == NULL) {
        return -1;
    }
    size_t len = 0;
    while (plain[len] != '\0') {
        len++;
    }
    if (len > 48) {
        return -1;
    }
    /* Output: 32 hex (IV) + ':' + 2*len hex + ':' +
     * 16 hex (MAC) + NUL. */
    if (cap < 34 + 2 * len + 17) {
        return -1;
    }
    uint8_t iv[16];
    entropy_reseed();
    entropy_bytes(iv, sizeof(iv));
    uint8_t ct[48];
    crypto_aes256_ctr(disk_key, iv, (const uint8_t *)plain, ct, len);
    static const char hexd[] = "0123456789abcdef";
    size_t p = 0;
    for (int i = 0; i < 16; i++) {
        out[p++] = hexd[iv[i] >> 4];
        out[p++] = hexd[iv[i] & 15];
    }
    out[p++] = ':';
    for (size_t i = 0; i < len; i++) {
        out[p++] = hexd[ct[i] >> 4];
        out[p++] = hexd[ct[i] & 15];
    }
    uint8_t mac[8];
    persist_secret_mac(iv, ct, len, mac);
    out[p++] = ':';
    for (int i = 0; i < 8; i++) {
        out[p++] = hexd[mac[i] >> 4];
        out[p++] = hexd[mac[i] & 15];
    }
    out[p] = '\0';
    return 0;
}

int persist_unwrap_secret(const char *enc, char *out, size_t cap) {
    if (!key_active || enc == NULL || out == NULL || cap == 0) {
        return -1;
    }
    uint8_t iv[16];
    for (int i = 0; i < 16; i++) {
        int hi = hex_val(enc[2 * i]);
        int lo = hex_val(enc[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        iv[i] = (uint8_t)((hi << 4) | lo);
    }
    if (enc[32] != ':') {
        return -1;
    }
    const char *hex = enc + 33;
    size_t hlen = 0;
    while (hex[hlen] != '\0' && hex[hlen] != ':') {
        hlen++;
    }
    if (hlen == 0 || (hlen & 1) != 0 || hlen / 2 >= cap || hlen / 2 > 48) {
        return -1;
    }
    /* MAC 部 (':' の後ろ 16 桁) があれば検証する。無い
     * 形式は移行期の旧データとして受け入れる。 */
    const char *mac_hex = NULL;
    if (hex[hlen] == ':') {
        mac_hex = hex + hlen + 1;
        size_t mlen = 0;
        while (mac_hex[mlen] != '\0') {
            mlen++;
        }
        if (mlen != 16) {
            return -1;
        }
    }
    uint8_t ct[48];
    for (size_t i = 0; i < hlen / 2; i++) {
        int hi = hex_val(hex[2 * i]);
        int lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        ct[i] = (uint8_t)((hi << 4) | lo);
    }
    if (mac_hex != NULL) {
        uint8_t want[8];
        persist_secret_mac(iv, ct, hlen / 2, want);
        uint8_t diff = 0;
        for (int i = 0; i < 8; i++) {
            int hi = hex_val(mac_hex[2 * i]);
            int lo = hex_val(mac_hex[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            diff |= (uint8_t)(want[i] ^ (uint8_t)((hi << 4) | lo));
        }
        if (diff != 0) {
            return -1; /* 改ざんか鍵違い */
        }
    }
    crypto_aes256_ctr(disk_key, iv, ct, (uint8_t *)out, hlen / 2);
    out[hlen / 2] = '\0';
    return 0;
}

static diskfs_dev ata_dev(void) {
    diskfs_dev dev;
    dev.ctx = NULL;
    dev.read_sector = block_read_wrap;
    dev.write_sector = block_write_wrap;
    dev.total_sectors = disk_base_lba == 0 ? (uint32_t)block_total_sectors()
                                           : disk_part_sectors;
    dev.base_lba = disk_base_lba;
    dev.crypto = key_active ? &persist_crypto : NULL;
    return dev;
}

/* 暗号化しないデバイス。".account" はログイン前に読むので
 * 平文のままでなければならない。 */
static diskfs_dev ata_dev_plain(void) {
    diskfs_dev dev = ata_dev();
    dev.crypto = NULL;
    return dev;
}

static void print_u64_serial(uint64_t value) {
    char buf[21];
    int i = 0;
    if (value == 0) {
        serial_write_char('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        serial_write_char(buf[--i]);
    }
}

/* diskfs の時刻源: RTC (ローカル時刻) を Unix 秒として
 * 返す。タイムスタンプはローカル時刻ベースの値になる。 */
static uint32_t persist_diskfs_time(void) {
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        return 0;
    }
    return rtc_to_unix(&t);
}

/* diskfs として読めるか調べ、回復モードの印を読む。
 * persist_init (平文ディスク) と、FDE のロック解除後
 * (persist_fde_try_unlock 等) の両方から呼ぶ。 */
static void persist_probe_diskfs(void) {
    diskfs_dev dev = ata_dev();
    int fmt = diskfs_is_formatted(&dev);
    if (fmt == 1) {
        disk_installed = 1;
        serial_write_line("[boot] disk: installed system found");
        /* 回復モードの印を読む。読んだら消す (1 回だけ有効)。 */
        int flag = diskfs_get_recovery_flag(&dev);
        if (flag == 1) {
            recovery_requested = 1;
            diskfs_set_recovery_flag(&dev, 0);
            serial_write_line("[boot] disk: recovery requested");
        }
    } else if (fmt == 0) {
        serial_write_line("[boot] disk: (not installed - run `install yes`)");
    } else {
        serial_write_line("[boot] disk: (could not read disk)");
    }
}

/* ヘッダセクタ全体 (スロット + ジャーナル) を生で書く。 */
static int fde_write_header_sector(int dir, uint32_t next,
                                   uint32_t total) {
    uint8_t sector[BLOCK_SECTOR_SIZE];
    /* 読み直して書き戻す: ヘッダの占有域 ([0..276)) だけを
     * 重ね、残りは元のセクタのまま残す。全体配置では
     * セクタ 0 に MBR のパーティション表 (446〜) と署名
     * (510-511) があり、diskfs_is_formatted がそれを検査
     * するため、ヘッダで丸ごと置き換えると復号後も
     * 「未フォーマット」と誤判定されてしまう。 */
    if (block_read_sector(disk_base_lba, sector) != 0) {
        memset(sector, 0, sizeof(sector));
    }
    uint8_t hdr[BLOCK_SECTOR_SIZE];
    if (fde_header_serialize(&fde_slot0, hdr) != 0) {
        return -1;
    }
    memcpy(sector, hdr, FDE_HDR_USED);
    if (fde_has_slot1) {
        if (fde_header_serialize(&fde_slot1, hdr) != 0) {
            return -1;
        }
        memcpy(sector + FDE_SLOT1_OFF, hdr, FDE_HDR_USED);
    } else {
        memset(sector + FDE_SLOT1_OFF, 0, FDE_HDR_USED);
    }
    if (dir != 0) {
        memcpy(sector + FDE_JOURNAL_OFF, "PRISMFP1", 8);
        sector[FDE_JOURNAL_OFF + 8] = (uint8_t)dir;
        fde_put32le(sector + FDE_JOURNAL_OFF + 12, next);
        fde_put32le(sector + FDE_JOURNAL_OFF + 16, total);
    } else {
        memset(sector + FDE_JOURNAL_OFF, 0, 20);
    }
    return block_write_sector(disk_base_lba, sector);
}

/* ヘッダセクタからジャーナルを読む。 */
static void fde_journal_read(const uint8_t *sector) {
    fde_mig_dir = 0;
    fde_mig_next = 0;
    fde_mig_total = 0;
    if (memcmp(sector + FDE_JOURNAL_OFF, "PRISMFP1", 8) != 0) {
        return;
    }
    int dir = sector[FDE_JOURNAL_OFF + 8];
    if (dir != 1 && dir != 2) {
        return;
    }
    fde_mig_dir = dir;
    fde_mig_next = fde_get32le(sector + FDE_JOURNAL_OFF + 12);
    fde_mig_total = fde_get32le(sector + FDE_JOURNAL_OFF + 16);
}

static void fde_forget_state(void) {
    fde_present = 0;
    fde_unlocked = 0;
    fde_has_slot1 = 0;
    fde_mig_dir = 0;
    fde_mig_next = 0;
    fde_mig_total = 0;
    memset(fde_master, 0, sizeof(fde_master));
}

int persist_fde_present(void) {
    return fde_present;
}

int persist_fde_unlocked(void) {
    return fde_present && fde_unlocked;
}

int persist_fde_migration_pending(void) {
    return fde_present ? fde_mig_dir : 0;
}

int persist_fde_verify(const char *passphrase) {
    if (!fde_present || passphrase == NULL) {
        return 0;
    }
    uint8_t mk[FDE_MASTER_KEY_LEN];
    int ok = fde_unwrap_key(passphrase, &fde_slot0, mk) == 0 ||
             (fde_has_slot1 &&
              fde_unwrap_key(passphrase, &fde_slot1, mk) == 0);
    memset(mk, 0, sizeof(mk));
    return ok ? 1 : 0;
}

int persist_fde_try_unlock(const char *passphrase) {
    if (!fde_present) {
        return 0;
    }
    if (fde_unlocked) {
        return fde_mig_dir != 0 ? 2 : 1;
    }
    if (passphrase == NULL) {
        return -1;
    }
    uint8_t mk[FDE_MASTER_KEY_LEN];
    if (fde_unwrap_key(passphrase, &fde_slot0, mk) != 0 &&
        (!fde_has_slot1 ||
         fde_unwrap_key(passphrase, &fde_slot1, mk) != 0)) {
        memset(mk, 0, sizeof(mk));
        return -1;
    }
    memcpy(fde_master, mk, FDE_MASTER_KEY_LEN);
    memset(mk, 0, sizeof(mk));
    fde_unlocked = 1;
    serial_write_line("[boot] disk: full-disk encryption unlocked");
    if (fde_mig_dir != 0) {
        return 2; /* 先に persist_fde_resume_migration() を呼ぶ */
    }
    persist_probe_diskfs();
    return 1;
}

int persist_fde_resume_migration(void) {
    if (!fde_present || !fde_unlocked || fde_mig_dir == 0) {
        return -1;
    }
    int dir = fde_mig_dir;
    uint32_t total = fde_mig_total != 0 ? fde_mig_total
                                        : disk_part_sectors;
    uint8_t buf[BLOCK_SECTOR_SIZE];
    for (uint32_t rel = fde_mig_next; rel < total; rel++) {
        uint32_t abs_lba = disk_base_lba + rel;
        if (block_read_sector(abs_lba, buf) != 0) {
            return -1;
        }
        fde_sector_crypt(fde_master, abs_lba, buf, buf);
        if (block_write_sector(abs_lba, buf) != 0) {
            return -1;
        }
        /* ジャーナルは毎セクタ記録する。間隔を空けると、
         * 中断時に「処理済みだが未記録」のセクタが再開で
         * 二重暗号化されて失われる。毎セクタなら、エラー
         * 復帰では完全に一貫し、電源断でも窓は処理中の
         * 1 セクタだけになる。 */
        fde_mig_next = rel + 1;
        if (fde_write_header_sector(dir, rel + 1, total) != 0) {
            return -1;
        }
        if ((rel & 4095u) == 0u) {
            serial_write(dir == 1 ? "[fde] encrypting... "
                                  : "[fde] decrypting... ");
            print_u64_serial((uint64_t)rel * 100u / total);
            serial_write_line("%");
        }
    }
    if (dir == 2) {
        /* 復号が終わった: ヘッダ占有域 ([0..276)) だけを
         * 消して平文ディスクに戻す。MBR のパーティション表
         * と署名 (446 以降) は有効化の間も保存してあるので、
         * これでフォーマット時の MBR と一致する。 */
        if (block_read_sector(disk_base_lba, buf) != 0) {
            return -1;
        }
        memset(buf, 0, FDE_JOURNAL_OFF + 20);
        if (block_write_sector(disk_base_lba, buf) != 0) {
            return -1;
        }
        fde_forget_state();
        serial_write_line("[fde] full-disk encryption removed");
    } else {
        if (fde_write_header_sector(0, 0, 0) != 0) {
            return -1;
        }
        fde_mig_dir = 0;
        fde_mig_next = 0;
        fde_mig_total = 0;
        serial_write_line("[fde] full-disk encryption active");
    }
    persist_probe_diskfs();
    return 0;
}

int persist_fde_enable(const char *passphrase, char *recovery_out,
                       size_t recovery_cap) {
    if (!disk_found || !disk_installed || fde_present) {
        return -1;
    }
    if (passphrase == NULL || recovery_out == NULL ||
        recovery_cap < 25) {
        return -1;
    }
    entropy_reseed();
    uint8_t master[FDE_MASTER_KEY_LEN];
    uint8_t salt[FDE_SALT_LEN], iv[16], salt1[FDE_SALT_LEN], iv1[16];
    entropy_bytes(master, sizeof(master));
    entropy_bytes(salt, sizeof(salt));
    entropy_bytes(iv, sizeof(iv));
    entropy_bytes(salt1, sizeof(salt1));
    entropy_bytes(iv1, sizeof(iv1));
    /* リカバリキー: 紛らわしい文字を除いた 24 文字。
     * そのままパスフレーズとしてスロット1 に包む。 */
    static const char alpha[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    uint8_t rb[24];
    entropy_bytes(rb, sizeof(rb));
    for (int i = 0; i < 24; i++) {
        recovery_out[i] = alpha[rb[i] % 31];
    }
    recovery_out[24] = '\0';
    if (fde_wrap_key(passphrase, salt, FDE_DEFAULT_ITERATIONS, iv,
                     master, &fde_slot0) != 0 ||
        fde_wrap_key(recovery_out, salt1, FDE_DEFAULT_ITERATIONS,
                     iv1, master, &fde_slot1) != 0) {
        memset(master, 0, sizeof(master));
        return -2;
    }
    fde_has_slot1 = 1;
    fde_present = 1;
    memcpy(fde_master, master, FDE_MASTER_KEY_LEN);
    memset(master, 0, sizeof(master));
    fde_unlocked = 1;
    /* ジャーナル付きヘッダを先に書いてからセクタを移行する。
     * 途中で止まっても、次回起動時に続きから再開できる。 */
    fde_mig_dir = 1;
    fde_mig_next = 1;
    fde_mig_total = disk_part_sectors;
    if (fde_write_header_sector(1, 1, disk_part_sectors) != 0) {
        fde_forget_state();
        return -2;
    }
    serial_write_line("[fde] encrypting disk...");
    if (persist_fde_resume_migration() != 0) {
        return -2; /* ジャーナルが残るので次回起動で再開する */
    }
    return 0;
}

int persist_fde_disable(void) {
    if (!fde_present || !fde_unlocked || fde_mig_dir != 0) {
        return -1;
    }
    fde_mig_dir = 2;
    fde_mig_next = 1;
    fde_mig_total = disk_part_sectors;
    if (fde_write_header_sector(2, 1, disk_part_sectors) != 0) {
        fde_mig_dir = 0;
        return -2;
    }
    serial_write_line("[fde] decrypting disk...");
    return persist_fde_resume_migration();
}

void persist_init(void) {
    diskfs_set_time_source(persist_diskfs_time);
    vfs_set_time_source(persist_diskfs_time);
    disk_found = block_init();
    disk_installed = 0;
    last_sync_seq = vfs_change_seq();
    disk_base_lba = 0;
    disk_part_sectors = (uint32_t)block_total_sectors();
    if (!disk_found) {
        serial_write_line("[boot] disk: (no disk found)");
        return;
    }
    serial_write("[boot] disk: ");
    serial_write(block_kind());
    serial_write(" ");
    serial_write(block_model());
    serial_write(" ");
    print_u64_serial(block_total_sectors());
    serial_write_line(" sectors");
    /* MBR を読んでデータ用パーティションを探す。ブート用ディスク
     * (起動パーティション + データ用パーティション) では、データは
     * パーティションの中に置く。見つからなければ従来の配置。 */
    static uint8_t mbr[BLOCK_SECTOR_SIZE];
    if (block_read_sector(0, mbr) == 0) {
        uint32_t part_size = 0;
        uint32_t base = diskfs_find_data_partition(mbr, &part_size);
        if (base != 0) {
            disk_base_lba = base;
            disk_part_sectors = part_size;
            serial_write("[boot] disk: data partition at LBA ");
            print_u64_serial((uint64_t)base);
            serial_write(" (");
            print_u64_serial((uint64_t)part_size);
            serial_write_line(" sectors)");
        }
    }
    /* FDE ヘッダの確認 (データ領域の先頭セクタ)。あれば
     * ロック解除されるまで diskfs には触らない (スーパー
     * ブロックも暗号化されているため)。 */
    static uint8_t hdrsec[BLOCK_SECTOR_SIZE];
    if (block_read_sector(disk_base_lba, hdrsec) == 0 &&
        fde_header_parse(hdrsec, &fde_slot0) == 0) {
        fde_present = 1;
        uint8_t tmp[BLOCK_SECTOR_SIZE];
        memset(tmp, 0, sizeof(tmp));
        memcpy(tmp, hdrsec + FDE_SLOT1_OFF, FDE_HDR_USED);
        if (fde_header_parse(tmp, &fde_slot1) == 0) {
            fde_has_slot1 = 1;
        }
        fde_journal_read(hdrsec);
        serial_write_line(
            "[boot] disk: full-disk encryption (locked)");
        if (fde_mig_dir != 0) {
            serial_write_line(
                "[boot] disk: encryption setup was interrupted; "
                "it resumes after unlock");
        }
        return;
    }
    persist_probe_diskfs();
}

int persist_initialize_disk(void) {
    if (!disk_found) {
        return -1;
    }
    if (disk_base_lba != 0) {
        if (persist_install_dual() != 0) {
            return -2;
        }
    } else if (persist_install() != 0) {
        return -2;
    }
    return 0;
}

int persist_request_recovery(void) {
    if (!disk_found || !disk_installed) {
        return -1;
    }
    diskfs_dev dev = ata_dev();
    if (diskfs_set_recovery_flag(&dev, 1) != 0) {
        return -1;
    }
    return 0;
}

int persist_recovery_requested(void) {
    return recovery_requested;
}

void persist_timestamp_now(char *buf) {
    prism_rtc_time t;
    static const char D[] = "0123456789";
    if (buf == NULL) {
        return;
    }
    if (rtc_read(&t) != 0) {
        const char *zero = "0000-00-00 00:00:00";
        for (int i = 0; zero[i] != '\0'; i++) {
            buf[i] = zero[i];
        }
        buf[19] = '\0';
        return;
    }
    int year = 2000 + (int)t.year;
    buf[0] = D[(year / 1000) % 10];
    buf[1] = D[(year / 100) % 10];
    buf[2] = D[(year / 10) % 10];
    buf[3] = D[year % 10];
    buf[4] = '-';
    buf[5] = D[(t.month / 10) % 10];
    buf[6] = D[t.month % 10];
    buf[7] = '-';
    buf[8] = D[(t.day / 10) % 10];
    buf[9] = D[t.day % 10];
    buf[10] = ' ';
    buf[11] = D[(t.hour / 10) % 10];
    buf[12] = D[t.hour % 10];
    buf[13] = ':';
    buf[14] = D[(t.minute / 10) % 10];
    buf[15] = D[t.minute % 10];
    buf[16] = ':';
    buf[17] = D[(t.second / 10) % 10];
    buf[18] = D[t.second % 10];
    buf[19] = '\0';
}

int persist_snapshot_create(const char *name) {
    if (!disk_found || !disk_installed) {
        return -1;
    }
    char ts[20];
    persist_timestamp_now(ts);
    diskfs_dev dev = ata_dev_plain();
    return diskfs_snapshot_create(&dev, name, ts);
}

int persist_snapshot_list(diskfs_snapshot_info *infos, int cap,
                          int *out_count) {
    if (!disk_found || !disk_installed) {
        if (out_count != NULL) {
            *out_count = 0;
        }
        return -1;
    }
    diskfs_dev dev = ata_dev_plain();
    return diskfs_snapshot_list(&dev, infos, cap, out_count);
}

int persist_snapshot_restore(const char *name) {
    if (!disk_found || !disk_installed) {
        return -1;
    }
    diskfs_dev dev = ata_dev_plain();
    return diskfs_snapshot_restore(&dev, name);
}

int persist_snapshot_delete(const char *name) {
    if (!disk_found || !disk_installed) {
        return -1;
    }
    diskfs_dev dev = ata_dev_plain();
    return diskfs_snapshot_delete(&dev, name);
}

int persist_key_ready(void) {
    return key_active;
}

int persist_get_key(uint8_t *out32) {
    if (!key_active || out32 == NULL) {
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        out32[i] = disk_key[i];
    }
    return 0;
}

void persist_set_key(const uint8_t *key32) {
    entropy_reseed(); /* IV カウンタの種を新しくする */
    for (int i = 0; i < 32; i++) {
        disk_key[i] = key32[i];
    }
    key_active = 1;
    /* カウンタの種は時計から取る。起動ごとにずれるので、
     * 前回の起動と同じ IV を使い回しにくい。エントロピー
     * プール (RDRAND があればその値も混ざる) も混ぜる。 */
    prism_rtc_time t;
    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    if (rtc_read(&t) == 0) {
        seed = (uint64_t)((2000 + t.year) * 372 + (int)t.month * 31 +
                          (int)t.day) *
                   86400ULL +
               (uint64_t)t.hour * 3600ULL + (uint64_t)t.minute * 60ULL +
               (uint64_t)t.second;
    }
    {
        uint8_t eb[8];
        entropy_bytes(eb, sizeof(eb));
        uint64_t extra = 0;
        for (int i = 0; i < 8; i++) {
            extra = (extra << 8) | (uint64_t)eb[i];
        }
        seed ^= extra;
    }
    iv_counter = seed == 0 ? 1 : seed;
}

void persist_clear_key(void) {
    for (int i = 0; i < 32; i++) {
        disk_key[i] = 0;
    }
    key_active = 0;
    enc_count = 0;
}

int persist_key_active(void) {
    return key_active;
}

int persist_name_pending_decrypt(const char *name) {
    if (name == NULL) {
        return 0;
    }
    for (int i = 0; i < enc_count; i++) {
        if (strcmp(enc_names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

int persist_decrypt_loaded(void) {
    if (!key_active || enc_count == 0) {
        return 0;
    }
    static uint8_t plain_buf[DISKFS_MAX_FILE_SIZE];
    int done = 0;
    for (int i = 0; i < enc_count; i++) {
        size_t len = 0;
        const char *data = vfs_read_ptr(enc_names[i], &len);
        if (data == NULL) {
            continue;
        }
        crypto_aes256_ctr(disk_key, enc_ivs[i], (const uint8_t *)data,
                          plain_buf, len);
        if (vfs_write(enc_names[i], (const char *)plain_buf, len) == 0) {
            done++;
        }
    }
    enc_count = 0;
    last_sync_seq = vfs_change_seq();
    if (done > 0) {
        serial_write("[boot] disk: decrypted ");
        print_u64_serial((uint64_t)done);
        serial_write_line(done == 1 ? " encrypted file"
                                    : " encrypted files");
    }
    return done;
}

int persist_disk_present(void) {
    return disk_found;
}

int persist_installed(void) {
    return disk_installed;
}

/* VFS の全ファイルをディスクに書き、ディスクにあって VFS に
 * 無いファイルをディスクから消す。minimal = 1 のときは構成
 * に入るファイルだけを書き、それ以外はディスクから消す。 */
static int sync_filtered(int minimal) {
    diskfs_dev dev = ata_dev();
    static diskfs_entry entries[DISKFS_MAX_FILES];
    int count = 0;
    if (diskfs_list(&dev, entries, &count) != 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (!vfs_exists(entries[i].name) ||
            !install_include_file(entries[i].name, minimal)) {
            if (!key_active && entries[i].encrypted) {
                continue; /* 暗号文は鍵が無い間は消さない */
            }
            if (diskfs_delete_file(&dev, entries[i].name) != 0) {
                return -1;
            }
        }
    }
    int n = vfs_file_count();
    for (int i = 0; i < n; i++) {
        const char *name = vfs_file_name(i);
        if (name == NULL) {
            continue;
        }
        if (!install_include_file(name, minimal)) {
            continue;
        }
        size_t len = 0;
        const char *data = vfs_read_ptr(name, &len);
        /* ".account" と "settings.txt" は平文で書く。
         * ログイン前・起動時に読むため。settings.txt に
         * 秘密情報は無い (見た目と更新先の設定だけ)。
         * audit.log は他の利用者ファイルと同じく暗号化する。
         * ログイン前は暗号文へ追記できないため、出来事は
         * RAM の保留キューに貯め、復号直後に書き込む
         * (shell.c の監査 sink)。改ざん検知は従来通り
         * ハッシュチェーンが担う。
         * "remote.conf" も平文: 起動時のリモート確認は
         * ログインの前に走り、サーバと合言葉を読む必要が
         * ある。合言葉は PIN そのものではなくそのハッシュ
         * だが、読める場所に置く設計上の取捨である。
         * "remote.lock" も同じ理由で平文 (中身は時刻だけ)。
         * "users.txt" も平文: 追加ユーザーのログイン判定と
         * TOTP の門は復号の前に要る。中身はソルト付き
         * ハッシュと包んだ主鍵と TOTP 秘密だけで、主鍵は
         * 検査値付きで包まれているため平文の包みからは
         * 復元できない。 */
        diskfs_dev wdev = dev;
        if (strcmp(name, PRISM_ACCOUNT_FILE) == 0 ||
            strcmp(name, "settings.txt") == 0 ||
            strcmp(name, "remote.conf") == 0 ||
            strcmp(name, "preaudit.log") == 0 ||
            strcmp(name, "users.txt") == 0 ||
            strcmp(name, "remote.lock") == 0) {
            wdev = ata_dev_plain();
        }
        /* 鍵がまだ無い間は、ディスク上で暗号化されて
         * いるファイルを書き戻さない。VFS の中身は
         * 暗号文のままで、それを平文として書き戻すと
         * 暗号化フラグが落ちて平文で残ってしまう。
         * ディスク上の暗号文が正なので触らない。 */
        if (!key_active && persist_name_pending_decrypt(name)) {
            continue;
        }
        if (diskfs_write_file(&wdev, name, (const uint8_t *)data, len) != 0) {
            return -1;
        }
    }
    ata_flush_disk();
    last_sync_seq = vfs_change_seq();
    return 0;
}

static int sync_all(void) {
    return sync_filtered(0);
}

/* 外部メディア制限のときの書き戻し: settings.txt と
 * .account だけを平文で書く。それ以外のファイルは
 * 触らない (消さない・書かない)。 */
static int sync_policy_only(void) {
    size_t len = 0;
    const char *data = vfs_read_ptr("settings.txt", &len);
    if (data != NULL) {
        diskfs_dev pdev = ata_dev_plain();
        if (diskfs_write_file(&pdev, "settings.txt",
                              (const uint8_t *)data, len) != 0) {
            return -1;
        }
    }
    data = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
    if (data != NULL) {
        diskfs_dev pdev = ata_dev_plain();
        if (diskfs_write_file(&pdev, PRISM_ACCOUNT_FILE,
                              (const uint8_t *)data, len) != 0) {
            return -1;
        }
    }
    last_sync_seq = vfs_change_seq();
    return 0;
}

int persist_media_blocked(void) {
    return media_blocked;
}

int persist_force_sync(void) {
    if (sync_blocked) {
        return 0;
    }
    if (!disk_found || !disk_installed) {
        return 0;
    }
    if (media_blocked) {
        return sync_policy_only() == 0 ? 1 : -1;
    }
    if (sync_all() != 0) {
        return -1;
    }
    return 1;
}

/* ディスクの settings.txt を平文で読んで、外部メディア制限
 * が on かどうかを返す。ファイルが無い・読めない・暗号文
 * (古い盤) なら 0 (許可)。 */
static int read_media_blocked(diskfs_dev *dev) {
    /* settings.txt はショートカット割り当ての行を含むため
     * 512 バイトを超える。直列化の上限 (1024) に合わせる。 */
    static uint8_t sbuf[1024];
    size_t len = 0;
    if (diskfs_read_file_raw(dev, "settings.txt", sbuf, sizeof(sbuf),
                             &len) != 0) {
        return 0;
    }
    prism_settings s;
    settings_defaults(&s);
    if (!settings_parse(&s, (const char *)sbuf, len)) {
        return 0;
    }
    return s.usb_media;
}

int persist_load(void) {
    if (!disk_found || !disk_installed) {
        return 0;
    }
    diskfs_dev dev = ata_dev();
    media_blocked = read_media_blocked(&dev);
    if (media_blocked) {
        serial_write_line(
            "[boot] disk: external media is blocked by the "
            "administrator (settings.txt and .account only).");
    }
    static diskfs_entry entries[DISKFS_MAX_FILES];
    int count = 0;
    if (diskfs_list(&dev, entries, &count) != 0) {
        serial_write_line("[boot] disk: (failed to read file list)");
        return -1;
    }
    /* 組み込みファイルを全部消してから、ディスクの内容を載せる。
     * こうしないと、ディスクで消したファイルが組み込みとして
     * 復活してしまう。外部メディア制限のときは組み込みは
     * 残す (ディスクの利用者ファイルだけを読まない)。 */
    int n = vfs_file_count();
    for (int i = n - 1; i >= 0; i--) {
        const char *name = vfs_file_name(i);
        if (name != NULL) {
            if (media_blocked && vfs_is_builtin(name)) {
                continue;
            }
            vfs_delete(name);
        }
    }
    static uint8_t load_buf[DISKFS_MAX_FILE_SIZE];
    int loaded = 0;
    enc_count = 0;
    for (int i = 0; i < count; i++) {
        /* 外部メディア制限のときは settings.txt と .account
         * だけを載せる。 */
        if (media_blocked &&
            strcmp(entries[i].name, "settings.txt") != 0 &&
            strcmp(entries[i].name, PRISM_ACCOUNT_FILE) != 0) {
            continue;
        }
        size_t len = 0;
        /* 鍵がまだ無いので、暗号文のまま読む。ログイン後に
         * persist_decrypt_loaded() で復号する。 */
        if (diskfs_read_file_raw(&dev, entries[i].name, load_buf,
                                 sizeof(load_buf), &len) != 0) {
            continue;
        }
        if (entries[i].encrypted && enc_count < DISKFS_MAX_FILES) {
            size_t n = 0;
            while (entries[i].name[n] != '\0' &&
                   n < DISKFS_MAX_NAME) {
                enc_names[enc_count][n] = entries[i].name[n];
                n++;
            }
            enc_names[enc_count][n] = '\0';
            for (int j = 0; j < 16; j++) {
                enc_ivs[enc_count][j] = entries[i].iv[j];
            }
            enc_count++;
        }
        if (vfs_write(entries[i].name, (const char *)load_buf, len) == 0) {
            /* ディスク側の更新時刻をそのまま引き継ぐ
             * (vfs_write が今の時刻で上書きしてしまうため)。 */
            vfs_set_mtime(entries[i].name, entries[i].mtime);
            loaded++;
        }
    }
    last_sync_seq = vfs_change_seq();
    serial_write("[boot] disk: loaded ");
    print_u64_serial((uint64_t)loaded);
    serial_write_line(" files from disk");
    return loaded;
}

void persist_block_sync(int on) {
    sync_blocked = on;
}

int persist_maybe_sync(void) {
    if (sync_blocked) {
        return 0;
    }
    if (!disk_found || !disk_installed) {
        return 0;
    }
    if (vfs_change_seq() == last_sync_seq) {
        return 0;
    }
    if (media_blocked) {
        return sync_policy_only() == 0 ? 1 : -1;
    }
    if (sync_all() != 0) {
        return -1;
    }
    return 1;
}

int persist_install(void) {
    return persist_install_filtered(0);
}

/* 構成を指定してディスクにインストールする。minimal = 1 は
 * 最小構成 (ブラウザ・ファイル管理・設定だけ)。データ用
 * パーティションがある盤ではそこだけを書き、MBR も起動用
 * パーティションも触らない。成功したら 0。ディスクが無ければ
 * -1、失敗したら -2。 */
int persist_install_filtered(int minimal) {
    if (!disk_found) {
        return -1;
    }
    /* 外部メディア制限のときはインストールを拒否する。 */
    if (media_blocked) {
        return -2;
    }
    /* FDE 有効なディスクへの再インストールは、先に暗号化を
     * 外してから行う (初期化で暗号化は解除される、という
     * 定義済みの振る舞い)。ロック解除済みならその場で復号
     * し、ロック中ならヘッダを消して平文として初期化する
     * (中身はどちらにせよ書式化で消える)。 */
    if (fde_present) {
        if (fde_unlocked) {
            if (fde_mig_dir != 0 ||
                persist_fde_disable() != 0) {
                return -2;
            }
        } else {
            uint8_t zero[BLOCK_SECTOR_SIZE];
            memset(zero, 0, sizeof(zero));
            if (block_write_sector(disk_base_lba, zero) != 0) {
                return -2;
            }
            fde_forget_state();
        }
    }
    diskfs_dev dev = ata_dev();
    if (diskfs_format(&dev) != 0) {
        return -2;
    }
    disk_installed = 1;
    if (sync_filtered(minimal) != 0) {
        return -2;
    }
    return 0;
}

/* インストールの検証: 構成に入る VFS のファイルが全部ディスク
 * にあるか確かめる。全部あればそのファイル数を、1 つでも
 * 無ければ -1、ディスクが無い・未インストールなら -2 を返す。 */
int persist_verify_install(int minimal) {
    if (!disk_found || !disk_installed) {
        return -2;
    }
    diskfs_dev dev = ata_dev();
    static diskfs_entry entries[DISKFS_MAX_FILES];
    int count = 0;
    if (diskfs_list(&dev, entries, &count) != 0) {
        return -1;
    }
    int verified = 0;
    int n = vfs_file_count();
    for (int i = 0; i < n; i++) {
        const char *name = vfs_file_name(i);
        if (name == NULL || !install_include_file(name, minimal)) {
            continue;
        }
        int found = 0;
        for (int j = 0; j < count; j++) {
            if (strcmp(entries[j].name, name) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return -1;
        }
        verified++;
    }
    return verified;
}

/* ---- インストーラー用のディスク情報 ---- */

const char *persist_disk_model(void) {
    if (!disk_found) {
        return "";
    }
    return block_model();
}

uint32_t persist_disk_sectors(void) {
    if (!disk_found) {
        return 0;
    }
    return (uint32_t)block_total_sectors();
}

int persist_data_partition_present(void) {
    return disk_found && disk_base_lba != 0;
}

int persist_read_mbr(uint8_t *buf512) {
    if (!disk_found || buf512 == NULL) {
        return -1;
    }
    return block_read_sector(0, buf512);
}

int persist_install_dual(void) {
    if (!disk_found) {
        return -1;
    }
    if (disk_base_lba == 0) {
        /* パーティションが無い盤: デュアルブートの盤ではない。
         * ここで persist_install() を呼ぶと MBR を書き換えて
         * しまうので、拒否する。 */
        return -2;
    }
    /* disk_base_lba > 0 なら diskfs_format() は MBR を書かず、
     * データパーティションだけを初期化する。 */
    if (persist_install() != 0) {
        return -3;
    }
    return 0;
}
