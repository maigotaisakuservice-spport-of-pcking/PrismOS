/* PrismOS — ARM カーネルメイン (AArch64)
 *
 * 起動の流れ:
 *   1. PL011 UART を初期化し、バナーを出す
 *   2. MMU を有効化する (恒等写像)
 *   3. GIC と汎用タイマを初期化し、割り込みを許可する。
 *      50 刻み (500ms) 待って、割り込みが本当に来ることを確かめる
 *   4. PMM・ヒープ・VFS を初期化し、x86 版と同じシェルコアを動かす
 *   5. UART から 1 行ずつ読み、シェルコアに渡す
 *
 * メモリ量は DTB (起動時に x0 で渡る) の /memory ノードから
 * 読む。読めなければ 256MB とみなす。PMM にはカーネル直後から
 * RAM の末尾までを渡す。
 * シェルは x86 版と同じコマンド (help / ver / ls / cat / echo /
 * rm / touch / cd / pwd / theme / reboot) が動く。ARM だけの
 * コマンドとして mem (メモリ状態) と ticks (タイマ刻み) がある。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "account.h"
#include "core.h"
#include "crypto.h"
#include "diskfs.h"
#include "fb.h"
#include "fdt.h"
#include "gic.h"
#include "heap.h"
#include "mmu.h"
#include "pmm.h"
#include "ramfb.h"
#include "timer.h"
#include "uart.h"
#include "vfs.h"
#include "virtio_blk.h"
#include "virtio_input.h"
#include "netstack.h"
#include "pl031.h"
#include "security.h"
#include "string.h"
#include "totp.h"
#include "users.h"

extern char __kernel_end[];
extern char arm_vectors[];

#define ARM_RAM_FALLBACK_END 0x50000000ULL /* DTB が読めない場合: 256MB */
/* RAM 末尾の上限: TCR の物理アドレスが 36 ビット (64GB) のため。
 * これを超える末尾は DTB の値がおかしいとして採用しない。 */
#define ARM_RAM_MAX_END 0x1000000000ULL
#define ARM_TIMER_WAIT_TICKS 50    /* 起動時に待つ刻み数 (500ms) */
#define ARM_TIMER_WAIT_SECS 3      /* 刻み待ちの制限 (秒) */

static void uart_write_u64(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[20] = '\0';
    if (v == 0) {
        arm_uart_putc('0');
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        buf[i] = (char)('0' + (v % 10));
        v /= 10;
    }
    arm_uart_write(&buf[i]);
}

static void uart_write_hex64(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    int i;
    arm_uart_write("0x");
    for (i = 60; i >= 0; i -= 4) {
        arm_uart_putc(digits[(v >> i) & 0xfu]);
    }
}

/* 同期例外 (ベクタ表から呼ばれる)。原因を出して止まる。 */
void arm_sync_handler(uint64_t esr, uint64_t elr) {
    arm_uart_write("\r\n[exception] ESR=");
    uart_write_hex64(esr);
    arm_uart_write(" ELR=");
    uart_write_hex64(elr);
    arm_uart_write_line("");
    arm_uart_write_line("[exception] halting.");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* PSCI の SYSTEM_RESET (0x84000009) を呼ぶ。HVC で試し、
 * 戻ってきたら SMC でも試す。どちらも戻ってきたら失敗。 */
static void psci_reset(void) {
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
    x0 = 0x84000009ULL;
    __asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
}

static void cmd_mem(void) {
    arm_uart_write("memory: ");
    uart_write_u64(pmm_total_pages());
    arm_uart_write(" pages total, ");
    uart_write_u64(pmm_free_pages_count());
    arm_uart_write(" free, heap ");
    uart_write_u64((uint64_t)heap_free_bytes());
    arm_uart_write_line(" bytes free");
}

static void cmd_ticks(void) {
    arm_uart_write("ticks: ");
    uart_write_u64(arm_timer_ticks());
    arm_uart_write_line("");
}

/* ramfb が使えれば壁紙とタスクバーを描く。使えなければ
 * シリアルだけのままにする (正直に報告する)。 */
static void arm_draw_desktop(void) {
    uint64_t fb_phys = arm_ramfb_init(1024, 768);
    if (fb_phys == 0) {
        arm_uart_write_line("[boot] fb: ramfb not available, serial only");
        return;
    }
    fb_init((void *)(uintptr_t)fb_phys, 1024, 768, 1024 * 4, 32,
            8, 16, 8, 8, 8, 0);
    if (!fb_ready()) {
        arm_uart_write_line("[boot] fb: fb_init failed, serial only");
        return;
    }
    /* 壁紙: 上の濃い青から下の紫への縦グラデーション。 */
    for (uint64_t y = 0; y < 720; y++) {
        uint32_t r = (uint32_t)(11 + (124 - 11) * y / 720);
        uint32_t g = (uint32_t)(16 + (92 - 16) * y / 720);
        uint32_t b = (uint32_t)(38 + (255 - 38) * y / 720);
        fb_fill_rect(0, y, 1024, 1, fb_rgb((uint8_t)r, (uint8_t)g,
                                           (uint8_t)b));
    }
    /* 中央にひし形 (プリズム) を描く。 */
    uint32_t prism = fb_rgb(76, 194, 255);
    fb_draw_line(512, 200, 712, 360, prism);
    fb_draw_line(712, 360, 512, 520, prism);
    fb_draw_line(512, 520, 312, 360, prism);
    fb_draw_line(312, 360, 512, 200, prism);
    fb_draw_line(512, 280, 632, 360, fb_rgb(255, 111, 181));
    fb_draw_line(632, 360, 512, 440, fb_rgb(255, 111, 181));
    fb_draw_line(512, 440, 392, 360, fb_rgb(255, 111, 181));
    fb_draw_line(392, 360, 512, 280, fb_rgb(255, 111, 181));
    /* タスクバー (下 48 ピクセル) と文字。 */
    fb_fill_rect(0, 720, 1024, 48, fb_rgb(11, 16, 38));
    fb_fill_rect(0, 720, 1024, 2, fb_rgb(76, 194, 255));
    fb_draw_text(16, 736, "PrismOS 27H1", fb_rgb(255, 255, 255),
                 fb_rgb(11, 16, 38), 2);
    fb_draw_text(880, 736, "ARM64", fb_rgb(76, 194, 255),
                 fb_rgb(11, 16, 38), 2);
    /* コンソールを画面内のウィンドウとして整える (F1 の
     * 第一段階): 壁紙の上にタイトルバーつきのパネルを置き、
     * テキストはその中だけに描く。 */
    fb_fill_rect(16, 16, 992, 688, fb_rgb(11, 16, 38));
    fb_fill_rect(16, 16, 992, 30, fb_rgb(27, 35, 64));
    fb_draw_text(28, 23, "Terminal - PrismOS shell",
                 fb_rgb(255, 255, 255), fb_rgb(27, 35, 64), 1);
    for (uint64_t i = 0; i < 992; i++) {
        fb_put_pixel(16 + i, 16, fb_rgb(76, 194, 255));
        fb_put_pixel(16 + i, 703, fb_rgb(76, 194, 255));
    }
    for (uint64_t i = 0; i < 688; i++) {
        fb_put_pixel(16, 16 + i, fb_rgb(76, 194, 255));
        fb_put_pixel(1007, 16 + i, fb_rgb(76, 194, 255));
    }
    arm_uart_write("[boot] fb: ramfb 1024x768 at ");
    uart_write_hex64(fb_phys);
    arm_uart_write_line("");
}


/* ---- 画面コンソール (ramfb 上のテキスト表示) ----
 * UART 出力をそのまま画面にも出す。セルは 9x17 (8x16
 * フォント + 間隔)、領域はタスクバーの上まで。行が溢れたら
 * 行リングをずらして全体を描き直す。 */
#define CON_COLS 106
#define CON_ROWS 37
#define CON_X 28
#define CON_Y 56

static char con_lines[CON_ROWS][CON_COLS + 1];
static int con_col;
static int con_row;
static int con_ready;

static uint32_t con_bg(void) { return fb_rgb(11, 16, 38); }
static uint32_t con_fg(void) { return fb_rgb(235, 240, 255); }

/* 画面コンソールの再描画。壁紙の上に文字だけを載せる：
 * 領域全体を背景色で塗りつぶすと壁紙のグラデーションと
 * ひし形が消えてしまう (2026-11-24 に画面テストで発覚)
 * ため、各行を文字セル単位で描き直す。行末まで空白で
 * 埋めることで、スクロールで残った古い字も消える。 */
static void con_redraw(void) {
    static char pad[CON_COLS + 1];
    static int pad_ready = 0;
    if (!pad_ready) {
        for (int k = 0; k < CON_COLS; k++) {
            pad[k] = ' ';
        }
        pad[CON_COLS] = '\0';
        pad_ready = 1;
    }
    for (int r = 0; r < CON_ROWS; r++) {
        size_t len = 0;
        while (con_lines[r][len] != '\0') {
            len++;
        }
        fb_draw_text(CON_X, CON_Y + (uint64_t)r * 17, con_lines[r],
                     con_fg(), con_bg(), 1);
        if (len < (size_t)CON_COLS) {
            fb_draw_text(CON_X + (uint64_t)len * 9,
                         CON_Y + (uint64_t)r * 17,
                         pad + len, con_fg(), con_bg(), 1);
        }
    }
}

static void con_putc(char c) {
    if (!con_ready) {
        return;
    }
    if (c == '\r') {
        con_col = 0;
        return;
    }
    if (c == '\n') {
        con_col = 0;
        con_row++;
    } else if (c == '\b') {
        if (con_col > 0) {
            con_col--;
        }
        return;
    } else {
        if (con_col < CON_COLS) {
            con_lines[con_row][con_col] = c;
            con_lines[con_row][con_col + 1] = '\0';
            char one[2];
            one[0] = c;
            one[1] = '\0';
            fb_draw_text(CON_X + (uint64_t)con_col * 9,
                         CON_Y + (uint64_t)con_row * 17, one,
                         con_fg(), con_bg(), 1);
            con_col++;
        }
        if (con_col >= CON_COLS) {
            con_col = 0;
            con_row++;
        }
    }
    if (con_row >= CON_ROWS) {
        for (int r = 1; r < CON_ROWS; r++) {
            for (int k = 0; k <= CON_COLS; k++) {
                con_lines[r - 1][k] = con_lines[r][k];
            }
        }
        for (int k = 0; k <= CON_COLS; k++) {
            con_lines[CON_ROWS - 1][k] = '\0';
        }
        con_row = CON_ROWS - 1;
        con_redraw();
    }
}

static void con_init(void) {
    for (int r = 0; r < CON_ROWS; r++) {
        con_lines[r][0] = '\0';
    }
    con_col = 0;
    con_row = 0;
    con_ready = 1;
    /* ここで領域全体を背景色で塗ると壁紙が消えるため塗らない。
     * 文字は con_putc がセル単位で背景つきで描く。 */
    arm_uart_set_console_sink(con_putc);
}

/* シリアルと virtio キーボードの両方から 1 行読む。
 * エコーは arm_uart_putc 経由 (画面にも出る)。 */
static size_t arm_read_line_merged(char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        int c = arm_uart_try_getc();
        if (c < 0) {
            c = arm_virtio_input_poll_char();
            if (c == 0) {
                continue;
            }
        }
        if (c == '\r' || c == '\n') {
            arm_uart_putc('\n');
            break;
        }
        if (c == 0x7f || c == 0x08) {
            if (n > 0) {
                n--;
                arm_uart_putc('\b');
                arm_uart_putc(' ');
                arm_uart_putc('\b');
            }
            continue;
        }
        if (c < 0x20 || c >= 0x7f) {
            continue;
        }
        if (n + 1 < cap) {
            buf[n] = (char)c;
            n++;
            arm_uart_putc((char)c);
        }
    }
    buf[n] = '\0';
    return n;
}

/* ---- ディスク (virtio-blk + diskfs) ---- */

static diskfs_dev arm_disk;
static int arm_disk_ready;
static uint8_t arm_file_buf[DISKFS_MAX_FILE_SIZE];
static diskfs_entry arm_entries[DISKFS_MAX_FILES];

static int arm_disk_read_sector(void *ctx, uint32_t lba, uint8_t *buf) {
    (void)ctx;
    return arm_blk_read(lba, buf);
}

static int arm_disk_write_sector(void *ctx, uint32_t lba,
                                 const uint8_t *buf) {
    (void)ctx;
    return arm_blk_write(lba, buf);
}

/* ---- ログインと暗号化 (x86 版と対等化) ---- */

/* ディスク暗号化の鍵 (PIN から crypto_kdf で作る)。
 * 起動中のメモリにだけ置き、ディスクには書かない。 */
static uint8_t arm_disk_key[32];
static int arm_key_active;
/* アカウントがあるのにログインできていない状態。
 * この間 dsync は、暗号文を平文で上書きしないよう
 * 拒否する (x86 版の sync_blocked と同じ考え方)。 */
static int arm_locked;
/* ゲストセッション (鍵なし・保存しない)。dsync は拒否する。 */
static int arm_guest;
static uint64_t arm_iv_counter;

static void arm_make_iv(uint8_t *iv_out) {
    uint8_t buf[32 + 8];
    for (int i = 0; i < 32; i++) {
        buf[i] = arm_disk_key[i];
    }
    for (int i = 0; i < 8; i++) {
        buf[32 + i] =
            (uint8_t)((arm_iv_counter >> ((7 - i) * 8)) & 0xFF);
    }
    arm_iv_counter++;
    uint8_t hash[32];
    crypto_sha256(buf, sizeof(buf), hash);
    for (int i = 0; i < 16; i++) {
        iv_out[i] = hash[i];
    }
}

static int arm_encrypt_hook(void *ctx, uint8_t *iv_out,
                            const uint8_t *in, uint8_t *out,
                            size_t len) {
    (void)ctx;
    if (!arm_key_active) {
        return -1;
    }
    arm_make_iv(iv_out);
    crypto_aes256_ctr(arm_disk_key, iv_out, in, out, len);
    return 0;
}

static int arm_decrypt_hook(void *ctx, const uint8_t *iv,
                            const uint8_t *in, uint8_t *out,
                            size_t len) {
    (void)ctx;
    if (!arm_key_active) {
        return -1;
    }
    crypto_aes256_ctr(arm_disk_key, iv, in, out, len);
    return 0;
}

static const diskfs_crypto arm_crypto = {
    NULL, arm_encrypt_hook, arm_decrypt_hook,
};

static void arm_set_key(const uint8_t *key32) {
    for (int i = 0; i < 32; i++) {
        arm_disk_key[i] = key32[i];
    }
    arm_key_active = 1;
    arm_iv_counter = arm_timer_now() ^ 0x9E3779B97F4A7C15ULL;
    if (arm_iv_counter == 0) {
        arm_iv_counter = 1;
    }
    arm_disk.crypto = &arm_crypto;
}

/* ソルト: 汎用タイマの生カウンタを少しずつずらして
 * 16 回読み、SHA-256 で潰して 16 バイト取る。QEMU の
 * タイマは決定的に近いので熵は弱い (実機では TRNG が
 * 前提。強さの嘘はつかない)。 */
static void arm_make_salt(uint8_t *salt16) {
    uint8_t samples[16 * 8];
    for (int i = 0; i < 16; i++) {
        uint64_t t = arm_timer_now();
        while (arm_timer_now() == t) {
        }
        t = arm_timer_now() + arm_timer_ticks();
        for (int j = 0; j < 8; j++) {
            samples[i * 8 + j] = (uint8_t)(t >> (j * 8));
        }
    }
    uint8_t hash[32];
    crypto_sha256(samples, sizeof(samples), hash);
    for (int i = 0; i < 16; i++) {
        salt16[i] = hash[i];
    }
}

/* 起動時に暗号文のまま読み込んだファイル (ログイン後に
 * 復号する)。x86 版 persist.c の enc_names と同じ仕組み。 */
static char arm_enc_names[DISKFS_MAX_FILES][DISKFS_MAX_NAME + 1];
static uint8_t arm_enc_ivs[DISKFS_MAX_FILES][16];
static int arm_enc_count;

/* ディスク上のファイルを全部、復号せずに VFS へ読み込む。
 * 暗号化されたファイルは暗号文のまま入り、IV を控える。 */
static int arm_disk_load_raw(void) {
    int count = 0, i;
    size_t len = 0;
    if (!arm_disk_ready ||
        diskfs_list(&arm_disk, arm_entries, &count) != 0) {
        return -1;
    }
    arm_enc_count = 0;
    for (i = 0; i < count; i++) {
        if (!arm_entries[i].used) {
            continue;
        }
        if (diskfs_read_file_raw(&arm_disk, arm_entries[i].name,
                                 arm_file_buf, sizeof(arm_file_buf),
                                 &len) == 0) {
            vfs_write(arm_entries[i].name,
                      (const char *)arm_file_buf, len);
            if (arm_entries[i].encrypted &&
                arm_enc_count < DISKFS_MAX_FILES) {
                int j = 0;
                while (arm_entries[i].name[j] != '\0' &&
                       j < DISKFS_MAX_NAME) {
                    arm_enc_names[arm_enc_count][j] =
                        arm_entries[i].name[j];
                    j++;
                }
                arm_enc_names[arm_enc_count][j] = '\0';
                for (int k = 0; k < 16; k++) {
                    arm_enc_ivs[arm_enc_count][k] =
                        arm_entries[i].iv[k];
                }
                arm_enc_count++;
            }
        }
    }
    return count;
}

/* ログインで鍵ができたら、暗号文のまま読み込んだ
 * ファイルをその場で復号する。復号した件数を返す。 */
static int arm_decrypt_loaded(void) {
    static uint8_t plain[DISKFS_MAX_FILE_SIZE];
    int done = 0;
    if (!arm_key_active) {
        return 0;
    }
    for (int i = 0; i < arm_enc_count; i++) {
        size_t len = 0;
        const char *data = vfs_read_ptr(arm_enc_names[i], &len);
        if (data == NULL) {
            continue;
        }
        crypto_aes256_ctr(arm_disk_key, arm_enc_ivs[i],
                          (const uint8_t *)data, plain, len);
        if (vfs_write(arm_enc_names[i], (const char *)plain,
                      len) == 0) {
            done++;
        }
    }
    arm_enc_count = 0;
    return done;
}

/* ログイン中のユーザー名 ("admin" / 追加ユーザー名 /
 * "guest")。totp setup/off の対象を決めるのに使う。 */
static char arm_current_user[PRISM_USER_NAME_MAX + 1] = "admin";

/* TOTP の再使用防止 (x86 版 totp_gate と同じ作法): 最後に
 * 受理したステップを、秘密の SHA-256 先頭 8 バイト (タグ)
 * と一緒に覚え、同じ秘密・同じステップは二度と受理しない。 */
static uint8_t arm_totp_used_tag[8];
static uint64_t arm_totp_used_step;
static int arm_totp_used_valid;

/* TOTP の門。秘密が無ければ素通り (1)。あれば 3 回まで
 * コードを聞く。時刻は PL031 の実時刻 (Unix 秒、UTC) を
 * 使うので、認証アプリのコードとそのまま照合できる。 */
static int arm_totp_gate(const char *name) {
    size_t ulen = 0;
    const char *utext = vfs_read_ptr(PRISM_USERS_FILE, &ulen);
    if (utext == NULL) {
        return 1;
    }
    char b32[64];
    if (users_totp(utext, ulen, name, b32, sizeof(b32)) != 0) {
        return 1;
    }
    uint8_t secret[32];
    int slen = base32_decode(b32, secret, sizeof(secret));
    if (slen <= 0) {
        arm_uart_write_line("TOTP secret is damaged. Halting.");
        return 0;
    }
    uint64_t unix = pl031_unix_seconds();
    arm_uart_write("totp: unix=");
    uart_write_u64(unix);
    arm_uart_write_line("");
    for (int i = 0; i < 3; i++) {
        char line[32];
        arm_uart_write("Code: ");
        arm_read_line_merged(line, sizeof(line));
        uint32_t code = 0;
        int ok = 1;
        size_t n = 0;
        while (line[n] != '\0') {
            if (line[n] < '0' || line[n] > '9') {
                ok = 0;
                break;
            }
            code = code * 10 + (uint32_t)(line[n] - '0');
            n++;
        }
        if (ok && n == 6 &&
            totp_verify(secret, (size_t)slen, unix, code, 1)) {
            uint64_t step = unix / 30;
            int64_t matched = -1;
            for (int w = -1; w <= 1; w++) {
                uint64_t ctr = (uint64_t)((int64_t)step + w);
                if (hotp_code(secret, (size_t)slen, ctr, 6) ==
                    code) {
                    matched = (int64_t)ctr;
                    break;
                }
            }
            uint8_t digest[32];
            crypto_sha256(secret, (size_t)slen, digest);
            int same_tag = arm_totp_used_valid;
            for (int k = 0; k < 8; k++) {
                if (digest[k] != arm_totp_used_tag[k]) {
                    same_tag = 0;
                }
            }
            if (matched >= 0 && same_tag &&
                (uint64_t)matched <= arm_totp_used_step) {
                arm_uart_write_line("Code already used.");
                audit_log_event("login-2fa-fail",
                                "TOTP code reuse");
                memset(digest, 0, sizeof(digest));
                continue;
            }
            if (matched >= 0) {
                for (int k = 0; k < 8; k++) {
                    arm_totp_used_tag[k] = digest[k];
                }
                arm_totp_used_step = (uint64_t)matched;
                arm_totp_used_valid = 1;
            }
            memset(digest, 0, sizeof(digest));
            arm_uart_write_line("Code accepted.");
            audit_log_event("login-2fa", "TOTP accepted");
            memset(secret, 0, sizeof(secret));
            return 1;
        }
        arm_uart_write_line("Wrong code.");
        audit_log_event("login-2fa-fail", "wrong TOTP code");
    }
    memset(secret, 0, sizeof(secret));
    return 0;
}

/* 追加ユーザーのログイン: PIN を確かめ、users.txt の
 * 鍵ラップを開いて主鍵を取り出し、読み込み済みファイルを
 * 復号する (x86 版 login_flow_multi と同じ鍵ラップ方式)。 */
static void arm_login_user(const char *utext, size_t ulen,
                           const char *name) {
    char pin[32];
    for (int attempt = 0; attempt < 3; attempt++) {
        arm_uart_write("PIN: ");
        arm_read_line_merged(pin, sizeof(pin));
        if (users_verify(utext, ulen, name, pin) == 1) {
            if (!arm_totp_gate(name)) {
                for (size_t i = 0; pin[i] != '\0'; i++) {
                    pin[i] = 0;
                }
                arm_locked = 1;
                arm_uart_write_line(
                    "[boot] login: locked (TOTP required)");
                return;
            }
            uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
            uint32_t iter = 0;
            uint8_t uk[32], wrap[32], checkb[8], mk[32];
            int ok =
                users_salt_iter(utext, ulen, name, salt, &iter) == 0;
            if (ok) {
                crypto_kdf(pin, salt, PRISM_ACCOUNT_SALT_LEN,
                           iter != 0 ? iter
                                     : PRISM_ACCOUNT_ITERATIONS,
                           uk);
                ok = users_get_wrap(utext, ulen, name, wrap,
                                    checkb) == 0 &&
                     users_unwrap(wrap, checkb, uk, mk) == 0;
                for (int i = 0; i < 32; i++) {
                    uk[i] = 0;
                }
            }
            for (size_t i = 0; pin[i] != '\0'; i++) {
                pin[i] = 0;
            }
            if (!ok) {
                arm_uart_write_line(
                    "[boot] login: no key wrap for this user "
                    "(locked)");
                arm_locked = 1;
                return;
            }
            arm_set_key(mk);
            for (int i = 0; i < 32; i++) {
                mk[i] = 0;
            }
            {
                size_t ci = 0;
                while (name[ci] != '\0' &&
                       ci + 1 < sizeof(arm_current_user)) {
                    arm_current_user[ci] = name[ci];
                    ci++;
                }
                arm_current_user[ci] = '\0';
            }
            int dec = arm_decrypt_loaded();
            arm_uart_write("[boot] login: welcome ");
            arm_uart_write(name);
            arm_uart_write(" (");
            uart_write_u64((uint64_t)dec);
            arm_uart_write_line(" encrypted file(s) decrypted)");
            arm_locked = 0;
            return;
        }
        arm_uart_write_line("PIN: incorrect");
    }
    for (size_t i = 0; pin[i] != '\0'; i++) {
        pin[i] = 0;
    }
    arm_locked = 1;
    arm_uart_write_line(
        "[boot] login: locked (files stay encrypted; "
        "dsync is disabled)");
}

static int arm_disk_sync(void);

/* `totp setup`: ログイン中のユーザーの TOTP 秘密を作り、
 * users.txt に totp 行として保存する (x86 版と同じ形式)。
 * `totp off`: 自分の totp 行を消す。 */
static void arm_totp_setup(void) {
    if (arm_guest) {
        arm_uart_write_line("totp: guests cannot set up TOTP");
        return;
    }
    uint8_t secret[10];
    entropy_bytes(secret, sizeof(secret));
    char b32[24];
    if (base32_encode(secret, sizeof(secret), b32,
                      sizeof(b32)) < 0) {
        arm_uart_write_line("totp: could not make a secret");
        return;
    }
    char tline[96];
    size_t p = 0;
    const char *parts[4] = {"totp ", arm_current_user, " ", b32};
    for (int i = 0; i < 4; i++) {
        const char *s = parts[i];
        while (*s != '\0' && p + 1 < sizeof(tline)) {
            tline[p++] = *s++;
        }
    }
    tline[p] = '\0';
    static char text[PRISM_USERS_TEXT_MAX];
    static char out[PRISM_USERS_TEXT_MAX];
    size_t len = 0;
    const char *cur = vfs_read_ptr(PRISM_USERS_FILE, &len);
    if (cur == NULL) {
        text[0] = '\0';
        len = 0;
    } else {
        if (len + 1 > sizeof(text)) {
            arm_uart_write_line("totp: users.txt is too large");
            return;
        }
        memcpy(text, cur, len);
        text[len] = '\0';
    }
    if (users_upsert(text, len, tline, out, sizeof(out)) != 0 ||
        vfs_write(PRISM_USERS_FILE, out, strlen(out)) != 0 ||
        arm_disk_sync() < 0) {
        arm_uart_write_line("totp: could not save users.txt");
        return;
    }
    arm_uart_write(
        "TOTP secret (enter it in your authenticator app): ");
    arm_uart_write_line(b32);
    uint32_t code = totp_code(secret, sizeof(secret),
                              pl031_unix_seconds(), 30, 6);
    char num[8];
    for (int i = 5; i >= 0; i--) {
        num[i] = (char)('0' + (code % 10));
        code /= 10;
    }
    num[6] = '\0';
    arm_uart_write("Current code should read: ");
    arm_uart_write_line(num);
    memset(secret, 0, sizeof(secret));
    audit_log_event("totp-setup", arm_current_user);
}

static void arm_totp_off(void) {
    if (arm_guest) {
        arm_uart_write_line("totp: guests cannot set up TOTP");
        return;
    }
    static char text[PRISM_USERS_TEXT_MAX];
    static char out[PRISM_USERS_TEXT_MAX];
    size_t len = 0;
    const char *cur = vfs_read_ptr(PRISM_USERS_FILE, &len);
    if (cur == NULL || len == 0) {
        arm_uart_write_line("TOTP is not set up.");
        return;
    }
    if (len + 1 > sizeof(text)) {
        arm_uart_write_line("totp: users.txt is too large");
        return;
    }
    memcpy(text, cur, len);
    text[len] = '\0';
    size_t op = 0;
    size_t off = 0;
    int dropped = 0;
    size_t nlen = strlen(arm_current_user);
    while (off < len) {
        size_t st = off;
        while (off < len && text[off] != '\n') {
            off++;
        }
        size_t ll = off - st;
        if (off < len) {
            off++;
        }
        int drop = 0;
        if (ll > 5 + nlen && memcmp(text + st, "totp ", 5) == 0 &&
            memcmp(text + st + 5, arm_current_user, nlen) == 0 &&
            text[st + 5 + nlen] == ' ') {
            drop = 1;
        }
        if (drop) {
            dropped = 1;
            continue;
        }
        memcpy(out + op, text + st, ll);
        op += ll;
        out[op++] = '\n';
    }
    out[op] = '\0';
    if (!dropped) {
        arm_uart_write_line("TOTP is not set up.");
        return;
    }
    if (vfs_write(PRISM_USERS_FILE, out, op) != 0 ||
        arm_disk_sync() < 0) {
        arm_uart_write_line("totp: could not save users.txt");
        return;
    }
    arm_uart_write_line("TOTP disabled for this user.");
}

/* 起動時のログイン: .account があれば PIN を最大 3 回
 * 聞き、合えば鍵を作って読み込み済みファイルを復号する。
 * users.txt に追加ユーザーがいれば、最初にユーザー名を
 * 聞く (空 = admin、guest = 保存しないゲスト)。 */
static void arm_boot_login(const char *acct, size_t alen) {
    size_t ulen = 0;
    const char *utext = vfs_read_ptr(PRISM_USERS_FILE, &ulen);
    if (utext != NULL && users_count(utext, ulen) > 0) {
        char name[PRISM_USER_NAME_MAX + 1];
        arm_uart_write("User (blank = admin, or guest): ");
        arm_read_line_merged(name, sizeof(name));
        if (strcmp(name, "guest") == 0) {
            arm_guest = 1;
            arm_locked = 1; /* 鍵なし: 復号も保存もしない */
            arm_current_user[0] = 'g';
            arm_current_user[1] = 'u';
            arm_current_user[2] = 'e';
            arm_current_user[3] = 's';
            arm_current_user[4] = 't';
            arm_current_user[5] = '\0';
            arm_uart_write_line(
                "[boot] login: welcome guest "
                "(session is not saved)");
            return;
        }
        if (name[0] != '\0' && strcmp(name, "admin") != 0) {
            if (!users_exists(utext, ulen, name)) {
                arm_locked = 1;
                arm_uart_write_line(
                    "[boot] login: no such user (locked)");
                return;
            }
            arm_login_user(utext, ulen, name);
            return;
        }
        /* admin: 下の従来フローへ */
    }
    char pin[32];
    for (int attempt = 0; attempt < 3; attempt++) {
        arm_uart_write("PIN: ");
        arm_read_line_merged(pin, sizeof(pin));
        if (account_verify(pin, acct, alen) == 1) {
            if (!arm_totp_gate("admin")) {
                for (size_t i = 0; pin[i] != '\0'; i++) {
                    pin[i] = 0;
                }
                arm_locked = 1;
                arm_uart_write_line(
                    "[boot] login: locked (TOTP required)");
                return;
            }
            arm_current_user[0] = 'a';
            arm_current_user[1] = 'd';
            arm_current_user[2] = 'm';
            arm_current_user[3] = 'i';
            arm_current_user[4] = 'n';
            arm_current_user[5] = '\0';
            uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
            uint32_t iter = 0;
            if (account_get_salt(acct, alen, salt, &iter) == 0) {
                uint8_t key[32];
                crypto_kdf(pin, salt, PRISM_ACCOUNT_SALT_LEN,
                           iter != 0 ? iter
                                     : PRISM_ACCOUNT_ITERATIONS,
                           key);
                arm_set_key(key);
                for (int i = 0; i < 32; i++) {
                    key[i] = 0;
                }
            }
            for (size_t i = 0; pin[i] != '\0'; i++) {
                pin[i] = 0;
            }
            int dec = arm_decrypt_loaded();
            arm_uart_write("[boot] login: welcome (");
            uart_write_u64((uint64_t)dec);
            arm_uart_write_line(" encrypted file(s) decrypted)");
            arm_locked = 0;
            return;
        }
        arm_uart_write_line("PIN: incorrect");
    }
    for (size_t i = 0; pin[i] != '\0'; i++) {
        pin[i] = 0;
    }
    arm_locked = 1;
    arm_uart_write_line(
        "[boot] login: locked (files stay encrypted; "
        "dsync is disabled)");
}

/* `pinset`: アカウント (.account) を作り、PIN から鍵を
 * 作って暗号化を有効にする。.account 自体はログイン前に
 * 読むので平文で書く (x86 版と同じ)。 */
static void arm_pinset(void) {
    char pin1[32], pin2[32];
    if (!arm_disk_ready) {
        arm_uart_write_line("pinset: no disk");
        return;
    }
    arm_uart_write("New PIN: ");
    arm_read_line_merged(pin1, sizeof(pin1));
    arm_uart_write("Confirm PIN: ");
    arm_read_line_merged(pin2, sizeof(pin2));
    if (strcmp(pin1, pin2) != 0) {
        arm_uart_write_line("pinset: PINs do not match");
        return;
    }
    if (!account_pin_ok(pin1)) {
        arm_uart_write_line(
            "pinset: PIN must be 4-16 printable characters");
        return;
    }
    uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
    arm_make_salt(salt);
    char text[PRISM_ACCOUNT_TEXT_MAX];
    if (account_make_text(salt, PRISM_ACCOUNT_ITERATIONS, pin1,
                          text, sizeof(text)) != 0) {
        arm_uart_write_line("pinset: failed to build account");
        return;
    }
    size_t len = 0;
    while (text[len] != '\0') {
        len++;
    }
    if (diskfs_is_formatted(&arm_disk) != 1) {
        if (diskfs_format(&arm_disk) != 0) {
            arm_uart_write_line("pinset: format failed");
            return;
        }
    }
    diskfs_dev plain = arm_disk;
    plain.crypto = NULL;
    if (diskfs_write_file(&plain, PRISM_ACCOUNT_FILE,
                          (const uint8_t *)text, len) != 0) {
        arm_uart_write_line("pinset: could not write .account");
        return;
    }
    vfs_write(PRISM_ACCOUNT_FILE, text, len);
    uint8_t key[32];
    crypto_kdf(pin1, salt, PRISM_ACCOUNT_SALT_LEN,
               PRISM_ACCOUNT_ITERATIONS, key);
    /* 追加ユーザーの主鍵の包みを新しい主鍵へ包み直す
     * (x86 版 passwd と同じ。しないと PIN 変更で追加
     * ユーザーがログインできなくなる)。 */
    if (arm_key_active) {
        size_t ulen = 0;
        const char *utext = vfs_read_ptr(PRISM_USERS_FILE, &ulen);
        if (utext != NULL) {
            static char new_utext[PRISM_USERS_TEXT_MAX];
            int nwrap = 0;
            if (users_rewrap_all(utext, ulen, arm_disk_key, key,
                                 new_utext, sizeof(new_utext),
                                 &nwrap) == 0 &&
                nwrap > 0 &&
                vfs_write(PRISM_USERS_FILE, new_utext,
                          strlen(new_utext)) == 0) {
                (void)arm_disk_sync();
                arm_uart_write_line(
                    "pinset: re-wrapped the disk key for added "
                    "users");
            }
        }
    }
    arm_set_key(key);
    for (int i = 0; i < 32; i++) {
        key[i] = 0;
    }
    for (size_t i = 0; pin1[i] != '\0'; i++) {
        pin1[i] = 0;
        pin2[i] = 0;
    }
    arm_locked = 0;
    arm_uart_write_line(
        "pinset: account created; disk encryption is on "
        "(dsync encrypts)");
}

/* ディスク上のファイルを全部 VFS へ読み込む。読み込んだ件数を返す。 */
static int arm_disk_load(void) {
    int count = 0, i;
    size_t len = 0;
    if (!arm_disk_ready ||
        diskfs_list(&arm_disk, arm_entries, &count) != 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (!arm_entries[i].used) {
            continue;
        }
        if (diskfs_read_file(&arm_disk, arm_entries[i].name,
                             arm_file_buf, sizeof(arm_file_buf),
                             &len) == 0) {
            vfs_write(arm_entries[i].name, (const char *)arm_file_buf,
                      len);
        }
    }
    return count;
}

/* VFS のファイルを全部ディスクへ書く。書いた件数を返す。
 * ロック中 (アカウントあり・未ログイン) とゲスト時は
 * -2 を返して何も書かない。.account と users.txt は
 * ログイン前に読むため常に平文で書く (x86 版と同じ)。 */
static int arm_disk_sync(void) {
    int i, n = vfs_file_count(), written = 0;
    if (!arm_disk_ready) {
        return -1;
    }
    if (arm_locked || arm_guest) {
        return -2;
    }
    if (diskfs_is_formatted(&arm_disk) != 1) {
        if (diskfs_format(&arm_disk) != 0) {
            return -1;
        }
        arm_uart_write_line("[disk] formatted the disk");
    }
    for (i = 0; i < n; i++) {
        const char *name = vfs_file_name(i);
        size_t len = 0;
        const char *data = vfs_read_ptr(name, &len);
        if (data == NULL) {
            continue;
        }
        if (strcmp(name, PRISM_ACCOUNT_FILE) == 0 ||
            strcmp(name, PRISM_USERS_FILE) == 0) {
            diskfs_dev plain = arm_disk;
            plain.crypto = NULL;
            if (diskfs_write_file(&plain, name,
                                  (const uint8_t *)data,
                                  len) == 0) {
                written++;
            }
            continue;
        }
        if (diskfs_write_file(&arm_disk, name,
                              (const uint8_t *)data, len) == 0) {
            written++;
        }
    }
    return written;
}

/* `useradd <name>`: 追加ユーザーを作り、今の主鍵をその
 * ユーザーの PIN 由来鍵で包んで users.txt に書く
 * (x86 版と同じ鍵ラップ)。鍵が有効な管理者セッション
 * でのみ使える。 */
static void arm_useradd(const char *name) {
    if (!arm_disk_ready) {
        arm_uart_write_line("useradd: no disk");
        return;
    }
    if (arm_guest) {
        arm_uart_write_line("useradd: guests cannot manage users");
        return;
    }
    if (!arm_key_active) {
        arm_uart_write_line(
            "useradd: log in as admin first (no disk key)");
        return;
    }
    if (!users_name_ok(name)) {
        arm_uart_write_line("useradd: bad user name");
        return;
    }
    char utext[PRISM_USERS_TEXT_MAX];
    size_t ulen = 0;
    const char *cur = vfs_read_ptr(PRISM_USERS_FILE, &ulen);
    if (cur == NULL) {
        ulen = 0;
        utext[0] = '\0';
    } else {
        if (ulen >= sizeof(utext)) {
            ulen = sizeof(utext) - 1;
        }
        memcpy(utext, cur, ulen);
        utext[ulen] = '\0';
    }
    if (users_exists(utext, ulen, name)) {
        arm_uart_write_line("useradd: user already exists");
        return;
    }
    char pin1[32], pin2[32];
    arm_uart_write("New PIN: ");
    arm_read_line_merged(pin1, sizeof(pin1));
    arm_uart_write("Confirm PIN: ");
    arm_read_line_merged(pin2, sizeof(pin2));
    if (strcmp(pin1, pin2) != 0) {
        arm_uart_write_line("useradd: PINs do not match");
        return;
    }
    if (!account_pin_ok(pin1)) {
        arm_uart_write_line(
            "useradd: PIN must be 4-16 printable characters");
        return;
    }
    uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
    arm_make_salt(salt);
    char line1[256], line2[256];
    if (users_make_line(name, salt, PRISM_ACCOUNT_ITERATIONS,
                        pin1, "user", line1, sizeof(line1)) != 0) {
        arm_uart_write_line("useradd: could not build user record");
        return;
    }
    uint8_t uk[32];
    crypto_kdf(pin1, salt, PRISM_ACCOUNT_SALT_LEN,
               PRISM_ACCOUNT_ITERATIONS, uk);
    for (size_t i = 0; pin1[i] != '\0'; i++) {
        pin1[i] = 0;
        pin2[i] = 0;
    }
    int wok = users_make_wrap_line(name, arm_disk_key, uk, line2,
                                   sizeof(line2));
    for (int i = 0; i < 32; i++) {
        uk[i] = 0;
    }
    if (wok != 0) {
        arm_uart_write_line("useradd: could not build key wrap");
        return;
    }
    char ntext[PRISM_USERS_TEXT_MAX];
    if (users_upsert(utext, ulen, line1, ntext, sizeof(ntext)) != 0 ||
        users_upsert(ntext, strlen(ntext), line2, utext,
                     sizeof(utext)) != 0) {
        arm_uart_write_line("useradd: users.txt is full");
        return;
    }
    if (vfs_write(PRISM_USERS_FILE, utext, strlen(utext)) != 0 ||
        arm_disk_sync() < 0) {
        arm_uart_write_line("useradd: could not save users.txt");
        return;
    }
    arm_uart_write("useradd: added ");
    arm_uart_write_line(name);
}

/* `userlist`: admin と追加ユーザーの名前を並べる。 */
static void arm_userlist(void) {
    arm_uart_write_line("admin");
    size_t ulen = 0;
    const char *utext = vfs_read_ptr(PRISM_USERS_FILE, &ulen);
    if (utext == NULL) {
        return;
    }
    size_t i = 0;
    while (i < ulen) {
        size_t j = i;
        while (j < ulen && utext[j] != '\n') {
            j++;
        }
        if (j - i > 5 && memcmp(utext + i, "user ", 5) == 0) {
            char name[PRISM_USER_NAME_MAX + 1];
            size_t n = 0;
            size_t k = i + 5;
            while (k < j && utext[k] != ' ' &&
                   n < PRISM_USER_NAME_MAX) {
                name[n++] = utext[k++];
            }
            name[n] = '\0';
            arm_uart_write_line(name);
        }
        i = j + 1;
    }
}

static void arm_disk_command(const char *line) {
    if (strcmp(line, "disk") == 0) {
        if (!arm_disk_ready) {
            arm_uart_write_line("disk: no virtio block device");
            return;
        }
        arm_uart_write("disk: virtio block device, ");
        uart_write_u64(arm_blk_sectors() / 2048);
        arm_uart_write(" MB, ");
        arm_uart_write_line(
            diskfs_is_formatted(&arm_disk) == 1
                ? "formatted (PRISMFS)"
                : "not formatted (dsync formats it)");
    } else if (strcmp(line, "dformat") == 0) {
        if (!arm_disk_ready) {
            arm_uart_write_line("dformat: no disk");
        } else if (diskfs_format(&arm_disk) == 0) {
            arm_uart_write_line("dformat: disk formatted, all files gone");
        } else {
            arm_uart_write_line("dformat: failed");
        }
    } else if (strcmp(line, "pinset") == 0) {
        arm_pinset();
    } else if (strcmp(line, "totp setup") == 0) {
        arm_totp_setup();
    } else if (strcmp(line, "totp off") == 0) {
        arm_totp_off();
    } else if (strncmp(line, "useradd ", 8) == 0) {
        arm_useradd(line + 8);
    } else if (strcmp(line, "userlist") == 0) {
        arm_userlist();
    } else if (strcmp(line, "dsync") == 0) {
        if (arm_guest) {
            arm_uart_write_line(
                "dsync: guest session is not saved");
            return;
        }
        int w = arm_disk_sync();
        if (w == -2) {
            arm_uart_write_line(
                "dsync: locked (log in at boot first)");
        } else if (w < 0) {
            arm_uart_write_line("dsync: failed (no disk?)");
        } else {
            arm_uart_write("dsync: ");
            uart_write_u64((uint64_t)w);
            arm_uart_write_line(" file(s) written to disk");
        }
    } else if (strcmp(line, "dload") == 0) {
        int c = arm_disk_load();
        if (c < 0) {
            arm_uart_write_line("dload: failed (not formatted?)");
        } else {
            arm_uart_write("dload: ");
            uart_write_u64((uint64_t)c);
            arm_uart_write_line(" file(s) loaded from disk");
        }
    } else if (strcmp(line, "dls") == 0) {
        int count = 0, i;
        if (!arm_disk_ready ||
            diskfs_list(&arm_disk, arm_entries, &count) != 0) {
            arm_uart_write_line("dls: failed (not formatted?)");
            return;
        }
        for (i = 0; i < count; i++) {
            if (arm_entries[i].used) {
                arm_uart_write(arm_entries[i].name);
                arm_uart_write("  ");
                uart_write_u64(arm_entries[i].size);
                arm_uart_write_line(" bytes");
            }
        }
        arm_uart_write("dls: ");
        uart_write_u64((uint64_t)count);
        arm_uart_write_line(" file(s) on disk");
    }
}

static int arm_is_disk_command(const char *line) {
    return strcmp(line, "disk") == 0 || strcmp(line, "dformat") == 0 ||
           strcmp(line, "dsync") == 0 || strcmp(line, "dload") == 0 ||
           strcmp(line, "dls") == 0 || strcmp(line, "pinset") == 0 ||
           strcmp(line, "totp setup") == 0 ||
           strcmp(line, "totp off") == 0 ||
           strncmp(line, "useradd ", 8) == 0 ||
           strcmp(line, "userlist") == 0;
}

/* ---- ネットワーク (virtio-net + netstack) ---- */

static void uart_write_ip(const uint8_t *ip) {
    int i;
    for (i = 0; i < 4; i++) {
        if (i > 0) {
            arm_uart_putc('.');
        }
        uart_write_u64(ip[i]);
    }
}

/* `net`: スタックを初期化して自分のアドレスを表示する。
 * `fetch <host> [path]`: 平文 HTTP で 1 ページ取る。 */
static void arm_net_command(const char *line) {
    if (strcmp(line, "net") == 0) {
        uint8_t ip[4];
        if (net_ensure() != 0) {
            arm_uart_write_line("net: setup failed (no card?)");
            return;
        }
        net_my_ip(ip);
        arm_uart_write("net: my ip is ");
        uart_write_ip(ip);
        arm_uart_write_line("");
        return;
    }
    /* fetch */
    {
        const char *p = line + 5;
        char host[64];
        char path[128];
        size_t hi = 0, pi = 0;
        static char body[16385];
        size_t got = 0;
        int code = 0;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            arm_uart_write_line("Usage: fetch <host> [path]");
            return;
        }
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               hi + 1 < sizeof(host)) {
            host[hi++] = *p++;
        }
        host[hi] = '\0';
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            path[pi++] = '/';
        } else {
            while (*p != '\0' && pi + 1 < sizeof(path)) {
                path[pi++] = *p++;
            }
        }
        path[pi] = '\0';
        arm_uart_write("Fetching ");
        arm_uart_write(host);
        arm_uart_write(path);
        arm_uart_write_line(" ...");
        if (net_fetch_http(host, path, body, sizeof(body), &got,
                           &code) != 0) {
            arm_uart_write("fetch failed (HTTP ");
            uart_write_u64((uint64_t)(code < 0 ? 0 : code));
            arm_uart_write_line(")");
            return;
        }
        body[got < sizeof(body) ? got : sizeof(body) - 1] = '\0';
        arm_uart_write(body);
        if (got > 0 && body[got - 1] != '\n') {
            arm_uart_write_line("");
        }
    }
}

static int arm_is_net_command(const char *line) {
    return strcmp(line, "net") == 0 ||
           (strncmp(line, "fetch", 5) == 0 &&
            (line[5] == '\0' || line[5] == ' ' || line[5] == '\t'));
}

void arm_main(uint64_t dtb_addr) {
    prism_shell_state state;
    static char line[PRISM_SHELL_LINE_CAP];
    static char out[4096];
    uint64_t pmm_base;
    uint64_t ram_end;
    int dtb_ok;
    prism_memmap_entry entries[2];
    uint64_t deadline;
    size_t n;

    arm_uart_init();
    arm_uart_write_line("PrismOS 27H1 (ARM64)");
    arm_uart_write_line("A customizable OS for everyone.");
    {
        uint64_t el;
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
        arm_uart_write("[boot] EL=");
        uart_write_u64((el >> 2) & 0x3);
        arm_uart_write_line("");
    }

    /* RAM の末尾は DTB の /memory ノードから読む。MMU の写像範囲を
     * 決めるために、MMU を有効にする前にここで読む。読めない・
     * おかしい場合は 256MB 構成とみなす (QEMU virt の既定)。
     * 上限は物理アドレス 36 ビット分 (64GB)。 */
    pmm_base = ((uint64_t)__kernel_end + PMM_PAGE_SIZE - 1) &
               ~(PMM_PAGE_SIZE - 1);
    ram_end = ARM_RAM_FALLBACK_END;
    dtb_ok = 0;
    {
        uint64_t mbase = 0, msize = 0;
        if (arm_fdt_memory(dtb_addr, &mbase, &msize) == 0 &&
            mbase <= pmm_base && msize > 0 &&
            mbase + msize > pmm_base && mbase + msize <= ARM_RAM_MAX_END) {
            ram_end = mbase + msize;
            dtb_ok = 1;
        }
    }

    arm_mmu_init(ram_end);
    arm_uart_write("[boot] mmu: identity map on, RAM mapped to ");
    uart_write_hex64(ram_end);
    arm_uart_write_line("");

    arm_gic_init();
    arm_timer_init();
    /* ベクタ表を指してから IRQ だけ許可する。 */
    __asm__ volatile("msr vbar_el1, %0" : : "r"((uint64_t)arm_vectors));
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /* 50 刻み (500ms) 待つ。割り込みが本当に来ることの証明。
     * 来なければ 3 秒で諦めてシェルへ進む (正直に報告する)。 */
    deadline = arm_timer_now() + ARM_TIMER_WAIT_SECS * arm_timer_freq_hz();
    while (arm_timer_ticks() < ARM_TIMER_WAIT_TICKS) {
        if (arm_timer_now() >= deadline) {
            break;
        }
        __asm__ volatile("wfi");
    }
    if (arm_timer_ticks() >= ARM_TIMER_WAIT_TICKS) {
        arm_uart_write("[boot] gic+timer ok: ");
        uart_write_u64(arm_timer_ticks());
        arm_uart_write_line(" ticks");
    } else {
        arm_uart_write("[boot] gic+timer TIMEOUT: ticks=");
        uart_write_u64(arm_timer_ticks());
        arm_uart_write_line("");
    }

    /* PMM: カーネル直後から、DTB から読んだ RAM の末尾まで
     * (末尾の決定は MMU 初期化の前に済ませてある)。 */
    arm_uart_write("[boot] memory: RAM ends at ");
    uart_write_hex64(ram_end);
    arm_uart_write(", ");
    uart_write_u64((ram_end - 0x40000000ULL) / (1024 * 1024));
    arm_uart_write_line(" MB from DTB or fallback");
    entries[0].base = pmm_base;
    entries[0].length = ram_end - pmm_base;
    entries[0].type = PMM_TYPE_USABLE;
    {
        int entry_count = 1;
        /* DTB が PMM の範囲内にあれば、その領域は起動側の
         * データなので PMM に渡さない (範囲を前後に分ける)。 */
        if (dtb_ok) {
            uint64_t dsz = arm_fdt_total_size(dtb_addr);
            uint64_t ds = dtb_addr & ~(PMM_PAGE_SIZE - 1);
            uint64_t de = (dtb_addr + dsz + PMM_PAGE_SIZE - 1) &
                          ~(PMM_PAGE_SIZE - 1);
            if (dsz > 0 && ds > pmm_base && de < ram_end) {
                entries[0].length = ds - pmm_base;
                entries[1].base = de;
                entries[1].length = ram_end - de;
                entries[1].type = PMM_TYPE_USABLE;
                entry_count = 2;
            }
        }
        pmm_init(entries, entry_count);
    }
    heap_init(0);

    arm_draw_desktop();
    if (fb_ready()) {
        con_init();
    }
    arm_virtio_input_init();

    vfs_set_allocator(kmalloc, kfree);
    prism_shell_init(&state);

    /* ディスク: virtio-blk があれば diskfs をつなぎ、
     * フォーマット済みならファイルを全部読み込む。
     * アカウント (.account) がある場合は暗号文のまま
     * 読み込み、PIN を聞いてから復号する (x86 版の
     * persist と同じ流れ)。アカウントが無ければ従来
     * どおり平文で読み込む。 */
    if (arm_blk_init() == 0) {
        arm_disk.ctx = NULL;
        arm_disk.read_sector = arm_disk_read_sector;
        arm_disk.write_sector = arm_disk_write_sector;
        arm_disk.total_sectors = (uint32_t)arm_blk_sectors();
        arm_disk.base_lba = 0;
        arm_disk.crypto = NULL;
        arm_disk_ready = 1;
        if (diskfs_is_formatted(&arm_disk) == 1) {
            int loaded = arm_disk_load_raw();
            arm_uart_write("[boot] disk: ");
            uart_write_u64(loaded > 0 ? (uint64_t)loaded : 0);
            arm_uart_write_line(" file(s) loaded from virtio disk");
            size_t alen = 0;
            const char *acct =
                vfs_read_ptr(PRISM_ACCOUNT_FILE, &alen);
            if (acct != NULL) {
                arm_boot_login(acct, alen);
            } else {
                arm_uart_write_line(
                    "[boot] disk: no account yet (run `pinset` "
                    "to enable login + encryption)");
            }
        } else {
            arm_uart_write_line(
                "[boot] disk: virtio disk present, not formatted");
        }
    } else {
        arm_uart_write_line("[boot] disk: no virtio block device");
    }

    /* ネットワーク: HHDM は無い (恒等写像) ので 0 を渡して
     * net_ensure が使えるようにしておく。実初期化は最初の
     * `net` / `fetch` コマンドのときに netstack が行う。 */
    net_set_hhdm(0);

    arm_uart_write_line("Type `help` to see available commands.");

    for (;;) {
        arm_uart_write("prismos> ");
        n = arm_read_line_merged(line, sizeof(line));
        if (n == 0) {
            continue;
        }
        if (strcmp(line, "mem") == 0) {
            cmd_mem();
            continue;
        }
        if (strcmp(line, "ticks") == 0) {
            cmd_ticks();
            continue;
        }
        if (arm_is_disk_command(line)) {
            arm_disk_command(line);
            continue;
        }
        if (arm_is_net_command(line)) {
            arm_net_command(line);
            continue;
        }
        if (strcmp(line, "help") == 0) {
            prism_shell_execute(&state, line, out, sizeof(out));
            arm_uart_write(out);
            arm_uart_write_line(
                "ARM disk commands: disk, dformat, dsync, dload, "
                "dls, pinset, useradd <name>, userlist");
            arm_uart_write_line(
                "ARM network commands: net, fetch <host> [path]");
            continue;
        }
        prism_shell_execute(&state, line, out, sizeof(out));
        arm_uart_write(out);
        if (state.should_reboot) {
            psci_reset();
            arm_uart_write_line("reboot: PSCI reset failed, halting.");
            for (;;) {
                __asm__ volatile("wfi");
            }
        }
        if (state.should_clear) {
            state.should_clear = 0;
            arm_uart_write_line("--- cleared ---");
        }
    }
}
