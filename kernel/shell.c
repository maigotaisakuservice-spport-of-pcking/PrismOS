/* PrismOS — シェル (カーネル側ラッパー)
 *
 * コマンドの解析と実行そのものは shell/core.c (ホストでも
 * テストできる純粋なロジック) にあり、ここは入出力と
 * reboot / clear の副作用だけを担当する。
 *
 * v0.2 フェーズ1: 行入力はシリアルと PS/2 キーボードの両方から
 * 受け付ける。どちらで打っても同じシェルが動き、打った文字は
 * シリアルにエコーする (画面出力はまだ無いため)。
 *
 * v0.2 フェーズ3: 打った文字とコマンドの出力は画面のターミナル
 * にも出す。F1〜F3 や矢印キーなどの特別なキーはデスクトップが
 * 先に受け取り、アプリの切り替えや操作に使う。
 *
 * v0.2 フェーズ4: 起動時にディスクからファイルを読み込む。
 * アカウントがある場合はログイン (PIN) を通してから
 * デスクトップに入る。`install yes` でディスクに書き込み、
 * `passwd` でログイン用 PIN を設定する。ファイルが変わる
 * たびにディスクへ書き戻す。ログインで確かめた PIN から鍵を
 * 作り、".account" 以外のファイルは AES-256-CTR で暗号化して
 * ディスクに書く (鍵の作り方は kernel/crypto.h を見ること)。
 */
#include "shell.h"
#include "account.h"
#include "bio.h"
#include "drvload.h"
#include "block.h"
#include "compat_run.h"
#include "crypto.h"
#include "desktop.h"
#include "fb.h"
#include "entropy_hw.h"
#include "fat16.h"
#include "heap.h"
#include "io.h"
#include "keyboard.h"
#include "../shell/mime.h"
#include "mailproto.h"
#include "net.h"
#include "netstack.h"
#include "persist.h"
#include "pmm.h"
#include "recovery.h"
#include "rtc.h"
#include "totp.h"
#include "users.h"
#include "../shell/storepkg.h"
#include "store.h"
#include "sb16.h"
#include "security.h"
#include "serial.h"
#include "hda.h"
#include "vinput.h"
#include "mouse.h"
#include "vbe.h"
#include "smp.h"
#include "acpi.h"
#include "settings.h"
#include "speaker.h"
#include "string.h"
#include "ehci.h"
#include "xhci.h"
#include "uvc.h"
#include "uhci.h"
#include "update.h"
#include "version.h"
#include "wav.h"
#include "../shell/core.h"
#include "../shell/compat.h"
#include "../shell/mail.h"
#include "../shell/mldsa.h"
#include "../shell/mlkem.h"
#include "../shell/vfs.h"

#define LINE_CAP 512
#define OUT_CAP 8192
#define PIN_CAP 17

/* 起動時に settings.txt から読む設定 (ログインや security
 * コマンドが使う)。デスクトップは別に持つ。 */
static prism_settings shell_settings;

/* Defined further down (used by the mail command above them). */
static int starts_with_word(const char *line, const char *word);
static const char *skip_spaces(const char *p);
static void load_settings(prism_settings *s);
static void save_settings(const prism_settings *s);

/* `mem` コマンド用: 行が "mem" だけ、または "mem " で始まるか。 */
static int is_mem_command(const char *line) {
    if (strncmp(line, "mem", 3) != 0) {
        return 0;
    }
    return line[3] == '\0' || line[3] == ' ' || line[3] == '\t';
}

static int line_eq(const char *line, const char *want) {
    return strcmp(line, want) == 0;
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

/* 16 ビット値を 4 桁の 16 進数でシリアルに出す。 */
static void print_hex16_serial(uint16_t value) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 12; i >= 0; i -= 4) {
        serial_write_char(digits[(value >> i) & 0xF]);
    }
}

/* PMM とヒープの状態をシリアルに出す。 */
static void print_memory_info(void) {
    uint64_t total = pmm_total_pages();
    uint64_t free_pages = pmm_free_pages_count();
    uint64_t used = pmm_used_pages_count();
    serial_write("memory: ");
    print_u64_serial(total);
    serial_write(" pages total (");
    print_u64_serial(total * 4096 / (1024 * 1024));
    serial_write(" MB), ");
    print_u64_serial(free_pages);
    serial_write(" free, ");
    print_u64_serial(used);
    serial_write_line(" used");
    if (heap_is_ready()) {
        serial_write("heap: ");
        print_u64_serial(heap_total_bytes());
        serial_write(" bytes total, ");
        print_u64_serial(heap_free_bytes());
        serial_write(" free, ");
        print_u64_serial(heap_used_bytes());
        serial_write_line(" used");
    } else {
        serial_write_line("heap: (not ready)");
    }
}

/* シリアルとキーボードのどちらからでも 1 行読む。
 * 入力が無い間は pause しながらポーリングする (hlt は使わない。
 * シリアルは割り込みでは来ないため、hlt するとシリアル入力で
 * 起きられなくなる)。 */
/* シェルの履歴: 直近 16 件のリスト。上キーで古い方へ、
 * 下キーで新しい方へ移動する (ターミナル画面のときだけ)。
 * `history` コマンドで一覧できる。 */
#define HIST_MAX 16
static char hist_list[HIST_MAX][512];
static int hist_count;
/* 編集中の移動位置: -1 = 移動していない、0 = 最新、
 * 1 = その1つ前、… */
static int hist_nav = -1;

static void hist_remember(const char *line) {
    if (line[0] == '\0') {
        return;
    }
    if (hist_count > 0) { /* 直前と同じなら追加しない */
        const char *last = hist_list[hist_count - 1];
        size_t i = 0;
        int same = 1;
        while (line[i] != '\0' || last[i] != '\0') {
            if (line[i] != last[i]) {
                same = 0;
                break;
            }
            i++;
        }
        if (same) {
            hist_nav = -1;
            return;
        }
    }
    if (hist_count == HIST_MAX) { /* 古いものから捨てる */
        for (int i = 1; i < HIST_MAX; i++) {
            for (size_t j = 0; j < sizeof(hist_list[0]); j++) {
                hist_list[i - 1][j] = hist_list[i][j];
            }
        }
        hist_count = HIST_MAX - 1;
    }
    size_t i = 0;
    while (line[i] != '\0' && i + 1 < sizeof(hist_list[0])) {
        hist_list[hist_count][i] = line[i];
        i++;
    }
    hist_list[hist_count][i] = '\0';
    hist_count++;
    hist_nav = -1;
}

/* 履歴の項目を現在の行に読み込む (画面の消去と再表示)。 */
static void hist_load(char *buf, size_t *len_io, size_t cap,
                      const char *entry) {
    size_t len = *len_io;
    while (len > 0) {
        len--;
        serial_write_char('\b');
        serial_write_char(' ');
        serial_write_char('\b');
        desktop_console_putc('\b');
    }
    size_t i = 0;
    while (entry[i] != '\0' && len + 1 < cap) {
        buf[len] = entry[i];
        serial_write_char(entry[i]);
        desktop_console_putc(entry[i]);
        len++;
        i++;
    }
    *len_io = len;
    desktop_refresh_console();
}

/* ユーザー切替の要求 (デスクトップの電源メニューから)。
 * メインループと行入力の待機部の両方で見て、ログインへ戻る。 */
static int switch_user_flag;

void shell_request_user_switch(void) {
    switch_user_flag = 1;
}

/* 設定アプリで切り替えた言語の保留値 (-1 = 変更なし)。
 * 設定の保存経路 (デスクトップ側) から通知され、メイン
 * ループがコマンド実行前に state へ反映する。 */
static int pending_language = -1;

void shell_notify_language_changed(int language) {
    pending_language = language ? 1 : 0;
}

static size_t read_line_merged(char *buf, size_t cap) {
    size_t len = 0;
    for (;;) {
        int c = keyboard_get_char();
        int from_kbd = 1;
        if (c < 0) {
            c = serial_try_read_char();
            from_kbd = 0;
        }
        if (c < 0) {
            /* 画面キーボードの注入キー。物理キーと同じ
             * 経路 (デスクトップ先行・行入力) を通す。 */
            c = desktop_osk_poll_key();
            from_kbd = 1;
        }
        if (c < 0) {
            if (switch_user_flag) {
                /* ユーザー切替: 空行として戻り、メインループの
                 * 先頭でログインへ分岐する。 */
                buf[0] = '\0';
                return 0;
            }
            desktop_tick();
            desktop_mouse_poll();
            fb_flush();
            __asm__ volatile("pause");
            continue;
        }
        /* すべてのキーはデスクトップが先に受け取る。Files や
         * Settings の画面が出ている間は、普通のキーも含めて
         * そちらが消費する。ターミナル画面のときだけ、普通の
         * キーが行入力に回ってくる。
         * ただしシリアルから来た 0x80 以上のバイトは特殊キー
         * ではなく UTF-8 のテキストとして扱う。特殊キー
         * コード (F4=0x84、Ctrl+D=0xA5 等) と UTF-8 の継続
         * バイトが同じ値域にあり、区別できないため。実キー
         * (PS/2・USB HID) の特殊コードは今まで通り効く。 */
        if ((from_kbd || c < 0x80) && desktop_handle_key(c)) {
            fb_flush();
            continue;
        }
        /* 上/下キー: 履歴リストを移動する。デスクトップが
         * 消費しなかったときだけ処理する (Files 等の画面で
         * 矢印を押しても、裏のシェル行に履歴が入り込ま
         * ないようにするため)。 */
        if (from_kbd && c == 0x90 && hist_count > 0) {
            if (hist_nav + 1 < hist_count) {
                hist_nav++;
                hist_load(buf, &len, cap,
                          hist_list[hist_count - 1 - hist_nav]);
            }
            continue;
        }
        if (from_kbd && c == 0x91 && hist_nav >= 0) {
            hist_nav--;
            if (hist_nav < 0) {
                hist_load(buf, &len, cap, "");
            } else {
                hist_load(buf, &len, cap,
                          hist_list[hist_count - 1 - hist_nav]);
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            serial_write_char('\n');
            desktop_console_putc('\n');
            desktop_refresh_console();
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                serial_write_char('\b');
                serial_write_char(' ');
                serial_write_char('\b');
                desktop_console_putc('\b');
                desktop_refresh_console();
            }
            continue;
        }
        if (len + 1 < cap) {
            buf[len++] = (char)c;
            serial_write_char((char)c); /* エコー */
            desktop_console_putc(c);
            desktop_refresh_console();
        }
    }
    buf[len] = '\0';
    return len;
}

/* PIN を読む。打った文字は '*' で表示する (平文エコーしない)。
 * デスクトップは通さない (ログイン画面と設定画面のため)。 */
static size_t read_secret(char *buf, size_t cap) {
    size_t len = 0;
    for (;;) {
        int c = keyboard_get_char();
        if (c < 0) {
            c = serial_try_read_char();
        }
        if (c < 0) {
            __asm__ volatile("pause");
            continue;
        }
        if (c >= 0x80) {
            continue; /* 特別なキー (F1 など) は無視する */
        }
        if (c == '\r' || c == '\n') {
            serial_write_char('\n');
            if (desktop_ready()) {
                desktop_console_putc('\n');
                desktop_refresh_console();
            }
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                serial_write_char('\b');
                serial_write_char(' ');
                serial_write_char('\b');
                if (desktop_ready()) {
                    desktop_console_putc('\b');
                    desktop_refresh_console();
                }
            }
            continue;
        }
        if (c < 0x21 || c > 0x7E) {
            continue;
        }
        if (len + 1 < cap) {
            buf[len++] = (char)c;
            serial_write_char('*');
            if (desktop_ready()) {
                desktop_console_putc('*');
                desktop_refresh_console();
            }
        }
    }
    buf[len] = '\0';
    return len;
}

/* 指定した秒数だけ待つ。RTC (1 秒単位) をポーリングする。
 * hlt は使わない (PIT の割り込みはマスクされており、hlt すると
 * キーボード以外の割り込みで起きられないため)。 */
static void wait_seconds(uint32_t seconds) {
    prism_rtc_time t0;
    if (rtc_read(&t0) != 0 || seconds == 0) {
        return;
    }
    uint32_t start = rtc_epoch_seconds(&t0);
    for (;;) {
        prism_rtc_time t;
        if (rtc_read(&t) != 0) {
            return;
        }
        if (rtc_epoch_seconds(&t) - start >= seconds) {
            return;
        }
        __asm__ volatile("pause");
    }
}

/* 管理者 PIN の確認。アカウントが無ければ「先に passwd」、
 * 間違っていればその旨を出して 0 を返す。成功したら 1。 */
static int admin_check(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
    if (text == NULL) {
        serial_write_line("No PIN is set. Run `passwd` first.");
        return 0;
    }
    char pin[PIN_CAP];
    serial_write("Admin PIN: ");
    read_secret(pin, sizeof(pin));
    int rc = account_verify(pin, text, len);
    for (size_t i = 0; pin[i] != '\0'; i++) {
        pin[i] = 0;
    }
    if (rc != 1) {
        serial_write_line("Wrong PIN.");
        audit_log_event("security-fail", "admin PIN rejected");
        return 0;
    }
    return 1;
}

/* ログイン画面。アカウントが無ければすぐに戻る (0)。
 * 通れば 1 を返し、通った PIN を pin_out に入れる (鍵を
 * 作るときに使う。呼び出し側で使い終わったら消すこと)。
 * 連続で間違えたらロックする: lockout_seconds が 0 なら
 * 許容回数を超えた時点で止まる (従来の動作)。0 より
 * 大きければその秒数だけ待ってから次の回に入る。
 * 3 回のロックを繰り返しても通らなければ止まる。 */

/* ---- 複数ユーザー / ゲスト / TOTP 2 要素認証 ----
 * 主アカウント (.account = "admin") は従来どおり。
 * users.txt に追加ユーザー (PIN + 主鍵の包み)、
 * ゲストは保存されない一時セッション、TOTP は
 * RFC 6238 の 6 桁コード (秘密は users.txt の
 * totp 行、Base32)。 */

static void make_salt(uint8_t *salt);


/* ---- ping / ファイアウォール (fw) ---- */

static int parse_ipv4_str(const char *s, uint8_t *ip) {
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
            digits++;
            if (v > 255 || digits > 3) {
                return -1;
            }
        }
        ip[part] = (uint8_t)v;
        if (part < 3) {
            if (*s != '.') {
                return -1;
            }
            s++;
        }
    }
    return *s == '\0' ? 0 : -1;
}

static void print_ip_serial(const uint8_t *ip) {
    for (int i = 0; i < 4; i++) {
        if (i > 0) {
            serial_write(".");
        }
        print_u64_serial(ip[i]);
    }
}

/* fw.txt (1 行 1 IP) から遮断ルールを読み直す。 */
static void fw_load(void) {
    net_fw_clear();
    size_t len = 0;
    const char *text = vfs_read_ptr("fw.txt", &len);
    if (text == NULL) {
        return;
    }
    size_t off = 0;
    while (off < len) {
        char line[24];
        size_t n = 0;
        while (off < len && text[off] != '\n') {
            if (n + 1 < sizeof(line)) {
                line[n++] = text[off];
            }
            off++;
        }
        if (off < len) {
            off++;
        }
        if (n > 0 && line[n - 1] == '\r') {
            n--;
        }
        line[n] = '\0';
        uint8_t ip[4];
        if (n > 0 && parse_ipv4_str(line, ip) == 0) {
            net_fw_add_drop(ip);
        }
    }
}

static void fw_save(void) {
    char buf[512];
    size_t p = 0;
    for (int i = 0; i < net_fw_count(); i++) {
        uint8_t ip[4];
        if (net_fw_get(i, ip) != 0) {
            continue;
        }
        for (int part = 0; part < 4; part++) {
            if (part > 0 && p + 1 < sizeof(buf)) {
                buf[p++] = '.';
            }
            int v = ip[part];
            char rev[4];
            int ri = 0;
            if (v == 0) {
                rev[ri++] = '0';
            }
            while (v > 0) {
                rev[ri++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (ri > 0 && p + 1 < sizeof(buf)) {
                buf[p++] = rev[--ri];
            }
        }
        if (p + 1 < sizeof(buf)) {
            buf[p++] = '\n';
        }
    }
    buf[p] = '\0';
    if (vfs_write("fw.txt", buf, p) == 0) {
        persist_maybe_sync();
    }
}

void shell_fw_save(void) {
    fw_save();
}

/* ---- 簡易 HTTP サーバ (serve コマンド) ----
 * net_tcp_listen で接続を受け、リクエスト行を読んで
 * 固定のページを返す。3 接続まで捌いたら戻る
 * (連続接続の受け付けとサーバソケットの実証)。 */
static void serve_flow(char *line) {
    const char *p = skip_spaces(line + 5);
    int port = 0;
    while (*p >= '0' && *p <= '9') {
        port = port * 10 + (*p - '0');
        p++;
    }
    if (port <= 0 || port > 65535) {
        serial_write_line("Usage: serve <port>");
        return;
    }
    if (net_ensure() != 0) {
        serial_write_line("serve: network is not ready.");
        return;
    }
    static net_tcp_stream s;
    serial_write("serve: listening on port ");
    print_u64_serial((uint64_t)port);
    serial_write_line("");
    for (int conn = 0; conn < 3; conn++) {
        if (net_tcp_listen((uint16_t)port, 120, &s) != 0) {
            serial_write_line("serve: no connection (timeout).");
            return;
        }
        serial_write_line("serve: connection accepted");
        /* リクエストを読む (最大 4KB、5 秒まで)。 */
        static char req[4096];
        size_t rlen = 0;
        while (rlen + 1 < sizeof(req)) {
            uint8_t buf[512];
            int n = net_tcp_recv(&s, buf, sizeof(buf), 5);
            if (n <= 0) {
                break;
            }
            for (int i = 0; i < n && rlen + 1 < sizeof(req); i++) {
                req[rlen++] = (char)buf[i];
            }
            if (rlen >= 4 && req[rlen - 4] == '\r' &&
                req[rlen - 3] == '\n' && req[rlen - 2] == '\r' &&
                req[rlen - 1] == '\n') {
                break;
            }
        }
        req[rlen] = '\0';
        /* リクエスト行からパスを取り出す。 */
        char path[96];
        size_t pi = 0;
        path[0] = '\0';
        if (rlen > 4 && memcmp(req, "GET ", 4) == 0) {
            size_t i = 4;
            while (i < rlen && req[i] != ' ' &&
                   pi + 1 < sizeof(path)) {
                path[pi++] = req[i++];
            }
            path[pi] = '\0';
        }
        serial_write("serve: request ");
        serial_write_line(path[0] != '\0' ? path : "(none)");
        static char resp[1024];
        size_t rp = 0;
        const char *parts1 =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
            "Connection: close\r\n\r\nHello from PrismOS!\n"
            "You asked for ";
        while (*parts1 != '\0' && rp + 1 < sizeof(resp)) {
            resp[rp++] = *parts1++;
        }
        const char *pp = path;
        while (*pp != '\0' && rp + 1 < sizeof(resp)) {
            resp[rp++] = *pp++;
        }
        if (rp + 1 < sizeof(resp)) {
            resp[rp++] = '\n';
        }
        resp[rp] = '\0';
        (void)net_tcp_send(&s, resp, rp);
        net_tcp_close(&s);
        serial_write_line("serve: responded");
    }
    serial_write_line("serve: done");
}

/* ---- 同時 2 接続サーバ (serve2 コマンド) ----
 * 2 本の接続を同時に開いたまま捌く。1 本目を受け付けた
 * まま 2 本目を受け付け、両方のリクエスト行を読んで
 * から、2 本目→1 本目の順に応答して閉じる。受信の
 * 振り分け (netstack の接続台帳) があるため、1 本目の
 * リクエストが 2 本目の待ち受け中に届いても捨てられ
 * ずにバッファへ入り、再送に頼らず読み出せる。 */
static void serve2_flow(char *line) {
    const char *p = skip_spaces(line + 6);
    int port = 0;
    while (*p >= '0' && *p <= '9') {
        port = port * 10 + (*p - '0');
        p++;
    }
    if (port <= 0 || port > 65535) {
        serial_write_line("Usage: serve2 <port>");
        return;
    }
    if (net_ensure() != 0) {
        serial_write_line("serve2: network is not ready.");
        return;
    }
    static net_tcp_stream a, b;
    serial_write("serve2: listening on port ");
    print_u64_serial((uint64_t)port);
    serial_write_line("");
    if (net_tcp_listen((uint16_t)port, 120, &a) != 0) {
        serial_write_line("serve2: no first connection (timeout).");
        return;
    }
    serial_write_line("serve2: first connection accepted");
    if (net_tcp_listen((uint16_t)port, 120, &b) != 0) {
        serial_write_line("serve2: no second connection (timeout).");
        net_tcp_close(&a);
        return;
    }
    serial_write_line(
        "serve2: second connection accepted (first still open)");
    static char line_a[256], line_b[256];
    int na = net_tcp_recv_line(&a, line_a, sizeof(line_a), 30);
    int nb = net_tcp_recv_line(&b, line_b, sizeof(line_b), 30);
    serial_write("serve2: request A: ");
    serial_write_line(na >= 0 ? line_a : "(none)");
    serial_write("serve2: request B: ");
    serial_write_line(nb >= 0 ? line_b : "(none)");
    /* 応答は 2 本目から先に返す (独立に捌けることの確認)。 */
    const char *resp_b =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
        "Connection: close\r\n\r\nserve2 second\n";
    const char *resp_a =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
        "Connection: close\r\n\r\nserve2 first\n";
    (void)net_tcp_send(&b, resp_b, strlen(resp_b));
    net_tcp_close(&b);
    serial_write_line("serve2: responded to second");
    (void)net_tcp_send(&a, resp_a, strlen(resp_a));
    net_tcp_close(&a);
    serial_write_line("serve2: responded to first");
    serial_write_line("serve2: done");
}


/* ---- HTTP プロキシ設定 (proxy コマンド) ---- */
static void proxy_flow(char *line) {
    const char *arg = skip_spaces(line + 5);
    if (starts_with_word(arg, "set")) {
        const char *p = skip_spaces(arg + 3);
        char conf[96];
        size_t q = 0;
        while (p[q] != '\0' && q + 1 < sizeof(conf)) {
            conf[q] = p[q];
            q++;
        }
        conf[q] = '\0';
        if (q == 0) {
            serial_write_line("Usage: proxy set <host> <port>");
            return;
        }
        if (vfs_write("proxy.conf", conf, q) == 0) {
            persist_maybe_sync();
            serial_write_line("proxy: set (plain HTTP only).");
        } else {
            serial_write_line("proxy: could not save proxy.conf.");
        }
        return;
    }
    if (line_eq(arg, "off")) {
        (void)vfs_delete("proxy.conf");
        persist_maybe_sync();
        serial_write_line("proxy: off.");
        return;
    }
    size_t len = 0;
    const char *text = vfs_read_ptr("proxy.conf", &len);
    if (text == NULL || len == 0) {
        serial_write_line("proxy: (not set)");
    } else {
        char buf[128];
        if (len + 1 > sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        memcpy(buf, text, len);
        buf[len] = '\0';
        serial_write("proxy: ");
        serial_write_line(buf);
    }
}

static void ping_flow(char *line) {
    const char *arg = skip_spaces(line + 4);
    char host[64];
    size_t hp = 0;
    while (arg[hp] != '\0' && arg[hp] != ' ' && hp + 1 < sizeof(host)) {
        host[hp] = arg[hp];
        hp++;
    }
    host[hp] = '\0';
    if (host[0] == '\0') {
        serial_write_line("Usage: ping <host or ip>");
        return;
    }
    uint8_t ip[4];
    if (parse_ipv4_str(host, ip) != 0) {
        if (net_resolve(host, ip) != 0) {
            serial_write_line("ping: cannot resolve host.");
            return;
        }
    }
    serial_write("PING ");
    print_ip_serial(ip);
    serial_write_line("");
    int replies = 0;
    for (int seq = 1; seq <= 3; seq++) {
        int rc = net_ping(ip, (uint16_t)seq, 2);
        if (rc == 0) {
            replies++;
            serial_write("reply from ");
            print_ip_serial(ip);
            serial_write(" seq=");
            print_u64_serial((uint64_t)seq);
            serial_write_line("");
        } else if (rc == -2) {
            serial_write_line("ping: blocked by the firewall.");
            return;
        } else {
            serial_write("no reply seq=");
            print_u64_serial((uint64_t)seq);
            serial_write_line("");
        }
    }
    serial_write("ping: ");
    print_u64_serial((uint64_t)replies);
    serial_write_line("/3 replies");
}

/* `dns6 <host>`: resolve an AAAA record over IPv6 UDP.
 * The DNS server is the user-net proxy at <prefix>::3
 * (derived from our SLAAC prefix). Diagnostic, like dnstest. */
static void dns6_flow(char *line) {
    const char *arg = skip_spaces(line + 4);
    char host[64];
    size_t hp = 0;
    while (arg[hp] != '\0' && arg[hp] != ' ' && hp + 1 < sizeof(host)) {
        host[hp] = arg[hp];
        hp++;
    }
    host[hp] = '\0';
    if (host[0] == '\0') {
        serial_write_line("Usage: dns6 <host>");
        return;
    }
    if (net6_configure() != 0 || !net6_have_global_addr()) {
        serial_write_line("dns6: no IPv6 global address.");
        return;
    }
    uint8_t server[16];
    net6_my_global(server);
    for (int i = 8; i < 15; i++) {
        server[i] = 0;
    }
    server[15] = 3;
    /* Build the query: header + qname + AAAA(28) IN(1). */
    static uint8_t query[128];
    size_t qn = 0;
    query[qn++] = 0xD0;
    query[qn++] = 0x06;
    query[qn++] = 0x01; /* RD */
    query[qn++] = 0x00;
    query[qn++] = 0x00;
    query[qn++] = 0x01; /* qdcount */
    query[qn++] = 0x00;
    query[qn++] = 0x00;
    query[qn++] = 0x00;
    query[qn++] = 0x00;
    query[qn++] = 0x00;
    query[qn++] = 0x00;
    const char *p = host;
    while (*p != '\0') {
        const char *dot = p;
        while (*dot != '\0' && *dot != '.') {
            dot++;
        }
        size_t label = (size_t)(dot - p);
        if (label == 0 || label > 63 || qn + label + 6 > sizeof(query)) {
            serial_write_line("dns6: bad host name.");
            return;
        }
        query[qn++] = (uint8_t)label;
        for (size_t i = 0; i < label; i++) {
            query[qn++] = (uint8_t)p[i];
        }
        p = (*dot == '.') ? dot + 1 : dot;
    }
    query[qn++] = 0;
    query[qn++] = 0;
    query[qn++] = 28; /* AAAA */
    query[qn++] = 0;
    query[qn++] = 1; /* IN */
    static uint8_t resp[512];
    int n = net6_udp_exchange(server, 53, 0xC006, query, qn, resp,
                              sizeof(resp), 5);
    if (n < 12) {
        serial_write_line("dns6: no reply.");
        return;
    }
    int ancount = (resp[6] << 8) | resp[7];
    size_t off = 12;
    int qd = (resp[4] << 8) | resp[5];
    for (int i = 0; i < qd; i++) { /* skip questions */
        while (off < (size_t)n && resp[off] != 0) {
            if ((resp[off] & 0xC0) == 0xC0) {
                off++;
                break;
            }
            off += (size_t)resp[off] + 1;
        }
        off += 5; /* terminator/pointer + type + class */
    }
    for (int i = 0; i < ancount; i++) {
        if (off + 12 > (size_t)n) {
            break;
        }
        if ((resp[off] & 0xC0) == 0xC0) {
            off += 2;
        } else {
            while (off < (size_t)n && resp[off] != 0) {
                off += (size_t)resp[off] + 1;
            }
            off += 1;
        }
        if (off + 10 > (size_t)n) {
            break;
        }
        int type = (resp[off] << 8) | resp[off + 1];
        int rdlen = (resp[off + 8] << 8) | resp[off + 9];
        off += 10;
        if (type == 28 && rdlen == 16 && off + 16 <= (size_t)n) {
            char text[48];
            net_ipv6_format_addr(resp + off, text);
            serial_write("dns6: ");
            serial_write(host);
            serial_write(" -> ");
            serial_write_line(text);
            return;
        }
        off += (size_t)rdlen;
    }
    serial_write_line("dns6: no AAAA record in reply.");
}

/* `udp6 <ipv6> <port> <text>`: send one UDP/IPv6 datagram
 * and print the reply as text. Diagnostic (netcat-style). */
static void udp6_flow(char *line) {
    const char *arg = skip_spaces(line + 4);
    char addr_text[48];
    size_t hp = 0;
    while (arg[hp] != '\0' && arg[hp] != ' ' && hp + 1 < sizeof(addr_text)) {
        addr_text[hp] = arg[hp];
        hp++;
    }
    addr_text[hp] = '\0';
    arg = skip_spaces(arg + hp);
    int port = 0;
    while (*arg >= '0' && *arg <= '9') {
        port = port * 10 + (*arg - '0');
        arg++;
    }
    arg = skip_spaces(arg);
    uint8_t addr[16];
    if (addr_text[0] == '\0' || port <= 0 || port > 65535 ||
        *arg == '\0' || net_ipv6_parse_addr(addr_text, addr) != 0) {
        serial_write_line("Usage: udp6 <ipv6> <port> <text>");
        return;
    }
    static uint8_t resp[256];
    int n = net6_udp_exchange(addr, (uint16_t)port, 0xC007,
                              (const uint8_t *)arg,
                              strlen(arg), resp, sizeof(resp) - 1, 5);
    if (n < 0) {
        serial_write_line("udp6: no reply.");
        return;
    }
    resp[n] = '\0';
    serial_write("udp6: ");
    serial_write_line((const char *)resp);
}

/* `ip6`: show the IPv6 addresses (configuring on first use). */
static void ip6_flow(void) {
    (void)net6_configure();
    static char buf[256];
    net6_status(buf, sizeof(buf));
    serial_write_line(buf);
}

/* `ping6 <ipv6>`: three ICMPv6 echo requests. */
static void ping6_flow(char *line) {
    const char *arg = skip_spaces(line + 5);
    char host[64];
    size_t hp = 0;
    while (arg[hp] != '\0' && arg[hp] != ' ' && hp + 1 < sizeof(host)) {
        host[hp] = arg[hp];
        hp++;
    }
    host[hp] = '\0';
    uint8_t addr[16];
    if (host[0] == '\0' || net_ipv6_parse_addr(host, addr) != 0) {
        serial_write_line("Usage: ping6 <ipv6 address>");
        return;
    }
    char text[48];
    net_ipv6_format_addr(addr, text);
    serial_write("PING6 ");
    serial_write_line(text);
    int replies = 0;
    for (int seq = 1; seq <= 3; seq++) {
        int rc = net6_ping6(addr, (uint16_t)seq, 2);
        if (rc == 0) {
            replies++;
            serial_write("reply from ");
            serial_write(text);
            serial_write(" seq=");
            print_u64_serial((uint64_t)seq);
            serial_write_line("");
        } else {
            serial_write("no reply seq=");
            print_u64_serial((uint64_t)seq);
            serial_write_line("");
        }
    }
    serial_write("ping6: ");
    print_u64_serial((uint64_t)replies);
    serial_write_line("/3 replies");
}

/* `tcp6 <ipv6> <port> <text>`: IPv6 で TCP 接続し、text を
 * 1 行送って、相手が閉じるか時間切れまで行単位で表示する。 */
static void tcp6_flow(char *line) {
    const char *arg = skip_spaces(line + 4);
    char host[64];
    size_t hp = 0;
    while (arg[hp] != '\0' && arg[hp] != ' ' && hp + 1 < sizeof(host)) {
        host[hp] = arg[hp];
        hp++;
    }
    host[hp] = '\0';
    arg = skip_spaces(arg + hp);
    int port = 0;
    while (*arg >= '0' && *arg <= '9') {
        port = port * 10 + (*arg - '0');
        arg++;
    }
    arg = skip_spaces(arg);
    uint8_t addr[16];
    if (host[0] == '\0' || port <= 0 || port > 65535 ||
        net_ipv6_parse_addr(host, addr) != 0) {
        serial_write_line("Usage: tcp6 <ipv6 address> <port> [text]");
        return;
    }
    static net_tcp_stream s;
    if (net6_tcp_open(addr, (uint16_t)port, &s) != 0) {
        serial_write_line("tcp6: connect failed");
        return;
    }
    serial_write_line("tcp6: connected");
    if (*arg != '\0') {
        static char msg[512];
        size_t mp = 0;
        while (arg[mp] != '\0' && mp + 3 < sizeof(msg)) {
            msg[mp] = arg[mp];
            mp++;
        }
        msg[mp++] = '\r';
        msg[mp++] = '\n';
        msg[mp] = '\0';
        if (net_tcp_send(&s, msg, mp) != 0) {
            serial_write_line("tcp6: send failed");
            net_tcp_close(&s);
            return;
        }
    }
    static char lbuf[1024];
    for (;;) {
        int n = net_tcp_recv_line(&s, lbuf, sizeof(lbuf), 10);
        if (n < 0) {
            break;
        }
        serial_write("tcp6: got ");
        serial_write_line(lbuf);
    }
    net_tcp_close(&s);
    serial_write_line("tcp6: done");
}

/* `serve6 <port>`: IPv6 で 1 接続受け付け、1 行読んで
 * 応答を返して閉じる。 */
static void serve6_flow(char *line) {
    const char *p = skip_spaces(line + 6);
    int port = 0;
    while (*p >= '0' && *p <= '9') {
        port = port * 10 + (*p - '0');
        p++;
    }
    if (port <= 0 || port > 65535) {
        serial_write_line("Usage: serve6 <port>");
        return;
    }
    static net_tcp_stream s;
    serial_write("serve6: listening on port ");
    print_u64_serial((uint64_t)port);
    serial_write_line("");
    if (net6_tcp_listen((uint16_t)port, 120, &s) != 0) {
        serial_write_line("serve6: no connection (timeout).");
        return;
    }
    serial_write_line("serve6: connection accepted");
    static char lbuf[1024];
    int n = net_tcp_recv_line(&s, lbuf, sizeof(lbuf), 15);
    if (n >= 0) {
        serial_write("serve6: got ");
        serial_write_line(lbuf);
        static char resp[1088];
        size_t rp = 0;
        const char *pre = "pong6: ";
        while (*pre != '\0' && rp + 1 < sizeof(resp)) {
            resp[rp++] = *pre++;
        }
        for (int i = 0; i < n && rp + 1 < sizeof(resp); i++) {
            resp[rp++] = lbuf[i];
        }
        if (rp + 1 < sizeof(resp)) {
            resp[rp++] = '\n';
        }
        resp[rp] = '\0';
        (void)net_tcp_send(&s, resp, rp);
    }
    net_tcp_close(&s);
    serial_write_line("serve6: done");
}

static void fw_flow(char *line) {
    const char *arg = skip_spaces(line + 2);
    if (line_eq(arg, "list")) {
        if (net_fw_count() == 0) {
            serial_write_line("fw: no rules (all traffic allowed)");
            return;
        }
        for (int i = 0; i < net_fw_count(); i++) {
            uint8_t ip[4];
            if (net_fw_get(i, ip) == 0) {
                serial_write("drop ");
                print_ip_serial(ip);
                serial_write_line("");
            }
        }
        return;
    }
    if (starts_with_word(arg, "add")) {
        const char *p = skip_spaces(arg + 3);
        char ips[24];
        size_t n = 0;
        while (p[n] != '\0' && p[n] != ' ' && n + 1 < sizeof(ips)) {
            ips[n] = p[n];
            n++;
        }
        ips[n] = '\0';
        uint8_t ip[4];
        if (parse_ipv4_str(ips, ip) != 0) {
            serial_write_line("fw: bad IP address.");
            return;
        }
        if (net_fw_add_drop(ip) != 0) {
            serial_write_line("fw: rule list is full.");
            return;
        }
        fw_save();
        serial_write("fw: now dropping ");
        print_ip_serial(ip);
        serial_write_line("");
        return;
    }
    if (line_eq(arg, "clear")) {
        net_fw_clear();
        fw_save();
        serial_write_line("fw: all rules cleared.");
        return;
    }
    serial_write_line("Usage: fw list | fw add <ip> | fw clear");
}

static char current_user[PRISM_USER_NAME_MAX + 1] = "admin";
static int guest_session;
/* 0 = 主アカウント/未ログイン、1 = 追加ユーザー
 * (login_mk に主鍵)、2 = ゲスト。 */
static int login_kind;
static uint8_t login_mk[32];

static int users_load(char *buf, size_t cap, size_t *len_out) {
    size_t len = 0;
    const char *text = vfs_read_ptr(PRISM_USERS_FILE, &len);
    if (text == NULL || len == 0) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        if (len_out != NULL) {
            *len_out = 0;
        }
        return 0;
    }
    if (len + 1 > cap) {
        return -1;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    if (len_out != NULL) {
        *len_out = len;
    }
    return 0;
}

static int users_save(const char *text) {
    size_t len = 0;
    while (text[len] != '\0') {
        len++;
    }
    if (vfs_write(PRISM_USERS_FILE, text, len) != 0) {
        return -1;
    }
    persist_maybe_sync();
    return 0;
}

static int current_is_admin(void) {
    if (guest_session) {
        return 0;
    }
    if (strcmp(current_user, "admin") == 0) {
        return 1;
    }
    char text[PRISM_USERS_TEXT_MAX];
    size_t len = 0;
    if (users_load(text, sizeof(text), &len) != 0) {
        return 0;
    }
    char role[16];
    return users_role(text, len, current_user, role, sizeof(role)) ==
               0 &&
           strcmp(role, "admin") == 0;
}

/* ---- 設定アプリ (GUI) との共用窓口 (shell.h) ---- */

const char *shell_current_user_name(void) {
    return current_user;
}

int shell_session_is_guest(void) {
    return guest_session;
}

int shell_current_user_is_admin(void) {
    return current_is_admin();
}

int shell_admin_pin_verify(const char *pin) {
    size_t len = 0;
    const char *text = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
    if (text == NULL) {
        return 0;
    }
    return account_verify(pin, text, len) == 1;
}

/* TOTP の再使用防止 (監査 F10): 最後に受理したステップを
 * 秘密の SHA-256 先頭 8 バイト (タグ) と一緒に覚え、同じ
 * 秘密・同じステップのコードは二度と受理しない。 */
static uint8_t totp_used_tag[8];
static uint64_t totp_used_step;
static int totp_used_valid;

/* TOTP の門。秘密が無ければ素通り (1)。あれば
 * 3 回までコードを聞く。 */
static int totp_gate(const char *name) {
    char text[PRISM_USERS_TEXT_MAX];
    size_t len = 0;
    if (users_load(text, sizeof(text), &len) != 0) {
        return 1;
    }
    char b32[64];
    if (users_totp(text, len, name, b32, sizeof(b32)) != 0) {
        return 1;
    }
    uint8_t secret[32];
    int slen = base32_decode(b32, secret, sizeof(secret));
    if (slen <= 0) {
        serial_write_line("TOTP secret is damaged. Halting.");
        return 0;
    }
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        return 0;
    }
    int64_t unix = rtc_to_unix(&t) - (int64_t)shell_settings.tz_offset * 60;
    if (unix < 0) {
        return 0;
    }
    {
        /* 診断: 時刻の解釈をシリアルに出す (コード自体は
         * 出さない)。 */
        char num[24];
        int ni = 0;
        int64_t v = unix;
        char rev[24];
        int ri = 0;
        if (v == 0) {
            rev[ri++] = '0';
        }
        while (v > 0) {
            rev[ri++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
        num[ni] = '\0';
        serial_write("totp: unix=");
        serial_write_line(num);
    }
    for (int i = 0; i < 3; i++) {
        char line[32];
        serial_write("Code: ");
        read_line_merged(line, sizeof(line));
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
            totp_verify(secret, (size_t)slen, (uint64_t)unix, code,
                        1)) {
            /* 一致したステップを特定し、受理済みステップの
             * 再使用でないことを確かめる (監査 F10)。 */
            uint64_t step = (uint64_t)unix / 30;
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
            int same_tag = totp_used_valid;
            for (int k = 0; k < 8; k++) {
                if (digest[k] != totp_used_tag[k]) {
                    same_tag = 0;
                }
            }
            if (matched >= 0 && same_tag &&
                (uint64_t)matched <= totp_used_step) {
                serial_write_line("Code already used.");
                audit_log_event("login-2fa-fail",
                                "TOTP code reuse");
                memset(digest, 0, sizeof(digest));
                continue;
            }
            if (matched >= 0) {
                for (int k = 0; k < 8; k++) {
                    totp_used_tag[k] = digest[k];
                }
                totp_used_step = (uint64_t)matched;
                totp_used_valid = 1;
            }
            memset(digest, 0, sizeof(digest));
            serial_write_line("Code accepted.");
            audit_log_event("login-2fa", "TOTP accepted");
            memset(secret, 0, sizeof(secret));
            return 1;
        }
        serial_write_line("Wrong code.");
        audit_log_event("login-2fa-fail", "wrong TOTP code");
    }
    memset(secret, 0, sizeof(secret));
    return 0;
}

/* 複数ユーザー有効時のログイン。戻り値は login_flow と
 * 同じ (1 = 成功、0 = 失敗で呼び出し側が停止する)。 */
static int login_flow_multi(char *pin_out, size_t pin_cap,
                            int attempts) {
    char utext[PRISM_USERS_TEXT_MAX];
    size_t ulen = 0;
    users_load(utext, sizeof(utext), &ulen);
    serial_write_line("");
    serial_write_line("PrismOS login");
    serial_write("User (blank = admin, or guest): ");
    char name[PRISM_USER_NAME_MAX + 1];
    read_line_merged(name, sizeof(name));
    if (name[0] == '\0') {
        memcpy(name, "admin", 6);
    }
    if (strcmp(name, "guest") == 0) {
        guest_session = 1;
        login_kind = 2;
        memcpy(current_user, "guest", 6);
        serial_write_line(
            "Welcome, guest. This session is not saved.");
        audit_log_event("login-ok", "guest session");
        if (pin_out != NULL && pin_cap > 0) {
            pin_out[0] = '\0';
        }
        return 1;
    }
    int is_admin = strcmp(name, "admin") == 0;
    if (!is_admin && !users_exists(utext, ulen, name)) {
        serial_write_line("No such user. Halting.");
        audit_log_event("login-fail", "unknown user");
        return 0;
    }
    size_t alen = 0;
    const char *atext = vfs_read_ptr(PRISM_ACCOUNT_FILE, &alen);
    for (int attempt = 0; attempt < attempts; attempt++) {
        char pin[PIN_CAP];
        serial_write("PIN: ");
        read_secret(pin, sizeof(pin));
        int rc;
        if (is_admin) {
            rc = atext != NULL ? account_verify(pin, atext, alen) : 0;
        } else {
            rc = users_verify(utext, ulen, name, pin);
        }
        if (rc == 1) {
            if (!is_admin) {
                /* 主鍵の包みを開く。 */
                uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
                uint32_t iter = 0;
                if (users_salt_iter(utext, ulen, name, salt,
                                    &iter) != 0) {
                    serial_write_line("User record is damaged.");
                    return 0;
                }
                uint8_t uk[32];
                crypto_kdf(pin, salt, PRISM_ACCOUNT_SALT_LEN, iter,
                           uk);
                uint8_t wrap[32], checkb[8];
                if (users_get_wrap(utext, ulen, name, wrap,
                                   checkb) != 0 ||
                    users_unwrap(wrap, checkb, uk, login_mk) != 0) {
                    memset(uk, 0, sizeof(uk));
                    serial_write_line(
                        "No key wrap for this user. Ask the admin "
                        "to run: user passwd <name>");
                    return 0;
                }
                memset(uk, 0, sizeof(uk));
                login_kind = 1;
            }
            if (!totp_gate(name)) {
                serial_write_line("Second factor failed. Halting.");
                return 0;
            }
            size_t cn = 0;
            while (name[cn] != '\0' && cn + 1 < sizeof(current_user)) {
                current_user[cn] = name[cn];
                cn++;
            }
            current_user[cn] = '\0';
            serial_write_line("Welcome to PrismOS.");
            audit_log_event("login-ok", "user login");
            if (pin_out != NULL && pin_cap > 0) {
                size_t i = 0;
                while (pin[i] != '\0' && i + 1 < pin_cap) {
                    pin_out[i] = pin[i];
                    i++;
                }
                pin_out[i] = '\0';
            }
            for (size_t i = 0; pin[i] != '\0'; i++) {
                pin[i] = 0;
            }
            return 1;
        }
        if (rc < 0) {
            serial_write_line("Account data is damaged. Halting.");
            return 0;
        }
        audit_log_event("login-fail", "wrong PIN (multi-user)");
        serial_write_line("Wrong PIN.");
    }
    serial_write_line("Too many attempts. Halting.");
    return 0;
}

/* user / totp コマンド (対話式フロー)。 */
static void users_flow(char *line) {
    if (guest_session) {
        serial_write_line("Guests cannot manage users.");
        return;
    }
    if (starts_with_word(line, "totp")) {
        const char *arg = skip_spaces(line + 4);
        if (line_eq(arg, "off")) {
            char text[PRISM_USERS_TEXT_MAX];
            size_t len = 0;
            if (users_load(text, sizeof(text), &len) != 0) {
                serial_write_line("users.txt is too large.");
                return;
            }
            char newline[64];
            size_t p = 0;
            const char *parts[3] = { "totp ", current_user, " x" };
            /* upsert は置換用。off は行ごと消す代わりに
             * remove の仕組みを使う: totp 行だけ消す。 */
            (void)newline;
            (void)parts;
            (void)p;
            char out[PRISM_USERS_TEXT_MAX];
            /* totp 行を消すため、一時的にユーザー名を
             * 変えた remove は使えないので直接編集する。 */
            size_t op = 0;
            size_t off = 0;
            int dropped = 0;
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
                if (ll > 5 && memcmp(text + st, "totp ", 5) == 0) {
                    /* 名前一致か */
                    size_t q = st + 5;
                    size_t cn = 0;
                    int match = 1;
                    while (current_user[cn] != '\0') {
                        if (q + cn >= st + ll ||
                            text[q + cn] != current_user[cn]) {
                            match = 0;
                            break;
                        }
                        cn++;
                    }
                    if (match &&
                        (q + cn >= st + ll ||
                         text[q + cn] == ' ')) {
                        drop = 1;
                    }
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
                serial_write_line("TOTP is not set up.");
                return;
            }
            if (users_save(out) == 0) {
                serial_write_line("TOTP disabled for this user.");
            } else {
                serial_write_line("Could not save users.txt.");
            }
            return;
        }
        if (line_eq(arg, "setup")) {
            uint8_t secret[10];
            entropy_bytes(secret, sizeof(secret));
            char b32[24];
            if (base32_encode(secret, sizeof(secret), b32,
                              sizeof(b32)) < 0) {
                serial_write_line("Could not make a secret.");
                return;
            }
            char text[PRISM_USERS_TEXT_MAX];
            size_t len = 0;
            if (users_load(text, sizeof(text), &len) != 0) {
                serial_write_line("users.txt is too large.");
                return;
            }
            char tline[96];
            size_t p = 0;
            const char *parts[4] = { "totp ", current_user, " ", b32 };
            for (int i = 0; i < 4; i++) {
                const char *s = parts[i];
                while (*s != '\0' && p + 1 < sizeof(tline)) {
                    tline[p++] = *s++;
                }
            }
            tline[p] = '\0';
            char out[PRISM_USERS_TEXT_MAX];
            if (users_upsert(text, len, tline, out, sizeof(out)) !=
                    0 ||
                users_save(out) != 0) {
                serial_write_line("Could not save users.txt.");
                return;
            }
            serial_write("TOTP secret (enter it in your "
                         "authenticator app): ");
            serial_write_line(b32);
            prism_rtc_time t;
            if (rtc_read(&t) == 0) {
                int64_t unix = rtc_to_unix(&t) -
                               (int64_t)shell_settings.tz_offset * 60;
                if (unix >= 0) {
                    uint32_t code = totp_code(
                        secret, sizeof(secret), (uint64_t)unix, 30,
                        6);
                    char num[16];
                    int ni = 0;
                    char rev[8];
                    int ri = 0;
                    uint32_t v = code;
                    if (v == 0) {
                        rev[ri++] = '0';
                    }
                    while (v > 0) {
                        rev[ri++] = (char)('0' + v % 10);
                        v /= 10;
                    }
                    while (ri < 6) {
                        rev[ri++] = '0';
                    }
                    while (ri > 0) {
                        num[ni++] = rev[--ri];
                    }
                    num[ni] = '\0';
                    serial_write("Current code should read: ");
                    serial_write_line(num);
                }
            }
            memset(secret, 0, sizeof(secret));
            audit_log_event("totp-setup", current_user);
            return;
        }
        serial_write_line("Usage: totp setup | totp off");
        return;
    }
    /* user ... */
    {
        const char *arg0 = skip_spaces(line + 4);
        if (line_eq(arg0, "whoami")) {
            serial_write_line(current_user);
            return;
        }
    }
    if (!current_is_admin()) {
        serial_write_line("Admin only.");
        return;
    }
    const char *arg = skip_spaces(line + 4);
    if (line_eq(arg, "list")) {
        serial_write_line("admin (primary account)");
        char text[PRISM_USERS_TEXT_MAX];
        size_t len = 0;
        if (users_load(text, sizeof(text), &len) == 0 && len > 0) {
            size_t off = 0;
            while (off < len) {
                size_t st = off;
                while (off < len && text[off] != '\n') {
                    off++;
                }
                size_t ll = off - st;
                if (off < len) {
                    off++;
                }
                if (ll > 5 && memcmp(text + st, "user ", 5) == 0) {
                    char nm[PRISM_USER_NAME_MAX + 1];
                    char role[16];
                    size_t q = 0;
                    size_t src = st + 5;
                    while (src < st + ll && text[src] != ' ' &&
                           q < PRISM_USER_NAME_MAX) {
                        nm[q++] = text[src++];
                    }
                    nm[q] = '\0';
                    users_role(text, len, nm, role, sizeof(role));
                    serial_write(nm);
                    serial_write(" (");
                    serial_write(role);
                    serial_write_line(")");
                }
            }
        }
        serial_write_line("guest (no password, session only)");
        return;
    }
    if (starts_with_word(arg, "add") || starts_with_word(arg, "passwd")) {
        int is_add = starts_with_word(arg, "add");
        const char *p = skip_spaces(arg + (is_add ? 3 : 6));
        char name[PRISM_USER_NAME_MAX + 1];
        size_t q = 0;
        while (p[q] != '\0' && p[q] != ' ' &&
               q < PRISM_USER_NAME_MAX) {
            name[q] = p[q];
            q++;
        }
        name[q] = '\0';
        const char *role = strstr(p, " admin") != NULL ? "admin" : "user";
        char text[PRISM_USERS_TEXT_MAX];
        size_t len = 0;
        if (users_load(text, sizeof(text), &len) != 0) {
            serial_write_line("users.txt is too large.");
            return;
        }
        if (is_add) {
            if (!users_name_ok(name)) {
                serial_write_line(
                    "Bad user name (1-16 chars, letters/digits/_-; "
                    "admin and guest are reserved).");
                return;
            }
            if (users_exists(text, len, name)) {
                serial_write_line("That user already exists.");
                return;
            }
        } else if (!users_exists(text, len, name)) {
            serial_write_line("No such user.");
            return;
        }
        uint8_t mk[32];
        if (persist_get_key(mk) != 0) {
            serial_write_line(
                "The disk key is not active; log in first.");
            return;
        }
        serial_write("New PIN: ");
        char pin1[PIN_CAP];
        read_secret(pin1, sizeof(pin1));
        serial_write("Confirm PIN: ");
        char pin2[PIN_CAP];
        read_secret(pin2, sizeof(pin2));
        if (strcmp(pin1, pin2) != 0 || !account_pin_ok(pin1)) {
            serial_write_line(
                "PINs do not match or are not valid (4-16 chars).");
            memset(mk, 0, sizeof(mk));
            return;
        }
        uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
        make_salt(salt);
        char uline[256];
        if (users_make_line(name, salt, PRISM_ACCOUNT_ITERATIONS,
                            pin1, role, uline, sizeof(uline)) != 0) {
            serial_write_line("Could not build the user record.");
            memset(mk, 0, sizeof(mk));
            return;
        }
        uint8_t uk[32];
        crypto_kdf(pin1, salt, PRISM_ACCOUNT_SALT_LEN,
                   PRISM_ACCOUNT_ITERATIONS, uk);
        char wline[256];
        if (users_make_wrap_line(name, mk, uk, wline,
                                 sizeof(wline)) != 0) {
            serial_write_line("Could not wrap the disk key.");
            memset(mk, 0, sizeof(mk));
            memset(uk, 0, sizeof(uk));
            return;
        }
        memset(mk, 0, sizeof(mk));
        memset(uk, 0, sizeof(uk));
        char out[PRISM_USERS_TEXT_MAX];
        if (users_upsert(text, len, uline, out, sizeof(out)) != 0) {
            serial_write_line("users.txt is full.");
            return;
        }
        size_t olen = 0;
        while (out[olen] != '\0') {
            olen++;
        }
        if (users_upsert(out, olen, wline, text, sizeof(text)) != 0) {
            serial_write_line("users.txt is full.");
            return;
        }
        if (users_save(text) == 0) {
            serial_write(is_add ? "User added: " : "PIN updated: ");
            serial_write_line(name);
            audit_log_event(is_add ? "user-add" : "user-passwd", name);
        } else {
            serial_write_line("Could not save users.txt.");
        }
        return;
    }
    if (starts_with_word(arg, "del")) {
        const char *p = skip_spaces(arg + 3);
        char name[PRISM_USER_NAME_MAX + 1];
        size_t q = 0;
        while (p[q] != '\0' && p[q] != ' ' &&
               q < PRISM_USER_NAME_MAX) {
            name[q] = p[q];
            q++;
        }
        name[q] = '\0';
        char text[PRISM_USERS_TEXT_MAX];
        size_t len = 0;
        if (users_load(text, sizeof(text), &len) != 0 ||
            !users_exists(text, len, name)) {
            serial_write_line("No such user.");
            return;
        }
        char out[PRISM_USERS_TEXT_MAX];
        if (users_remove_user(text, len, name, out, sizeof(out)) !=
                0 ||
            users_save(out) != 0) {
            serial_write_line("Could not save users.txt.");
            return;
        }
        serial_write("User deleted: ");
        serial_write_line(name);
        audit_log_event("user-del", name);
        return;
    }
    serial_write_line(
        "Usage: user list | user add <name> [admin] | "
        "user passwd <name> | user del <name> | user whoami");
}

static int login_flow(char *pin_out, size_t pin_cap, int attempts,
                      uint32_t lockout_seconds) {
    size_t len = 0;
    const char *text = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
    if (text == NULL) {
        if (pin_out != NULL && pin_cap > 0) {
            pin_out[0] = '\0';
        }
        return 0;
    }
    if (attempts < 1) {
        attempts = 1;
    }
    if (attempts > 10) {
        attempts = 10;
    }
    /* users.txt に追加ユーザーか TOTP があれば、
     * ユーザー選択ありのログインにする。主アカウント
     * だけの環境では従来の PIN のみの流れのまま。 */
    {
        char utext[PRISM_USERS_TEXT_MAX];
        size_t ulen = 0;
        if (users_load(utext, sizeof(utext), &ulen) == 0 &&
            ulen > 0) {
            int r = login_flow_multi(pin_out, pin_cap, attempts);
            if (r == 0) {
                prism_halt();
            }
            return r;
        }
    }
    serial_write_line("");
    serial_write_line("PrismOS login");
    for (int round = 0; round < 3; round++) {
        for (int attempt = 0; attempt < attempts; attempt++) {
            char pin[PIN_CAP];
            serial_write("PIN: ");
            read_secret(pin, sizeof(pin));
            int rc = account_verify(pin, text, len);
            if (rc == 1) {
                serial_write_line("Welcome to PrismOS.");
                audit_log_event("login-ok", "PIN accepted");
                if (pin_out != NULL && pin_cap > 0) {
                    size_t i = 0;
                    while (pin[i] != '\0' && i + 1 < pin_cap) {
                        pin_out[i] = pin[i];
                        i++;
                    }
                    pin_out[i] = '\0';
                }
                for (size_t i = 0; pin[i] != '\0'; i++) {
                    pin[i] = 0;
                }
                return 1;
            }
            if (rc < 0) {
                serial_write_line("Account file is damaged. Halting.");
                audit_log_event("login-fail", "account damaged");
                prism_halt();
            }
            audit_log_event("login-fail", "wrong PIN");
            serial_write_line("Wrong PIN.");
        }
        if (lockout_seconds == 0 || round == 2) {
            break;
        }
        audit_log_event("lockout", "too many wrong PINs");
        serial_write("Locked for ");
        {
            char num[12];
            uint32_t v = lockout_seconds;
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
                num[q++] = tmp[--d];
            }
            num[q] = '\0';
            serial_write(num);
        }
        serial_write_line(" seconds.");
        wait_seconds(lockout_seconds);
    }
    serial_write_line("Too many attempts. Halting.");
    prism_halt();
    return 0;
}

/* ソルトを作る。エントロピープールから取る (キー入力・
 * マウス・時刻・RDRAND があればその値を混ぜたもの)。
 * プールが空でも SHA-256 で拡散するため、同じ値には
 * なりにくい。 */
static void make_salt(uint8_t *salt) {
    entropy_bytes(salt, PRISM_ACCOUNT_SALT_LEN);
}

static void passwd_flow(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
    if (text != NULL) {
        char current[PIN_CAP];
        serial_write("Current PIN: ");
        read_secret(current, sizeof(current));
        int rc = account_verify(current, text, len);
        if (rc != 1) {
            serial_write_line("Wrong PIN.");
            return;
        }
    }
    char pin1[PIN_CAP];
    char pin2[PIN_CAP];
    serial_write("New PIN (4-16 chars): ");
    read_secret(pin1, sizeof(pin1));
    if (!account_pin_ok_policy(pin1, shell_settings.pin_min_len,
                               shell_settings.pin_require_digit)) {
        serial_write("PIN must be ");
        {
            char num[4];
            int v = shell_settings.pin_min_len;
            int d = 0;
            char tmp[4];
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
                num[q++] = tmp[--d];
            }
            num[q] = '\0';
            serial_write(num);
        }
        serial_write(" or more printable characters");
        if (shell_settings.pin_require_digit) {
            serial_write(" including a digit");
        }
        serial_write_line(".");
        return;
    }
    serial_write("Confirm PIN: ");
    read_secret(pin2, sizeof(pin2));
    if (strcmp(pin1, pin2) != 0) {
        serial_write_line("PINs do not match.");
        return;
    }
    uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
    make_salt(salt);
    char account_text[PRISM_ACCOUNT_TEXT_MAX];
    if (account_make_text(salt, PRISM_ACCOUNT_ITERATIONS, pin1,
                          account_text,
                          sizeof(account_text)) != 0) {
        serial_write_line("Failed to set PIN.");
        return;
    }
    size_t text_len = 0;
    while (account_text[text_len] != '\0') {
        text_len++;
    }
    if (vfs_write(PRISM_ACCOUNT_FILE, account_text, text_len) != 0) {
        serial_write_line("Failed to save account.");
        return;
    }
    /* 新しい PIN から反復ハッシュで鍵を作る。次に同期するとき、
     * 全ファイルを新しい鍵で暗号化し直す。 */
    uint8_t key[32];
    crypto_kdf(pin1, salt, PRISM_ACCOUNT_SALT_LEN,
               PRISM_ACCOUNT_ITERATIONS, key);
    /* 追加ユーザーの主鍵の包みを新しい主鍵へ包み直す。
     * これをしないと、プライマリ PIN の変更で追加ユーザーが
     * 古い主鍵しか開けなくなり、ログインできなくなる
     * (既知の問題)。旧主鍵は現在のセッション鍵から取る。
     * 包み直しに失敗した場合は `user passwd <name>` で
     * 個別に直せる (ログイン時の案内どおり)。 */
    {
        uint8_t old_mk[32];
        size_t ulen = 0;
        const char *utext = vfs_read_ptr(PRISM_USERS_FILE, &ulen);
        if (utext != NULL && persist_get_key(old_mk) == 0) {
            static char new_utext[PRISM_USERS_TEXT_MAX];
            int nwrap = 0;
            if (users_rewrap_all(utext, ulen, old_mk, key,
                                 new_utext, sizeof(new_utext),
                                 &nwrap) == 0 &&
                nwrap > 0) {
                if (users_save(new_utext) == 0) {
                    serial_write("passwd: re-wrapped the disk "
                                 "key for ");
                    print_u64_serial((uint64_t)nwrap);
                    serial_write_line(" user(s).");
                }
            }
        }
        memset(old_mk, 0, sizeof(old_mk));
    }
    persist_set_key(key);
    for (int i = 0; i < 32; i++) {
        key[i] = 0;
    }
    for (size_t i = 0; pin1[i] != '\0'; i++) {
        pin1[i] = 0;
    }
    for (size_t i = 0; pin2[i] != '\0'; i++) {
        pin2[i] = 0;
    }
    audit_log_event("passwd", "PIN changed");
    serial_write_line("Account set. PIN will be required at next boot.");
}

/* `net`: show network status, configuring on first use. */
static void print_net_status(void) {
    if (net_ensure() != 0) {
        serial_write_line("network: not available (no card or no DHCP).");
        return;
    }
    char buf[96];
    net_status(buf, sizeof(buf));
    serial_write("network: ");
    serial_write_line(buf);
}

/* `disk`: show the system disk (kind/model/size). */
static void print_disk_status(void) {
    if (!block_present()) {
        serial_write_line("disk: (no disk found)");
        return;
    }
    serial_write("disk: ");
    serial_write(block_kind());
    serial_write(" ");
    serial_write(block_model());
    serial_write(" ");
    print_u64_serial(block_total_sectors());
    serial_write_line(" sectors");
    for (int dev = 1; dev <= 3; dev++) {
        if (block_dev_sectors(dev) == 0) {
            continue;
        }
        serial_write("disk");
        print_u64_serial((uint64_t)dev);
        serial_write(": ");
        serial_write(block_dev_kind(dev));
        serial_write(" ");
        print_u64_serial(block_dev_sectors(dev));
        serial_write_line(" sectors");
    }
}

/* `disk test <dev>`: 末尾のセクタにパターンを書いて
 * 読み戻し、一致するか確かめる (データを壊さないよう
 * ファイル領域でない末尾を使う)。 */
static void disk_test_flow(const char *line) {
    const char *p = skip_spaces(line + 9); /* skip "disk test" */
    int dev = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        dev = dev * 10 + (*p - '0');
        p++;
        any = 1;
    }
    if (!any || block_dev_sectors(dev) == 0) {
        serial_write_line("disk test: no such device");
        return;
    }
    uint64_t lba = block_dev_sectors(dev) - 1;
    static uint8_t wbuf[512];
    static uint8_t rbuf[512];
    for (int i = 0; i < 512; i++) {
        wbuf[i] = (uint8_t)(i * 7 + 13);
    }
    if (block_write_sector_dev(dev, lba, wbuf) != 0) {
        serial_write_line("disk test: write failed");
        return;
    }
    if (block_read_sector_dev(dev, lba, rbuf) != 0) {
        serial_write_line("disk test: read failed");
        return;
    }
    for (int i = 0; i < 512; i++) {
        if (rbuf[i] != wbuf[i]) {
            serial_write_line("disk test: mismatch");
            return;
        }
    }
    serial_write_line("disk test: ok");
}

/* `usb`: list USB devices found by the UHCI host. */
static void print_usb_status(void) {
    if (!uhci_present()) {
        serial_write_line("usb: (no USB host found)");
        return;
    }
    int n = uhci_device_count();
    serial_write("usb: ");
    print_u64_serial((uint64_t)n);
    serial_write_line(" device(s)");
    for (int i = 0; i < n; i++) {
        const uhci_device *d = uhci_device_at(i);
        if (d == NULL) {
            continue;
        }
        serial_write("  port ");
        print_u64_serial(d->port);
        serial_write(" addr ");
        print_u64_serial(d->address);
        serial_write(" vendor 0x");
        print_hex16_serial(d->vendor);
        serial_write(" product 0x");
        print_hex16_serial(d->product);
        serial_write(" class ");
        print_u64_serial(d->dev_class);
        serial_write(" if-class ");
        print_u64_serial(d->if_class);
        serial_write(d->low_speed ? " (low speed)" : "");
        switch (d->kind) {
            case UHCI_KIND_HID_KEYBOARD:
                serial_write_line(" hid keyboard");
                break;
            case UHCI_KIND_HID_MOUSE:
                serial_write_line(" hid mouse");
                break;
            case UHCI_KIND_STORAGE:
                serial_write(" storage ");
                print_u64_serial(d->storage_sectors);
                serial_write_line(" sectors");
                break;
            default:
                serial_write_line("");
                break;
        }
    }
}

/* `usb test`: write a pattern sector to the USB storage
 * (block device 1) at LBA 100, read it back and compare.
 * Proves the BOT/SCSI write path end to end. The test
 * sector is beyond any file system area the test disk
 * would hold; the driver test uses a scratch image. */
static void usb_storage_test(void) {
    if (!uhci_storage_present()) {
        serial_write_line("usb: no storage device.");
        return;
    }
    static uint8_t wbuf[BLOCK_SECTOR_SIZE];
    static uint8_t rbuf[BLOCK_SECTOR_SIZE];
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        wbuf[i] = (uint8_t)(i * 7 + 3);
    }
    const uint64_t lba = 100;
    if (block_write_sector_dev(1, lba, wbuf) != 0) {
        serial_write_line("usb: storage write failed.");
        return;
    }
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        rbuf[i] = 0;
    }
    if (block_read_sector_dev(1, lba, rbuf) != 0) {
        serial_write_line("usb: storage read failed.");
        return;
    }
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        if (rbuf[i] != wbuf[i]) {
            serial_write_line("usb: storage verify failed.");
            return;
        }
    }
    serial_write_line("usb: storage test passed.");
}

/* `ehci`: list USB devices found by the EHCI (USB 2.0) host. */
static void print_ehci_status(void) {
    if (!ehci_present()) {
        serial_write_line("ehci: (no EHCI host found)");
        return;
    }
    int n = ehci_device_count();
    serial_write("ehci: ");
    print_u64_serial((uint64_t)n);
    serial_write_line(" device(s)");
    for (int i = 0; i < n; i++) {
        const ehci_device *d = ehci_device_at(i);
        if (d == NULL) {
            continue;
        }
        serial_write("  port ");
        print_u64_serial(d->port);
        serial_write(" addr ");
        print_u64_serial(d->address);
        serial_write(" vendor 0x");
        print_hex16_serial(d->vendor);
        serial_write(" product 0x");
        print_hex16_serial(d->product);
        serial_write(" if-class ");
        print_u64_serial(d->if_class);
        serial_write(d->speed == 2   ? " (high speed)"
                     : d->speed == 1 ? " (low speed)"
                                     : " (full speed)");
        if (d->storage_sectors != 0) {
            serial_write(" storage ");
            print_u64_serial(d->storage_sectors);
            serial_write_line(" sectors");
        } else {
            serial_write_line("");
        }
    }
}

/* `ehci test`: write a pattern sector to the EHCI storage
 * (block device 4) at LBA 100, read it back and compare. */
static void ehci_storage_test(void) {
    if (!ehci_storage_present()) {
        serial_write_line("ehci: no storage device.");
        return;
    }
    static uint8_t wbuf[BLOCK_SECTOR_SIZE];
    static uint8_t rbuf[BLOCK_SECTOR_SIZE];
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        wbuf[i] = (uint8_t)(i * 13 + 5);
    }
    const uint64_t lba = 100;
    if (block_write_sector_dev(4, lba, wbuf) != 0) {
        serial_write_line("ehci: storage write failed.");
        return;
    }
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        rbuf[i] = 0;
    }
    if (block_read_sector_dev(4, lba, rbuf) != 0) {
        serial_write_line("ehci: storage read failed.");
        return;
    }
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        if (rbuf[i] != wbuf[i]) {
            serial_write_line("ehci: storage verify failed.");
            return;
        }
    }
    serial_write_line("ehci: storage test passed.");
}

/* `xhci`: list USB devices found by the xHCI (USB 3.x) host. */
static void print_xhci_status(void) {
    if (!xhci_present()) {
        serial_write_line("xhci: (no xHCI host found)");
        return;
    }
    int n = xhci_device_count();
    serial_write("xhci: ");
    print_u64_serial((uint64_t)n);
    serial_write_line(" device(s)");
    for (int i = 0; i < n; i++) {
        const xhci_device *d = xhci_device_at(i);
        if (d == NULL) {
            continue;
        }
        serial_write("  port ");
        print_u64_serial(d->port);
        serial_write(" addr ");
        print_u64_serial(d->address);
        serial_write(" vendor 0x");
        print_hex16_serial(d->vendor);
        serial_write(" product 0x");
        print_hex16_serial(d->product);
        serial_write(" if-class ");
        print_u64_serial(d->if_class);
        serial_write(d->speed == 3   ? " (super speed)"
                     : d->speed == 2 ? " (high speed)"
                     : d->speed == 1 ? " (low speed)"
                                     : " (full speed)");
        if (d->storage_sectors != 0) {
            serial_write(" storage ");
            print_u64_serial(d->storage_sectors);
            serial_write_line(" sectors");
        } else {
            serial_write_line("");
        }
    }
}

/* `xhci test`: write a pattern sector to the xHCI storage
 * (block device 5) at LBA 100, read it back and compare. */
static void xhci_storage_test(void) {
    if (!xhci_storage_present()) {
        serial_write_line("xhci: no storage device.");
        return;
    }
    static uint8_t wbuf[BLOCK_SECTOR_SIZE];
    static uint8_t rbuf[BLOCK_SECTOR_SIZE];
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        wbuf[i] = (uint8_t)(i * 13 + 5);
    }
    const uint64_t lba = 100;
    if (block_write_sector_dev(5, lba, wbuf) != 0) {
        serial_write_line("xhci: storage write failed.");
        return;
    }
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        rbuf[i] = 0;
    }
    if (block_read_sector_dev(5, lba, rbuf) != 0) {
        serial_write_line("xhci: storage read failed.");
        return;
    }
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        if (rbuf[i] != wbuf[i]) {
            serial_write_line("xhci: storage verify failed.");
            return;
        }
    }
    serial_write_line("xhci: storage test passed.");
}

/* ---- pq: 耐量子鍵交換と署名 ----
 * 実際に使える手順としてシェルに載せたもの:
 *   pq keygen            ML-KEM-768 の鍵対を作り
 *                        pqkem.pub / pqkem.key に保存
 *   pq encaps <pub> <ct> 相手の公開鍵でカプセル化し、
 *                        暗号文ファイルとセッション鍵
 *                        (RAM 内) を得る
 *   pq decaps <ct>       自分の秘密鍵で共有秘密を取り
 *                        戻し、同じセッション鍵を得る
 *   pq seal <in> <out>   セッション鍵で封をする
 *                        (AES-256-CTR + HMAC-SHA-256)
 *   pq open <in> <out>   封を開ける (タグ検証必須)
 *   pq signkey           ML-DSA-44 の署名鍵対を作る
 *   pq sign <file>       <file>.sig に署名を書く
 *   pq verify <file>     署名を検証する
 * 両端でセッション鍵のフィンガープリントが一致すれば、
 * 鍵交換が相手認証つきで成立したことになる (ピン留めと
 * 同じ考え方で、フィンガープリントは別経路で照合する)。 */
static uint8_t pq_session_key[32];
static int pq_session_valid;
static uint8_t pq_ek[MLKEM768_EK_LEN];
static uint8_t pq_dk[MLKEM768_DK_LEN];
static uint8_t pq_ct[MLKEM768_CT_LEN];
static uint8_t pq_pk[MLDSA44_PK_LEN];
static uint8_t pq_sk[MLDSA44_SK_LEN];
static uint8_t pq_sig[MLDSA44_SIG_LEN];
static uint8_t pq_data[PRISM_VFS_MAX_FILE_SIZE];
static uint8_t pq_sealed[PRISM_VFS_MAX_FILE_SIZE + 64];

static void pq_derive_session(const uint8_t ss[MLKEM_SS_LEN]) {
    crypto_sha256_ctx h;
    crypto_sha256_init(&h);
    crypto_sha256_update(&h, ss, MLKEM_SS_LEN);
    crypto_sha256_update(&h, (const uint8_t *)"PrismOS PQ session v1",
                         21);
    crypto_sha256_final(&h, pq_session_key);
    pq_session_valid = 1;
}

static void pq_print_fingerprint(void) {
    uint8_t fp[32];
    crypto_sha256(pq_session_key, 32, fp);
    static const char hexd[] = "0123456789abcdef";
    char out[9];
    for (int i = 0; i < 4; i++) {
        out[2 * i] = hexd[fp[i] >> 4];
        out[2 * i + 1] = hexd[fp[i] & 15];
    }
    out[8] = '\0';
    serial_write("pq: session fingerprint ");
    serial_write_line(out);
}

/* HMAC-SHA-256 (crypto.c の SHA-256 から構成)。 */
static void pq_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *data, size_t data_len,
                           uint8_t out[32]) {
    uint8_t k[64];
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) {
        k[i] = 0;
    }
    if (key_len > 64) {
        crypto_sha256(key, key_len, k);
    } else {
        for (size_t i = 0; i < key_len; i++) {
            k[i] = key[i];
        }
    }
    crypto_sha256_ctx h;
    for (int i = 0; i < 64; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x36);
    }
    crypto_sha256_init(&h);
    crypto_sha256_update(&h, pad, 64);
    crypto_sha256_update(&h, data, data_len);
    uint8_t inner[32];
    crypto_sha256_final(&h, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x5C);
    }
    crypto_sha256_init(&h);
    crypto_sha256_update(&h, pad, 64);
    crypto_sha256_update(&h, inner, 32);
    crypto_sha256_final(&h, out);
}

/* 空白区切りの単語を 1 つ取る (mail_next_word と同じ役)。 */
static int pq_next_word(const char **p, char *out, size_t cap) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        return -1;
    }
    size_t n = 0;
    while (*s != '\0' && *s != ' ' && *s != '\t') {
        if (n + 1 < cap) {
            out[n++] = *s;
        }
        s++;
    }
    out[n] = '\0';
    *p = s;
    return 0;
}

static int pq_read_file(const char *name, uint8_t *buf, size_t cap,
                        size_t *out_len) {
    size_t len = 0;
    const char *text = vfs_read_ptr(name, &len);
    if (text == NULL || len > cap) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)text[i];
    }
    *out_len = len;
    return 0;
}

/* ---- 生体認証 (bio) と画面ロック (lock) ----
 * 登録・照合は bio.c。lock は鍵が有効な間の再ロックで、
 * PIN か生体で解除する (起動ログインはディスク鍵の
 * 都合で PIN が要る。bio.h の説明を見ること)。 */
static void bio_flow(const char *line) {
    const char *arg = skip_spaces(line + 3); /* skip "bio" */
    char w1[16];
    char w2[16];
    const char *p = arg;
    if (pq_next_word(&p, w1, sizeof(w1)) != 0) {
        serial_write_line(
            "Usage: bio enroll finger|face | bio verify "
            "finger|face | bio status | bio clear | bio mock <id>");
        return;
    }
    if (strcmp(w1, "mock") == 0) {
        if (pq_next_word(&p, w2, sizeof(w2)) != 0) {
            serial_write_line("Usage: bio mock <id> (0 = none)");
            return;
        }
        int id = 0;
        for (const char *q = w2; *q >= '0' && *q <= '9'; q++) {
            id = id * 10 + (*q - '0');
        }
        bio_mock_present(id);
        serial_write("bio: simulator presenting sample ");
        print_u64_serial((uint64_t)id);
        serial_write_line(".");
        return;
    }
    if (strcmp(w1, "status") == 0) {
        serial_write("bio: finger ");
        serial_write_line(bio_enrolled(BIO_TYPE_FINGER)
                              ? "enrolled"
                              : "not enrolled");
        serial_write("bio: face ");
        serial_write_line(bio_enrolled(BIO_TYPE_FACE)
                              ? "enrolled"
                              : "not enrolled");
        if (bio_locked()) {
            serial_write_line("bio: locked (too many failures)");
        }
        return;
    }
    if (strcmp(w1, "clear") == 0) {
        if (bio_clear() == 0) {
            serial_write_line("bio: templates cleared.");
        } else {
            serial_write_line("bio: could not store the change.");
        }
        return;
    }
    if (strcmp(w1, "enroll") == 0 || strcmp(w1, "verify") == 0) {
        if (pq_next_word(&p, w2, sizeof(w2)) != 0 ||
            (strcmp(w2, "finger") != 0 && strcmp(w2, "face") != 0)) {
            serial_write_line("Usage: bio enroll finger|face");
            return;
        }
        int type = (strcmp(w2, "finger") == 0) ? BIO_TYPE_FINGER
                                               : BIO_TYPE_FACE;
        if (w1[0] == 'e') {
            int rc = bio_enroll(type);
            if (rc == 0) {
                serial_write_line("bio: enrolled.");
            } else if (rc == -1) {
                serial_write_line(
                    "bio: no sample presented (see `bio mock`).");
            } else {
                serial_write_line("bio: could not store the template.");
            }
            return;
        }
        int rc = bio_verify(type);
        if (rc == 0) {
            serial_write_line("bio: match.");
        } else if (rc == -1) {
            serial_write_line("bio: no match.");
        } else if (rc == -2) {
            serial_write_line("bio: locked (too many failures).");
        } else if (rc == -3) {
            serial_write_line("bio: not enrolled.");
        } else {
            serial_write_line(
                "bio: no sample presented (see `bio mock`).");
        }
        return;
    }
    serial_write_line(
        "Usage: bio enroll finger|face | bio verify finger|face | "
        "bio status | bio clear | bio mock <id>");
}

/* セッションの再ロック。PIN か生体で解除する。 */
static void lock_flow(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
    int have_bio = bio_enrolled(BIO_TYPE_FINGER) ||
                   bio_enrolled(BIO_TYPE_FACE);
    if (text == NULL && !have_bio) {
        serial_write_line(
            "lock: no PIN set and no biometrics enrolled.");
        return;
    }
    serial_write_line("Locked. Enter PIN, or type bio to use "
                      "biometrics.");
    int wrong = 0;
    for (;;) {
        char line[PIN_CAP + 8];
        serial_write("unlock: ");
        read_line_merged(line, sizeof(line));
        if (strcmp(line, "bio") == 0) {
            int rc = -3;
            if (bio_enrolled(BIO_TYPE_FINGER)) {
                rc = bio_verify(BIO_TYPE_FINGER);
            }
            if (rc != 0 && bio_enrolled(BIO_TYPE_FACE)) {
                rc = bio_verify(BIO_TYPE_FACE);
            }
            if (rc == 0) {
                serial_write_line("Unlocked.");
                audit_log_event("unlock", "biometrics accepted");
                return;
            }
            if (rc == -2) {
                serial_write_line(
                    "Biometrics locked. Use the PIN.");
            } else if (rc == -3) {
                serial_write_line("No biometrics enrolled.");
            } else {
                serial_write_line("No match.");
            }
            continue;
        }
        if (text != NULL && account_verify(line, text, len) == 1) {
            bio_unlock_reset();
            serial_write_line("Unlocked.");
            audit_log_event("unlock", "PIN accepted");
            return;
        }
        serial_write_line("Wrong PIN.");
        audit_log_event("unlock-fail", "wrong PIN at lock screen");
        wrong++;
        if (wrong >= 9) {
            serial_write_line("Too many attempts. Halting.");
            prism_halt();
        }
    }
}


/* ---- 後付けドライバ (driver コマンド) ----
 * 導入には ML-DSA-44 署名が必須。信頼の根は drvroot.pub、
 * 署名鍵は drvroot.key (*.key としてゲストからも保護される)。
 * driver sign は開発用 (本来の署名は OS の外で行う前提)。
 * 問題のあるドライバは drivers.disabled に名前を入れて
 * 導入を拒否できる (回復手段)。 */
static uint8_t drv_pk[MLDSA44_PK_LEN];
static uint8_t drv_sk[MLDSA44_SK_LEN];
static uint8_t drv_sig[MLDSA44_SIG_LEN];

static int drv_name_disabled(const char *name) {
    size_t len = 0;
    const char *text = vfs_read_ptr("drivers.disabled", &len);
    if (text == NULL) {
        return 0;
    }
    size_t nl = 0;
    while (name[nl] != '\0') {
        nl++;
    }
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') {
            j++;
        }
        if (j - i == nl) {
            size_t k = 0;
            while (k < nl && text[i + k] == name[k]) {
                k++;
            }
            if (k == nl) {
                return 1;
            }
        }
        i = j + 1;
    }
    return 0;
}

static void drv_set_disabled(const char *name, int disable) {
    static char buf[1024];
    size_t len = 0;
    const char *text = vfs_read_ptr("drivers.disabled", &len);
    size_t n = 0;
    if (text != NULL) {
        while (n < len && n < sizeof(buf) - 64) {
            buf[n] = text[n];
            n++;
        }
    }
    buf[n] = '\0';
    if (disable) {
        if (drv_name_disabled(name)) {
            serial_write_line("driver: already disabled.");
            return;
        }
        size_t k = 0;
        while (name[k] != '\0' && n + 1 < sizeof(buf)) {
            buf[n++] = name[k++];
        }
        if (n + 1 < sizeof(buf)) {
            buf[n++] = '\n';
        }
        buf[n] = '\0';
        if (vfs_write("drivers.disabled", buf, n) == 0) {
            serial_write_line("driver: disabled (installs refused).");
        }
    } else {
        /* 行を取り除いて書き直す。 */
        static char out[1024];
        size_t o = 0;
        size_t i = 0;
        size_t nl = 0;
        while (name[nl] != '\0') {
            nl++;
        }
        while (i < n) {
            size_t j = i;
            while (j < n && buf[j] != '\n') {
                j++;
            }
            int match = (j - i == nl);
            if (match) {
                size_t k = 0;
                while (k < nl && buf[i + k] == name[k]) {
                    k++;
                }
                match = (k == nl);
            }
            if (!match) {
                for (size_t k = i; k < j && o + 1 < sizeof(out); k++) {
                    out[o++] = buf[k];
                }
                if (o + 1 < sizeof(out)) {
                    out[o++] = '\n';
                }
            }
            i = j + 1;
        }
        out[o] = '\0';
        if (vfs_write("drivers.disabled", out, o) == 0) {
            serial_write_line("driver: enabled.");
        }
    }
}

int shell_driver_name_disabled(const char *name) {
    return drv_name_disabled(name);
}

void shell_driver_set_disabled(const char *name, int disable) {
    drv_set_disabled(name, disable);
}

/* ドライバの特権操作 (rootkey / sign / install / disable /
 * enable) の門。ドライバは ring 0 で動くため、署名・導入は
 * 管理者ロールに加えて対話的な管理者 PIN 確認を必須にする
 * (監査 F1: ゲストや通常ユーザーが端末内の鍵で自作自演の
 * 署名をして導入できてしまう問題への対策)。 */
static int driver_admin_gate(void) {
    if (!current_is_admin()) {
        serial_write_line("Admin only.");
        audit_log_event("driver-refused", "not admin");
        return 0;
    }
    return admin_check();
}

static void driver_flow(const char *line) {
    if (guest_session) {
        serial_write_line("Guests cannot manage drivers.");
        audit_log_event("driver-refused", "guest session");
        return;
    }
    const char *arg = skip_spaces(line + 6); /* skip "driver" */
    char w1[64];
    const char *p = arg;
    if (starts_with_word(arg, "rootkey")) {
        if (!driver_admin_gate()) {
            return;
        }
        if (vfs_exists("drvroot.pub")) {
            serial_write_line("driver: root key already exists.");
            return;
        }
        uint8_t seed[MLDSA44_SEED_LEN];
        entropy_reseed();
        entropy_bytes(seed, sizeof(seed));
        if (mldsa44_keygen_from_seed(seed, drv_pk, drv_sk) != 0) {
            serial_write_line("driver: keygen failed.");
            return;
        }
        if (vfs_write("drvroot.pub", (const char *)drv_pk,
                      MLDSA44_PK_LEN) != 0 ||
            vfs_write("drvroot.key", (const char *)drv_sk,
                      MLDSA44_SK_LEN) != 0) {
            serial_write_line("driver: could not store the root key.");
            return;
        }
        serial_write_line("driver: root key created "
                          "(drvroot.pub / drvroot.key).");
        return;
    }
    if (starts_with_word(arg, "sign")) {
        if (!driver_admin_gate()) {
            return;
        }
        p = skip_spaces(arg + 4);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0) {
            serial_write_line("Usage: driver sign <package>");
            return;
        }
        size_t len = 0;
        if (pq_read_file("drvroot.key", drv_sk, sizeof(drv_sk),
                         &len) != 0 ||
            len != MLDSA44_SK_LEN) {
            serial_write_line("driver: no root key "
                              "(run `driver rootkey`).");
            return;
        }
        size_t plen = 0;
        const char *pkg = vfs_read_ptr(w1, &plen);
        if (pkg == NULL) {
            serial_write_line("driver: no such package.");
            return;
        }
        if (mldsa44_sign(drv_sk, (const uint8_t *)pkg, plen,
                         drv_sig) != 0) {
            serial_write_line("driver: signing failed.");
            return;
        }
        char name[80];
        size_t n = 0;
        while (w1[n] != '\0' && n + 5 < sizeof(name)) {
            name[n] = w1[n];
            n++;
        }
        const char *suf = ".sig";
        while (*suf != '\0' && n + 1 < sizeof(name)) {
            name[n++] = *suf++;
        }
        name[n] = '\0';
        if (vfs_write(name, (const char *)drv_sig,
                      MLDSA44_SIG_LEN) != 0) {
            serial_write_line("driver: could not store the signature.");
            return;
        }
        serial_write_line("driver: signed (development signing).");
        return;
    }
    if (starts_with_word(arg, "install")) {
        if (!driver_admin_gate()) {
            return;
        }
        p = skip_spaces(arg + 7);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0) {
            serial_write_line("Usage: driver install <package>");
            return;
        }
        if (drv_name_disabled(w1)) {
            serial_write_line("driver: refused (disabled). "
                              "Use `driver enable <name>` to undo.");
            audit_log_event("driver-refused", w1);
            return;
        }
        size_t len = 0;
        if (pq_read_file("drvroot.pub", drv_pk, sizeof(drv_pk),
                         &len) != 0 ||
            len != MLDSA44_PK_LEN) {
            serial_write_line("driver: no root key "
                              "(run `driver rootkey`).");
            return;
        }
        size_t plen = 0;
        const char *pkg = vfs_read_ptr(w1, &plen);
        if (pkg == NULL) {
            serial_write_line("driver: no such package.");
            return;
        }
        char name[80];
        size_t n = 0;
        while (w1[n] != '\0' && n + 5 < sizeof(name)) {
            name[n] = w1[n];
            n++;
        }
        const char *suf = ".sig";
        while (*suf != '\0' && n + 1 < sizeof(name)) {
            name[n++] = *suf++;
        }
        name[n] = '\0';
        if (pq_read_file(name, drv_sig, sizeof(drv_sig), &len) != 0 ||
            len != MLDSA44_SIG_LEN) {
            serial_write_line("driver: refused (no signature). "
                              "Unsigned drivers are not loaded.");
            audit_log_event("driver-refused", w1);
            return;
        }
        if (mldsa44_verify(drv_pk, (const uint8_t *)pkg, plen,
                           drv_sig) != 0) {
            serial_write_line("driver: refused (bad signature).");
            audit_log_event("driver-refused", w1);
            return;
        }
        int rc = drv_load(w1, (const uint8_t *)pkg, plen);
        if (rc != 0) {
            serial_write("driver: load failed (");
            print_u64_serial((uint64_t)(-(int64_t)rc));
            serial_write_line(").");
            audit_log_event("driver-failed", w1);
            return;
        }
        serial_write_line("driver: installed.");
        audit_log_event("driver-install", w1);
        return;
    }
    if (starts_with_word(arg, "disable")) {
        if (!driver_admin_gate()) {
            return;
        }
        p = skip_spaces(arg + 7);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0) {
            serial_write_line("Usage: driver disable <name>");
            return;
        }
        drv_set_disabled(w1, 1);
        return;
    }
    if (starts_with_word(arg, "enable")) {
        if (!driver_admin_gate()) {
            return;
        }
        p = skip_spaces(arg + 6);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0) {
            serial_write_line("Usage: driver enable <name>");
            return;
        }
        drv_set_disabled(w1, 0);
        return;
    }
    if (strcmp(arg, "list") == 0 || arg[0] == '\0') {
        int n = drv_loaded_count();
        serial_write("driver: loaded ");
        print_u64_serial((uint64_t)n);
        serial_write_line(":");
        for (int i = 0; i < n; i++) {
            serial_write("  ");
            serial_write_line(drv_loaded_name(i));
        }
        size_t len = 0;
        const char *dis = vfs_read_ptr("drivers.disabled", &len);
        if (dis != NULL && len > 0) {
            serial_write_line("driver: disabled list:");
            static char dbuf[1024];
            size_t m = len < sizeof(dbuf) - 1 ? len : sizeof(dbuf) - 1;
            for (size_t i = 0; i < m; i++) {
                dbuf[i] = dis[i];
            }
            dbuf[m] = '\0';
            serial_write_line(dbuf);
        }
        return;
    }
    serial_write_line("Usage: driver rootkey | sign <pkg> | "
                      "install <pkg> | list | disable <name> | "
                      "enable <name>");
}

static void pq_flow(const char *line) {
    const char *arg = skip_spaces(line + 2); /* skip "pq" */
    char w1[64], w2[64];
    const char *p = arg;
    if (strcmp(arg, "keygen") == 0) {
        uint8_t d[32], z[32];
        entropy_bytes(d, 32);
        entropy_bytes(z, 32);
        if (mlkem768_keygen(d, z, pq_ek, pq_dk) != 0) {
            serial_write_line("pq: key generation failed.");
            return;
        }
        if (vfs_write("pqkem.pub", (const char *)pq_ek,
                      MLKEM768_EK_LEN) != 0 ||
            vfs_write("pqkem.key", (const char *)pq_dk,
                      MLKEM768_DK_LEN) != 0) {
            serial_write_line("pq: could not store the key pair.");
            return;
        }
        serial_write_line(
            "pq: ML-KEM-768 key pair stored (pqkem.pub / pqkem.key).");
        return;
    }
    if (strcmp(arg, "signkey") == 0) {
        uint8_t seed[MLDSA44_SEED_LEN];
        entropy_bytes(seed, sizeof(seed));
        if (mldsa44_keygen_from_seed(seed, pq_pk, pq_sk) != 0) {
            serial_write_line("pq: key generation failed.");
            return;
        }
        if (vfs_write("pqsign.pub", (const char *)pq_pk,
                      MLDSA44_PK_LEN) != 0 ||
            vfs_write("pqsign.key", (const char *)pq_sk,
                      MLDSA44_SK_LEN) != 0) {
            serial_write_line("pq: could not store the key pair.");
            return;
        }
        serial_write_line(
            "pq: ML-DSA-44 key pair stored (pqsign.pub / pqsign.key).");
        return;
    }
    if (starts_with_word(arg, "encaps")) {
        p = skip_spaces(arg + 6);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0 ||
            pq_next_word(&p, w2, sizeof(w2)) != 0) {
            serial_write_line("Usage: pq encaps <pub-file> <ct-file>");
            return;
        }
        size_t len = 0;
        if (pq_read_file(w1, pq_ek, sizeof(pq_ek), &len) != 0 ||
            len != MLKEM768_EK_LEN) {
            serial_write_line("pq: not an ML-KEM-768 public key file.");
            return;
        }
        uint8_t m[32];
        uint8_t ss[MLKEM_SS_LEN];
        entropy_bytes(m, 32);
        if (mlkem768_encaps(pq_ek, m, pq_ct, ss) != 0) {
            serial_write_line("pq: encapsulation failed.");
            return;
        }
        if (vfs_write(w2, (const char *)pq_ct, MLKEM768_CT_LEN) != 0) {
            serial_write_line("pq: could not store the ciphertext.");
            return;
        }
        pq_derive_session(ss);
        serial_write_line("pq: ciphertext stored; session key derived.");
        pq_print_fingerprint();
        return;
    }
    if (starts_with_word(arg, "decaps")) {
        p = skip_spaces(arg + 6);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0) {
            serial_write_line("Usage: pq decaps <ct-file>");
            return;
        }
        size_t len = 0;
        if (pq_read_file("pqkem.key", pq_dk, sizeof(pq_dk), &len) != 0 ||
            len != MLKEM768_DK_LEN) {
            serial_write_line("pq: no key pair (run `pq keygen`).");
            return;
        }
        if (pq_read_file(w1, pq_ct, sizeof(pq_ct), &len) != 0 ||
            len != MLKEM768_CT_LEN) {
            serial_write_line("pq: not an ML-KEM-768 ciphertext file.");
            return;
        }
        uint8_t ss[MLKEM_SS_LEN];
        if (mlkem768_decaps(pq_dk, pq_ct, ss) != 0) {
            serial_write_line("pq: decapsulation failed.");
            return;
        }
        pq_derive_session(ss);
        serial_write_line("pq: session key derived.");
        pq_print_fingerprint();
        return;
    }
    if (starts_with_word(arg, "seal")) {
        p = skip_spaces(arg + 4);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0 ||
            pq_next_word(&p, w2, sizeof(w2)) != 0) {
            serial_write_line("Usage: pq seal <in> <out>");
            return;
        }
        if (!pq_session_valid) {
            serial_write_line(
                "pq: no session key (run pq encaps/decaps first).");
            return;
        }
        size_t len = 0;
        if (pq_read_file(w1, pq_data, sizeof(pq_data), &len) != 0) {
            serial_write_line("pq: cannot read the input file.");
            return;
        }
        /* 形式: "PQS1" || IV(16) || 暗号文 || HMAC(32)。 */
        uint8_t *o = pq_sealed;
        o[0] = 'P';
        o[1] = 'Q';
        o[2] = 'S';
        o[3] = '1';
        entropy_bytes(o + 4, 16);
        crypto_aes256_ctr(pq_session_key, o + 4, pq_data, o + 20, len);
        pq_hmac_sha256(pq_session_key, 32, o + 4, 16 + len,
                       o + 20 + len);
        if (vfs_write(w2, (const char *)o, 20 + len + 32) != 0) {
            serial_write_line("pq: could not store the sealed file.");
            return;
        }
        serial_write_line("pq: sealed.");
        return;
    }
    if (starts_with_word(arg, "open")) {
        p = skip_spaces(arg + 4);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0 ||
            pq_next_word(&p, w2, sizeof(w2)) != 0) {
            serial_write_line("Usage: pq open <in> <out>");
            return;
        }
        if (!pq_session_valid) {
            serial_write_line(
                "pq: no session key (run pq encaps/decaps first).");
            return;
        }
        size_t len = 0;
        if (pq_read_file(w1, pq_sealed, sizeof(pq_sealed), &len) != 0 ||
            len < 52 || pq_sealed[0] != 'P' || pq_sealed[1] != 'Q' ||
            pq_sealed[2] != 'S' || pq_sealed[3] != '1') {
            serial_write_line("pq: not a sealed file.");
            return;
        }
        size_t body = len - 52; /* IV(16) + tag(32) を除く */
        uint8_t tag[32];
        pq_hmac_sha256(pq_session_key, 32, pq_sealed + 4, 16 + body,
                       tag);
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) {
            diff |= (uint8_t)(tag[i] ^ pq_sealed[20 + body + i]);
        }
        if (diff != 0) {
            serial_write_line(
                "pq: authentication failed (wrong key or tampered).");
            return;
        }
        crypto_aes256_ctr(pq_session_key, pq_sealed + 4,
                          pq_sealed + 20, pq_data, body);
        if (vfs_write(w2, (const char *)pq_data, body) != 0) {
            serial_write_line("pq: could not store the opened file.");
            return;
        }
        serial_write_line("pq: opened.");
        return;
    }
    if (starts_with_word(arg, "sign")) {
        p = skip_spaces(arg + 4);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0) {
            serial_write_line("Usage: pq sign <file>");
            return;
        }
        size_t len = 0;
        if (pq_read_file("pqsign.key", pq_sk, sizeof(pq_sk), &len) != 0 ||
            len != MLDSA44_SK_LEN) {
            serial_write_line("pq: no signing key (run `pq signkey`).");
            return;
        }
        if (pq_read_file(w1, pq_data, sizeof(pq_data), &len) != 0) {
            serial_write_line("pq: cannot read the file.");
            return;
        }
        if (mldsa44_sign(pq_sk, pq_data, len, pq_sig) != 0) {
            serial_write_line("pq: signing failed.");
            return;
        }
        char name[80];
        size_t n = 0;
        while (w1[n] != '\0' && n + 5 < sizeof(name)) {
            name[n] = w1[n];
            n++;
        }
        const char *suf = ".sig";
        while (*suf != '\0' && n + 1 < sizeof(name)) {
            name[n++] = *suf++;
        }
        name[n] = '\0';
        if (vfs_write(name, (const char *)pq_sig, MLDSA44_SIG_LEN) != 0) {
            serial_write_line("pq: could not store the signature.");
            return;
        }
        serial_write_line("pq: signed.");
        return;
    }
    if (starts_with_word(arg, "verify")) {
        p = skip_spaces(arg + 6);
        if (pq_next_word(&p, w1, sizeof(w1)) != 0) {
            serial_write_line("Usage: pq verify <file>");
            return;
        }
        size_t len = 0;
        if (pq_read_file("pqsign.pub", pq_pk, sizeof(pq_pk), &len) != 0 ||
            len != MLDSA44_PK_LEN) {
            serial_write_line("pq: no public key (run `pq signkey`).");
            return;
        }
        char name[80];
        size_t n = 0;
        while (w1[n] != '\0' && n + 5 < sizeof(name)) {
            name[n] = w1[n];
            n++;
        }
        const char *suf = ".sig";
        while (*suf != '\0' && n + 1 < sizeof(name)) {
            name[n++] = *suf++;
        }
        name[n] = '\0';
        if (pq_read_file(name, pq_sig, sizeof(pq_sig), &len) != 0 ||
            len != MLDSA44_SIG_LEN) {
            serial_write_line("pq: no signature file.");
            return;
        }
        if (pq_read_file(w1, pq_data, sizeof(pq_data), &len) != 0) {
            serial_write_line("pq: cannot read the file.");
            return;
        }
        if (mldsa44_verify(pq_pk, pq_data, len, pq_sig) == 0) {
            serial_write_line("pq: signature OK.");
        } else {
            serial_write_line("pq: signature INVALID.");
        }
        return;
    }
    serial_write_line(
        "Usage: pq keygen|encaps <pub> <ct>|decaps <ct>|seal <in> "
        "<out>|open <in> <out>|signkey|sign <file>|verify <file>");
}

/* ---- mail ---- */

#define MAIL_CONF_FILE "mail.conf"

/* Load "mail.conf" into cfg (defaults when missing).
 * A wrapped password (pass_enc) is unwrapped with the
 * disk key when it is active; without the key the
 * password stays empty and sending/receiving will fail
 * authentication, never silently use a wrong secret. */
static void load_mail_config(mail_config *cfg) {
    mail_config_defaults(cfg);
    size_t len = 0;
    const char *text = vfs_read_ptr(MAIL_CONF_FILE, &len);
    if (text != NULL) {
        mail_config_parse(cfg, text, len);
    }
    if (cfg->pass_enc[0] != '\0') {
        char plain[64];
        if (persist_unwrap_secret(cfg->pass_enc, plain,
                                  sizeof(plain)) == 0) {
            size_t i = 0;
            while (plain[i] != '\0' && i + 1 < sizeof(cfg->pass)) {
                cfg->pass[i] = plain[i];
                i++;
            }
            cfg->pass[i] = '\0';
        }
    }
}

static void save_mail_config(const mail_config *cfg) {
    mail_config tmp = *cfg;
    /* With the disk key active, store the password
     * wrapped, not plain. The wrap is recomputed on
     * every save (fresh IV), which also migrates older
     * plain-text configs on the next save. */
    if (tmp.pass[0] != '\0' && persist_key_active()) {
        if (persist_wrap_secret(tmp.pass, tmp.pass_enc,
                                sizeof(tmp.pass_enc)) == 0) {
            tmp.pass[0] = '\0';
        }
    }
    char buf[1024];
    size_t n = mail_config_serialize(&tmp, buf, sizeof(buf));
    if (vfs_write(MAIL_CONF_FILE, buf, n) != 0) {
        serial_write_line("mail: could not save mail.conf.");
    }
}

/* Next free message number (max existing + 1), or -1
 * when the box is full. */
static int mail_next_number(void) {
    int count = vfs_file_count();
    if (count >= PRISM_VFS_MAX_FILES) {
        return -1;
    }
    int max = 0;
    for (int i = 0; i < count; i++) {
        int n = mail_index_from_name(vfs_file_name(i));
        if (n > max) {
            max = n;
        }
    }
    if (max >= 9999) {
        return -1;
    }
    return max + 1;
}

/* POP3 fetch callback: store one message as the next
 * mail-NNNN.txt file. */
static int mail_store_cb(const char *msg, size_t len, void *ctx) {
    int *stored = (int *)ctx;
    int n = mail_next_number();
    if (n < 0) {
        serial_write_line("mail: mailbox full, message skipped.");
        return -1;
    }
    char name[16];
    if (mail_file_name(n, name, sizeof(name)) != 0) {
        return -1;
    }
    if (vfs_write(name, msg, len) != 0) {
        serial_write_line("mail: could not store message.");
        return -1;
    }
    (void)mail_save_attachments(msg, len);
    (*stored)++;
    return 0;
}

/* Folder helpers over mail-folders.txt. */
static char mail_folders_buf[2048];

static int mail_folders_load(void) {
    size_t len = 0;
    const char *t = vfs_read_ptr("mail-folders.txt", &len);
    if (t == NULL || len >= sizeof(mail_folders_buf)) {
        mail_folders_buf[0] = '\0';
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        mail_folders_buf[i] = t[i];
    }
    mail_folders_buf[len] = '\0';
    return (int)len;
}

static void mail_assign_folder(int n, const char *folder) {
    int len = mail_folders_load();
    static char out[2048];
    int w = mail_folder_set(mail_folders_buf, (size_t)len, n,
                            folder, out, sizeof(out));
    if (w > 0) {
        (void)vfs_write("mail-folders.txt", out, (size_t)w);
        persist_maybe_sync();
    }
}

static void mail_folder_of_n(int n, char *out, size_t cap) {
    int len = mail_folders_load();
    if (mail_folder_of(mail_folders_buf, (size_t)len, n, out,
                       cap) != 0) {
        out[0] = '\0';
    }
}

/* List the messages: "N  From  Subject". */
static void mail_list(void) {
    int count = vfs_file_count();
    int found = 0;
    for (int i = 0; i < count; i++) {
        const char *name = vfs_file_name(i);
        int n = mail_index_from_name(name);
        if (n < 0) {
            continue;
        }
        size_t len = 0;
        const char *text = vfs_read_ptr(name, &len);
        if (text == NULL) {
            continue;
        }
        char from[64];
        char subject[96];
        char raw[128];
        if (mail_parse_header(text, "From", raw, sizeof(raw)) != 0) {
            raw[0] = '\0';
        }
        mime_decode_header(raw, from, sizeof(from));
        if (mail_parse_header(text, "Subject", raw,
                              sizeof(raw)) != 0) {
            raw[0] = '\0';
        }
        mime_decode_header(raw, subject, sizeof(subject));
        found++;
        serial_write("  ");
        print_u64_serial((uint64_t)n);
        serial_write("  ");
        serial_write(from);
        serial_write("  ");
        serial_write(subject);
        {
            char folder[24];
            mail_folder_of_n(n, folder, sizeof(folder));
            if (folder[0] != '\0' &&
                !(folder[0] == 'I' && folder[1] == 'n' &&
                  folder[2] == 'b' && folder[3] == 'o' &&
                  folder[4] == 'x' && folder[5] == '\0')) {
                serial_write("  [");
                serial_write(folder);
                serial_write("]");
            }
        }
        serial_write_line("");
    }
    /* Sent copies (sent-NNNN.txt). */
    for (int i = 0; i < count; i++) {
        const char *name = vfs_file_name(i);
        if (!(name[0] == 's' && name[1] == 'e' && name[2] == 'n' &&
              name[3] == 't' && name[4] == '-')) {
            continue;
        }
        int n = 0;
        int digits = 0;
        for (int k = 5; name[k] >= '0' && name[k] <= '9'; k++) {
            n = n * 10 + (name[k] - '0');
            digits++;
        }
        if (digits == 0) {
            continue;
        }
        size_t len = 0;
        const char *text = vfs_read_ptr(name, &len);
        if (text == NULL) {
            continue;
        }
        char to[64];
        char subject[96];
        char raw[128];
        if (mail_parse_header(text, "To", raw, sizeof(raw)) != 0) {
            raw[0] = '\0';
        }
        mime_decode_header(raw, to, sizeof(to));
        if (mail_parse_header(text, "Subject", raw,
                              sizeof(raw)) != 0) {
            raw[0] = '\0';
        }
        mime_decode_header(raw, subject, sizeof(subject));
        found++;
        serial_write("  S");
        print_u64_serial((uint64_t)n);
        serial_write("  ");
        serial_write(to);
        serial_write("  ");
        serial_write(subject);
        serial_write_line("  [Sent]");
    }
    if (found == 0) {
        serial_write_line("No mail.");
    }
}

/* Split the next space-separated word out of *p. */
static int mail_next_word(const char **p, char *out, size_t cap) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        *p = s;
        return -1;
    }
    size_t n = 0;
    while (*s != '\0' && *s != ' ' && *s != '\t') {
        if (n + 1 < cap) {
            out[n++] = *s;
        }
        s++;
    }
    out[n] = '\0';
    *p = s;
    return 0;
}

/* ---------- clock: date / tz / ntp ---------- */

static void tz_format(int tz, char *out) {
    /* "UTC+09:00" / "UTC-05:30" / "UTC+00:00" */
    size_t p = 0;
    out[p++] = 'U';
    out[p++] = 'T';
    out[p++] = 'C';
    out[p++] = tz < 0 ? '-' : '+';
    int v = tz < 0 ? -tz : tz;
    int hh = v / 60;
    int mm = v % 60;
    out[p++] = (char)('0' + (hh / 10) % 10);
    out[p++] = (char)('0' + hh % 10);
    out[p++] = ':';
    out[p++] = (char)('0' + (mm / 10) % 10);
    out[p++] = (char)('0' + mm % 10);
    out[p] = '\0';
}

static void clock_show(void) {
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        serial_write_line("clock: RTC read failed.");
        return;
    }
    char date[16];
    rtc_format_date(&t, date);
    serial_write(date);
    serial_write(" ");
    char hhmm[8];
    rtc_format_hhmm(&t, hhmm);
    serial_write(hhmm);
    serial_write(":");
    char ss[3];
    ss[0] = (char)('0' + (t.second / 10) % 10);
    ss[1] = (char)('0' + t.second % 10);
    ss[2] = '\0';
    serial_write(ss);
    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed",
                                 "Thu", "Fri", "Sat" };
    serial_write(" (");
    int w = rtc_weekday(2000 + t.year, t.month, t.day);
    serial_write(wd[w]);
    serial_write(") ");
    char tzw[12];
    tz_format(shell_settings.tz_offset, tzw);
    serial_write_line(tzw);
}

static int parse_fixed_int(const char *s, int digits, int *out) {
    int v = 0;
    for (int i = 0; i < digits; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

/* `date`, `date set YYYY-MM-DD HH:MM:SS`, `tz [+HHMM|
 * minutes]`, `ntp [server]`. */
static void clock_flow(const char *line) {
    if (starts_with_word(line, "tz")) {
        const char *arg = skip_spaces(line + 2);
        if (arg[0] == '\0') {
            char tzw[12];
            tz_format(shell_settings.tz_offset, tzw);
            serial_write("timezone: ");
            serial_write_line(tzw);
            return;
        }
        int sign = 1;
        const char *p = arg;
        if (*p == '+') {
            p++;
        } else if (*p == '-') {
            sign = -1;
            p++;
        }
        int v = 0;
        int digits = 0;
        while (p[digits] >= '0' && p[digits] <= '9') {
            digits++;
        }
        int num = 0;
        if (parse_fixed_int(p, digits, &num) != 0 || digits == 0) {
            serial_write_line("Usage: tz [+HHMM | minutes]");
            return;
        }
        int minutes;
        if (digits == 4) {
            minutes = (num / 100) * 60 + (num % 100);
        } else {
            minutes = num;
        }
        minutes *= sign;
        if (minutes < -720 || minutes > 840) {
            serial_write_line("tz: offset out of range.");
            return;
        }
        prism_settings s;
        load_settings(&s);
        s.tz_offset = minutes;
        save_settings(&s);
        shell_settings.tz_offset = minutes;
        char tzw[12];
        tz_format(minutes, tzw);
        serial_write("timezone set: ");
        serial_write_line(tzw);
        return;
    }
    if (starts_with_word(line, "ntp")) {
        const char *arg = skip_spaces(line + 3);
        char host[64];
        size_t hp = 0;
        while (arg[hp] != '\0' && arg[hp] != ' ' &&
               hp + 1 < sizeof(host)) {
            host[hp] = arg[hp];
            hp++;
        }
        host[hp] = '\0';
        if (host[0] == '\0') {
            const char *d = "pool.ntp.org";
            size_t i = 0;
            while (d[i] != '\0') {
                host[i] = d[i];
                i++;
            }
            host[i] = '\0';
        }
        /* NTP での時刻変更も管理者 PIN を必須にする
         * (date set と同じゲート)。 */
        if (!admin_check()) {
            return;
        }
        uint8_t ip[4];
        if (net_resolve(host, ip) != 0) {
            serial_write_line("ntp: cannot resolve server.");
            return;
        }
        uint16_t port = 123;
        {
            /* Optional second word: the port. */
            const char *p2 = arg + hp;
            while (*p2 == ' ') {
                p2++;
            }
            int v = 0;
            int any = 0;
            while (*p2 >= '0' && *p2 <= '9') {
                v = v * 10 + (*p2 - '0');
                p2++;
                any = 1;
            }
            if (any && v > 0 && v < 65536) {
                port = (uint16_t)v;
            }
        }
        uint8_t req[48];
        for (int i = 0; i < 48; i++) {
            req[i] = 0;
        }
        req[0] = 0x23; /* LI=0, VN=4, Mode=3 (client) */
        uint8_t resp[48];
        int n = net_udp_exchange(ip, port, 49123, req,
                                 sizeof(req), resp,
                                 sizeof(resp), 5);
        if (n < 48) {
            serial_write_line("ntp: no reply from server.");
            return;
        }
        uint32_t secs = ((uint32_t)resp[40] << 24) |
                        ((uint32_t)resp[41] << 16) |
                        ((uint32_t)resp[42] << 8) |
                        (uint32_t)resp[43];
        prism_rtc_time t;
        if (rtc_from_ntp(secs, shell_settings.tz_offset,
                         &t) != 0) {
            serial_write_line("ntp: bad timestamp.");
            return;
        }
        if (rtc_write(&t) != 0) {
            serial_write_line("ntp: RTC write failed.");
            return;
        }
        serial_write("ntp: clock set from ");
        serial_write_line(host);
        clock_show();
        return;
    }
    /* date [set ...] */
    const char *arg = skip_spaces(line + 4);
    if (arg[0] == '\0') {
        clock_show();
        return;
    }
    if (starts_with_word(arg, "set")) {
        /* 時計設定は管理者 PIN を必須にする (GUI の時計
         * ページと同じゲート。深層チェック L6 の統一)。 */
        if (!admin_check()) {
            return;
        }
        const char *p = skip_spaces(arg + 3);
        /* YYYY-MM-DD HH:MM:SS (fixed width) */
        int year, mon, day, hh, mm, ss;
        if (parse_fixed_int(p, 4, &year) != 0 || p[4] != '-' ||
            parse_fixed_int(p + 5, 2, &mon) != 0 || p[7] != '-' ||
            parse_fixed_int(p + 8, 2, &day) != 0 ||
            p[10] != ' ' ||
            parse_fixed_int(p + 11, 2, &hh) != 0 ||
            p[13] != ':' ||
            parse_fixed_int(p + 14, 2, &mm) != 0 ||
            p[16] != ':' ||
            parse_fixed_int(p + 17, 2, &ss) != 0) {
            serial_write_line("Usage: date set YYYY-MM-DD HH:MM:SS");
            return;
        }
        if (year < 2000 || year > 2099 || mon < 1 || mon > 12 ||
            day < 1 || day > rtc_days_in_month(year, mon) ||
            hh > 23 || mm > 59 || ss > 59) {
            serial_write_line("date: value out of range.");
            return;
        }
        prism_rtc_time t;
        t.year = (uint8_t)(year - 2000);
        t.month = (uint8_t)mon;
        t.day = (uint8_t)day;
        t.hour = (uint8_t)hh;
        t.minute = (uint8_t)mm;
        t.second = (uint8_t)ss;
        t.weekday = (uint8_t)rtc_weekday(year, mon, day);
        if (rtc_write(&t) != 0) {
            serial_write_line("date: RTC write failed.");
            return;
        }
        serial_write_line("clock: set.");
        clock_show();
        return;
    }
    serial_write_line("Usage: date [set YYYY-MM-DD HH:MM:SS]");
}

static void mail_flow(const char *line) {
    const char *arg = skip_spaces(line + 4); /* skip "mail" */
    if (arg[0] == '\0') {
        mail_list();
        return;
    }
    if (strcmp(arg, "config") == 0) {
        mail_config cfg;
        load_mail_config(&cfg);
        serial_write("mail: smtp=");
        serial_write(cfg.smtp_host);
        serial_write(":");
        print_u64_serial(cfg.smtp_port != 0 ? cfg.smtp_port : 25);
        serial_write(" pop3=");
        serial_write(cfg.pop3_host);
        serial_write(":");
        print_u64_serial(cfg.pop3_port != 0 ? cfg.pop3_port : 110);
        serial_write(" user=");
        serial_write(cfg.user);
        serial_write(" from=");
        serial_write_line(cfg.from);
        serial_write("mail: imap=");
        serial_write(cfg.imap_host[0] != '\0' ? cfg.imap_host : "-");
        serial_write(":");
        print_u64_serial(cfg.imap_port != 0 ? cfg.imap_port
                                            : (cfg.imap_tls ? 993 : 143));
        serial_write(" tls smtp=");
        serial_write(cfg.smtp_tls ? "on" : "off");
        serial_write(" pop3=");
        serial_write(cfg.pop3_tls ? "on" : "off");
        serial_write(" imap=");
        serial_write(cfg.imap_tls ? "on" : "off");
        serial_write(" pin=");
        serial_write_line(cfg.cert_pin[0] != '\0' ? "set" : "unset");
        return;
    }
    if (starts_with_word(arg, "imap")) {
        const char *p = skip_spaces(arg + 4);
        mail_config cfg;
        load_mail_config(&cfg);
        char w[96];
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            serial_write_line("Usage: mail imap <host> <port>");
            return;
        }
        if (strcmp(w, "off") == 0) {
            cfg.imap_host[0] = '\0';
            cfg.imap_port = 0;
            save_mail_config(&cfg);
            serial_write_line("mail: IMAP off (POP3 is used).");
            return;
        }
        size_t n = 0;
        while (w[n] != '\0' && n + 1 < sizeof(cfg.imap_host)) {
            cfg.imap_host[n] = w[n];
            n++;
        }
        cfg.imap_host[n] = '\0';
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            serial_write_line("Usage: mail imap <host> <port>");
            return;
        }
        int v = 0;
        for (int i = 0; w[i] != '\0'; i++) {
            if (w[i] < '0' || w[i] > '9') {
                serial_write_line("Usage: mail imap <host> <port>");
                return;
            }
            v = v * 10 + (w[i] - '0');
        }
        if (v <= 0 || v > 65535) {
            serial_write_line("Usage: mail imap <host> <port>");
            return;
        }
        cfg.imap_port = (uint16_t)v;
        save_mail_config(&cfg);
        serial_write_line("mail: IMAP server set.");
        return;
    }
    if (starts_with_word(arg, "tls")) {
        const char *p = skip_spaces(arg + 3);
        char w[16];
        if (mail_next_word(&p, w, sizeof(w)) != 0 ||
            (strcmp(w, "on") != 0 && strcmp(w, "off") != 0)) {
            serial_write_line("Usage: mail tls <on|off>");
            return;
        }
        mail_config cfg;
        load_mail_config(&cfg);
        int on = strcmp(w, "on") == 0;
        cfg.smtp_tls = on;
        cfg.pop3_tls = on;
        cfg.imap_tls = on;
        save_mail_config(&cfg);
        serial_write_line(on ? "mail: TLS on for smtp/pop3/imap."
                             : "mail: TLS off.");
        return;
    }
    if (starts_with_word(arg, "pin")) {
        const char *p = skip_spaces(arg + 3);
        char w[80];
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            serial_write_line(
                "Usage: mail pin <64 hex chars|off>");
            return;
        }
        mail_config cfg;
        load_mail_config(&cfg);
        if (strcmp(w, "off") == 0) {
            cfg.cert_pin[0] = '\0';
            save_mail_config(&cfg);
            serial_write_line("mail: certificate pin cleared.");
            return;
        }
        size_t n = 0;
        while (w[n] != '\0') {
            n++;
        }
        int ok = n == 64;
        for (size_t i = 0; ok && i < n; i++) {
            char ch = w[i];
            if (!((ch >= '0' && ch <= '9') ||
                  (ch >= 'a' && ch <= 'f') ||
                  (ch >= 'A' && ch <= 'F'))) {
                ok = 0;
            }
        }
        if (!ok) {
            serial_write_line(
                "mail: pin must be 64 hex characters "
                "(SHA-256 of the server certificate).");
            return;
        }
        for (size_t i = 0; i < 64; i++) {
            char ch = w[i];
            if (ch >= 'A' && ch <= 'F') {
                ch = (char)(ch - 'A' + 'a');
            }
            cfg.cert_pin[i] = ch;
        }
        cfg.cert_pin[64] = '\0';
        save_mail_config(&cfg);
        serial_write_line("mail: certificate pin set.");
        return;
    }
    if (starts_with_word(arg, "setup")) {
        const char *p = skip_spaces(arg + 5);
        mail_config cfg;
        mail_config_defaults(&cfg);
        char w[96];
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            goto setup_usage;
        }
        {
            size_t n = 0;
            while (w[n] != '\0' && n + 1 < sizeof(cfg.smtp_host)) {
                cfg.smtp_host[n] = w[n];
                n++;
            }
            cfg.smtp_host[n] = '\0';
        }
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            goto setup_usage;
        }
        {
            int v = 0;
            for (int i = 0; w[i] != '\0'; i++) {
                if (w[i] < '0' || w[i] > '9') {
                    goto setup_usage;
                }
                v = v * 10 + (w[i] - '0');
            }
            if (v <= 0 || v > 65535) {
                goto setup_usage;
            }
            cfg.smtp_port = (uint16_t)v;
        }
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            goto setup_usage;
        }
        {
            size_t n = 0;
            while (w[n] != '\0' && n + 1 < sizeof(cfg.pop3_host)) {
                cfg.pop3_host[n] = w[n];
                n++;
            }
            cfg.pop3_host[n] = '\0';
        }
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            goto setup_usage;
        }
        {
            int v = 0;
            for (int i = 0; w[i] != '\0'; i++) {
                if (w[i] < '0' || w[i] > '9') {
                    goto setup_usage;
                }
                v = v * 10 + (w[i] - '0');
            }
            if (v <= 0 || v > 65535) {
                goto setup_usage;
            }
            cfg.pop3_port = (uint16_t)v;
        }
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            goto setup_usage;
        }
        {
            size_t n = 0;
            while (w[n] != '\0' && n + 1 < sizeof(cfg.user)) {
                cfg.user[n] = w[n];
                n++;
            }
            cfg.user[n] = '\0';
        }
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            goto setup_usage;
        }
        {
            size_t n = 0;
            while (w[n] != '\0' && n + 1 < sizeof(cfg.pass)) {
                cfg.pass[n] = w[n];
                n++;
            }
            cfg.pass[n] = '\0';
        }
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            goto setup_usage;
        }
        {
            size_t n = 0;
            while (w[n] != '\0' && n + 1 < sizeof(cfg.from)) {
                cfg.from[n] = w[n];
                n++;
            }
            cfg.from[n] = '\0';
        }
        save_mail_config(&cfg);
        serial_write_line("mail: server configured.");
        return;
    setup_usage:
        serial_write_line("Usage: mail setup <smtp_host> <smtp_port> <pop3_host> <pop3_port> <user> <pass> <from>");
        return;
    }
    if (strcmp(arg, "recv") == 0) {
        mail_config cfg;
        load_mail_config(&cfg);
        if (!mail_config_ready(&cfg)) {
            serial_write_line("mail: no mail server configured.");
            serial_write_line("Set it with `mail setup <smtp_host> <smtp_port> <pop3_host> <pop3_port> <user> <pass> <from>`.");
            return;
        }
        int stored = 0;
        int got;
        if (cfg.imap_host[0] != '\0') {
            got = mail_imap_fetch(&cfg, mail_store_cb, &stored);
        } else {
            got = mail_pop3_fetch(&cfg, mail_store_cb, &stored);
        }
        if (got < 0) {
            serial_write_line("mail: receive failed.");
            return;
        }
        if (got == 0) {
            serial_write_line("mail: no new messages.");
        }
        return;
    }
    if (starts_with_word(arg, "send")) {
        const char *p = skip_spaces(arg + 4);
        mail_config cfg;
        load_mail_config(&cfg);
        if (!mail_config_ready(&cfg)) {
            serial_write_line("mail: no mail server configured.");
            serial_write_line("Set it with `mail setup <smtp_host> <smtp_port> <pop3_host> <pop3_port> <user> <pass> <from>`.");
            return;
        }
        char to[96];
        if (mail_next_word(&p, to, sizeof(to)) != 0) {
            serial_write_line("Usage: mail send <to> <subject> <body...>");
            return;
        }
        char subject[96];
        if (mail_next_word(&p, subject, sizeof(subject)) != 0) {
            serial_write_line("Usage: mail send <to> <subject> <body...>");
            return;
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            serial_write_line("Usage: mail send <to> <subject> <body...>");
            return;
        }
        {
            static char built[PRISM_VFS_MAX_FILE_SIZE];
            int blen = 0;
            if (mail_smtp_send_att(&cfg, to, subject, p, NULL,
                                   built, sizeof(built),
                                   &blen) != 0) {
                serial_write_line("mail: send failed.");
            } else if (blen > 0 &&
                       mail_archive_sent(built, (size_t)blen) ==
                           0) {
                persist_maybe_sync();
            }
        }
        return;
    }
    if (starts_with_word(arg, "read")) {
        const char *p = skip_spaces(arg + 4);
        char w[16];
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            serial_write_line("Usage: mail read <number>");
            return;
        }
        int n = 0;
        for (int i = 0; w[i] != '\0'; i++) {
            if (w[i] < '0' || w[i] > '9') {
                serial_write_line("Usage: mail read <number>");
                return;
            }
            n = n * 10 + (w[i] - '0');
        }
        char name[16];
        if (mail_file_name(n, name, sizeof(name)) != 0 ||
            !vfs_exists(name)) {
            serial_write_line("mail: no such message.");
            return;
        }
        size_t len = 0;
        const char *text = vfs_read_ptr(name, &len);
        if (text == NULL) {
            serial_write_line("mail: no such message.");
            return;
        }
        {
            char raw[128];
            char dec[128];
            if (mail_parse_header(text, "From", raw,
                                  sizeof(raw)) == 0) {
                mime_decode_header(raw, dec, sizeof(dec));
                serial_write("From: ");
                serial_write_line(dec);
            }
            if (mail_parse_header(text, "Subject", raw,
                                  sizeof(raw)) == 0) {
                mime_decode_header(raw, dec, sizeof(dec));
                serial_write("Subject: ");
                serial_write_line(dec);
            }
            if (mail_parse_header(text, "Date", raw,
                                  sizeof(raw)) == 0 &&
                raw[0] != '\0') {
                serial_write("Date: ");
                serial_write_line(raw);
            }
            static char body[PRISM_VFS_MAX_FILE_SIZE];
            int bn = mime_text_body(text, len, body,
                                    sizeof(body) - 1);
            if (bn >= 0) {
                body[bn] = '\0';
                serial_write_line("---");
                serial_write(body);
                if (bn > 0 && body[bn - 1] != '\n') {
                    serial_write_char('\n');
                }
            } else {
                serial_write(text);
                if (len > 0 && text[len - 1] != '\n') {
                    serial_write_char('\n');
                }
            }
        }
        return;
    }
    if (starts_with_word(arg, "folders")) {
        int len = mail_folders_load();
        serial_write_line("Folders: Inbox (default), Sent "
                          "(sent-*.txt files), plus:");
        size_t i = 0;
        int any = 0;
        while (i < (size_t)len) {
            size_t ls = i;
            while (i < (size_t)len &&
                   mail_folders_buf[i] != '\n') {
                i++;
            }
            /* print "n=folder" lines' folder part */
            for (size_t k = ls; k < i; k++) {
                if (mail_folders_buf[k] == '=') {
                    serial_write("  ");
                    for (size_t m = k + 1; m < i; m++) {
                        serial_write_char(mail_folders_buf[m]);
                    }
                    serial_write_line("");
                    any = 1;
                    break;
                }
            }
            i++;
        }
        if (!any) {
            serial_write_line("  (no custom folders yet)");
        }
        return;
    }
    if (starts_with_word(arg, "move")) {
        const char *p = skip_spaces(arg + 4);
        char w[16];
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            serial_write_line("Usage: mail move <number> "
                              "<folder>");
            return;
        }
        int n = 0;
        for (int i = 0; w[i] != '\0'; i++) {
            if (w[i] < '0' || w[i] > '9') {
                serial_write_line("Usage: mail move <number> "
                                  "<folder>");
                return;
            }
            n = n * 10 + (w[i] - '0');
        }
        char folder[24];
        if (mail_next_word(&p, folder, sizeof(folder)) != 0) {
            serial_write_line("Usage: mail move <number> "
                              "<folder>");
            return;
        }
        char name[16];
        if (mail_file_name(n, name, sizeof(name)) != 0 ||
            !vfs_exists(name)) {
            serial_write_line("mail: no such message.");
            return;
        }
        mail_assign_folder(n, folder);
        serial_write("mail: message ");
        print_u64_serial((uint64_t)n);
        serial_write(" moved to ");
        serial_write_line(folder);
        return;
    }
    if (starts_with_word(arg, "del")) {
        const char *p = skip_spaces(arg + 3);
        char w[16];
        if (mail_next_word(&p, w, sizeof(w)) != 0) {
            serial_write_line("Usage: mail del <number>");
            return;
        }
        int n = 0;
        for (int i = 0; w[i] != '\0'; i++) {
            if (w[i] < '0' || w[i] > '9') {
                serial_write_line("Usage: mail del <number>");
                return;
            }
            n = n * 10 + (w[i] - '0');
        }
        char name[16];
        if (mail_file_name(n, name, sizeof(name)) != 0 ||
            vfs_delete(name) != 0) {
            serial_write_line("mail: no such message.");
            return;
        }
        serial_write("mail: deleted message ");
        print_u64_serial((uint64_t)n);
        serial_write_line(".");
        return;
    }
    serial_write_line("Usage: mail | mail config | mail setup <smtp_host> <smtp_port> <pop3_host> <pop3_port> <user> <pass> <from> | mail send <to> <subject> <body...> | mail recv | mail read <number> | mail del <number>");
}

/* `fetch <host> [path]`: fetch a plain HTTP page and print
 * its body (at most the output buffer holds). */
static void fetch_flow(const char *line) {
    const char *p = line + 5; /* skip "fetch" */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        serial_write_line("Usage: fetch <host> [path]");
        return;
    }
    char host[64];
    size_t hi = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && hi + 1 < sizeof(host)) {
        host[hi++] = *p++;
    }
    host[hi] = '\0';
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char path[128];
    size_t pi = 0;
    if (*p == '\0') {
        path[pi++] = '/';
    } else {
        while (*p != '\0' && pi + 1 < sizeof(path)) {
            path[pi++] = *p++;
        }
    }
    path[pi] = '\0';
    char *body = (char *)kmalloc(16385);
    if (body == NULL) {
        serial_write_line("fetch: out of memory.");
        return;
    }
    size_t got = 0;
    int code = 0;
    serial_write("Fetching ");
    serial_write(host);
    serial_write(path);
    serial_write_line(" ...");
    int rc = net_fetch_http(host, path, body, 16385, &got, &code);
    if (rc != 0) {
        serial_write("fetch failed (HTTP ");
        char num[8];
        int ni = 0;
        int v = code;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[8];
            int ri = 0;
            while (v > 0 && ri < 7) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        serial_write(num);
        serial_write_line(").");
        kfree(body);
        return;
    }
    serial_write(body);
    if (got > 0 && body[got - 1] != '\n') {
        serial_write_char('\n');
    }
    kfree(body);
}

/* ---- 更新の設定 (settings.txt) ---- */

static void load_settings(prism_settings *s) {
    settings_defaults(s);
    size_t len = 0;
    const char *text = vfs_read_ptr("settings.txt", &len);
    if (text != NULL) {
        settings_parse(s, text, len);
    }
}

static void save_settings(const prism_settings *s) {
    if (guest_session) {
        /* ゲストの変更は永続化しない (監査 L5)。セッション
         * 内の状態 (shell_settings) は呼び出し側が更新する。 */
        return;
    }
    static char buf[SETTINGS_TEXT_CAP];
    size_t n = settings_serialize(s, buf, sizeof(buf));
    if (n >= sizeof(buf)) {
        /* 切り捨てた設定は保存しない (監査 M1)。 */
        serial_write_line("Failed to save settings.txt.");
        return;
    }
    if (vfs_write("settings.txt", buf, n) != 0) {
        serial_write_line("Failed to save settings.txt.");
    }
}

/* 監査ログの書き込み先 (security.c の sink)。audit.log に
 * 1 件追加する。時刻は RTC から取る。
 *
 * audit.log はディスク上では暗号化される。起動時に暗号文の
 * まま読み込まれた間 (ログイン前) は中身へ追記できないため、
 * 出来事を RAM の保留キューに貯め、ログインが通って復号
 * された直後に audit_flush_pending() でまとめて書き込む。
 * ログインできないまま電源が落ちれば保留分は消えるが、
 * 従来も同期前の出来事はディスクに残らなかったので、
 * 永続化の意味は変わらない。 */
#define AUDIT_PENDING_MAX 16
static char audit_pending_time[AUDIT_PENDING_MAX][20];
static char audit_pending_event[AUDIT_PENDING_MAX][25];
static char audit_pending_detail[AUDIT_PENDING_MAX][49];
static int audit_pending_count;

static void audit_copy_str(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void audit_append_now(const char *ts, const char *event,
                             const char *detail) {
    size_t len = 0;
    const char *old = vfs_read_ptr("audit.log", &len);
    static char buf[AUDIT_LOG_CAP];
    size_t n = 0;
    if (old != NULL) {
        while (n < len && n + 1 < sizeof(buf)) {
            buf[n] = old[n];
            n++;
        }
    }
    buf[n] = '\0';
    if (audit_append(buf, sizeof(buf), ts, event, detail) != 0) {
        return;
    }
    size_t m = 0;
    while (buf[m] != '\0') {
        m++;
    }
    vfs_write("audit.log", buf, m);
}

/* ログイン前の出来事をディスクにも残すジャーナル。
 * preaudit.log は平文の例外 (persist.c) で、1 行
 * "時刻|出来事|詳細" を追記する。中身は出来事の名前と
 * 固定文だけで秘密は入らない。ログインが通ったときに
 * audit_flush_pending() が本体の監査ログ (暗号化 +
 * ハッシュチェーン) へ取り込んで消す。これにより、
 * ロックアウトで止まった・電源が落ちた場合でも
 * ログイン前の出来事が無言で消えなくなった。 */
#define PREAUDIT_CAP 8192
static void audit_journal_append(const char *ts, const char *event,
                                 const char *detail) {
    if (!persist_installed()) {
        return;
    }
    size_t len = 0;
    const char *old = vfs_read_ptr("preaudit.log", &len);
    static char buf[PREAUDIT_CAP + 512];
    size_t n = 0;
    if (old != NULL) {
        while (n < len && n < PREAUDIT_CAP) {
            buf[n] = old[n];
            n++;
        }
    }
    if (n >= PREAUDIT_CAP) {
        return; /* 肥大化防止 (RAM 側の保留は別に効く) */
    }
    const char *parts[3];
    parts[0] = ts;
    parts[1] = event;
    parts[2] = detail;
    for (int k = 0; k < 3; k++) {
        if (k > 0 && n + 1 < sizeof(buf)) {
            buf[n++] = '|';
        }
        const char *w = parts[k];
        while (*w != '\0' && n + 1 < sizeof(buf)) {
            char ch = *w++;
            buf[n++] = (ch == '|' || ch == '\n') ? '_' : ch;
        }
    }
    if (n + 1 < sizeof(buf)) {
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    if (vfs_write("preaudit.log", buf, n) == 0) {
        persist_maybe_sync();
    }
}

static void shell_audit_sink(const char *event, const char *detail) {
    char ts[20];
    persist_timestamp_now(ts);
    if (persist_name_pending_decrypt("audit.log")) {
        audit_journal_append(ts, event, detail);
        if (audit_pending_count == AUDIT_PENDING_MAX) {
            /* 満杯なら古いものから捨てる。 */
            for (int i = 1; i < AUDIT_PENDING_MAX; i++) {
                audit_copy_str(audit_pending_time[i - 1], 20,
                               audit_pending_time[i]);
                audit_copy_str(audit_pending_event[i - 1], 25,
                               audit_pending_event[i]);
                audit_copy_str(audit_pending_detail[i - 1], 49,
                               audit_pending_detail[i]);
            }
            audit_pending_count = AUDIT_PENDING_MAX - 1;
        }
        audit_copy_str(audit_pending_time[audit_pending_count], 20,
                       ts);
        audit_copy_str(audit_pending_event[audit_pending_count], 25,
                       event);
        audit_copy_str(audit_pending_detail[audit_pending_count], 49,
                       detail);
        audit_pending_count++;
        return;
    }
    audit_append_now(ts, event, detail);
}

/* 保留していた出来事を、復号済みの監査ログへ順に書き込む。
 * ジャーナル (preaudit.log) があればそちらが正本: 前回の
 * 起動で取り込まれなかった分も含めて順に取り込み、消す。
 * 無ければ RAM の保留キューを使う。 */
static void audit_flush_pending(void) {
    size_t jlen = 0;
    const char *journal = vfs_read_ptr("preaudit.log", &jlen);
    if (journal != NULL && jlen > 0) {
        size_t i = 0;
        while (i < jlen) {
            char line[128];
            size_t ln = 0;
            while (i < jlen && journal[i] != '\n' &&
                   ln + 1 < sizeof(line)) {
                line[ln++] = journal[i++];
            }
            while (i < jlen && journal[i] != '\n') {
                i++;
            }
            if (i < jlen) {
                i++; /* 改行を読み飛ばす */
            }
            line[ln] = '\0';
            /* "時刻|出来事|詳細" に分ける。 */
            char *bar1 = NULL;
            char *bar2 = NULL;
            for (size_t k = 0; line[k] != '\0'; k++) {
                if (line[k] == '|' && bar1 == NULL) {
                    bar1 = line + k;
                } else if (line[k] == '|' && bar2 == NULL) {
                    bar2 = line + k;
                }
            }
            if (bar1 != NULL && bar2 != NULL) {
                *bar1 = '\0';
                *bar2 = '\0';
                audit_append_now(line, bar1 + 1, bar2 + 1);
            }
        }
        vfs_delete("preaudit.log");
        persist_maybe_sync();
        audit_pending_count = 0;
        return;
    }
    for (int i = 0; i < audit_pending_count; i++) {
        audit_append_now(audit_pending_time[i],
                         audit_pending_event[i],
                         audit_pending_detail[i]);
    }
    audit_pending_count = 0;
}

/* チャンネルに合ったマニフェストの場所を作る。ベータ版は
 * <path>.beta を見る。out は cap バイト以上あること。 */
static void manifest_path_for(const prism_settings *s, char *out,
                              size_t cap) {
    size_t i = 0;
    while (s->update_path[i] != '\0' && i + 1 < cap) {
        out[i] = s->update_path[i];
        i++;
    }
    if (s->update_channel == 1) {
        const char *suffix = ".beta";
        size_t j = 0;
        while (suffix[j] != '\0' && i + 1 < cap) {
            out[i++] = suffix[j++];
        }
    }
    out[i] = '\0';
}

/* マニフェストの署名 (<path>.sig) を取ってきて検証する。
 * 署名がなければ・長さが違えば・正しくなければ -1 を返す。
 * 署名のないマニフェストは受け付けない。 */
static int verify_manifest_signature(const prism_settings *s, const char *path,
                                     const char *manifest,
                                     size_t manifest_len) {
    char sig_path[168];
    size_t n = 0;
    while (path[n] != '\0' && n + 1 < sizeof(sig_path)) {
        sig_path[n] = path[n];
        n++;
    }
    const char *suffix = ".sig";
    size_t si = 0;
    while (suffix[si] != '\0' && n + 1 < sizeof(sig_path)) {
        sig_path[n++] = suffix[si++];
    }
    sig_path[n] = '\0';
    /* 署名 2420 バイト + 応答ヘッダ分の余裕。 */
    static uint8_t sigbuf[4096];
    size_t got = 0;
    int code = 0;
    if (net_fetch_http(s->update_host, sig_path, (char *)sigbuf,
                       sizeof(sigbuf), &got, &code) != 0) {
        return -1;
    }
    return update_verify_manifest_sig((const uint8_t *)manifest, manifest_len,
                                      sigbuf, got);
}

/* ---- カーネル自体の更新 ----
 *
 * 手順: 起動パーティション (FAT16) を開く → /boot/prismos.new
 * に新しいカーネルを HTTP から少しずつ書き込みながら SHA-256
 * を計算する → ハッシュがマニフェストと合えば、ディレクトリ
 * エントリの入れ替え (1 セクタへの 1 回の書き込み) で
 * /boot/prismos.elf と置き換える → 古いカーネルを指す
 * prismos.new を消す。途中で止まっても、/boot/prismos.elf は
 * 古いカーネルのまま残る。署名はマニフェスト全体の ML-DSA
 * 署名でカバーする (kernel 行のハッシュが署名対象に含まれる)。
 * カーネルイメージは約 4.2MB あり VFS の 1 ファイル上限
 * (16384 バイト) には入らないため、VFS を経由しない。
 * 戻り値: 0 = 置き換えた、-1 = 失敗した (旧カーネルは残る)。 */

/* ブロック層を FAT16 のセクタ読み書きにつなぐ。 */
static int fat16_block_read(void *ctx, uint32_t lba, uint8_t *buf) {
    (void)ctx;
    return block_read_sector((uint64_t)lba, buf);
}

static int fat16_block_write(void *ctx, uint32_t lba, const uint8_t *buf) {
    (void)ctx;
    return block_write_sector((uint64_t)lba, buf);
}

typedef struct {
    fat16_writer *writer;
    crypto_sha256_ctx *sha;
    int failed;
} kernel_stream_ctx;

/* ストリーミング取得のコールバック: 届いた分を FAT に書き、
 * 同時に SHA-256 を進める。書き込みに失敗したら取得を止める。 */
static int kernel_stream_cb(void *ctx, const uint8_t *data, size_t len) {
    kernel_stream_ctx *k = (kernel_stream_ctx *)ctx;
    if (k->failed) {
        return -1;
    }
    crypto_sha256_update(k->sha, data, len);
    if (fat16_write_chunk(k->writer, data, len) != 0) {
        k->failed = 1;
        return -1;
    }
    return 0;
}

static int update_apply_kernel(const prism_settings *s,
                               const update_manifest *m) {
    /* カーネルイメージの場所: <base>/kernel.elf。 */
    char url[128];
    size_t ui = 0;
    const char *base = m->base;
    while (*base != '\0' && ui + 1 < sizeof(url)) {
        url[ui++] = *base++;
    }
    const char *tail = "/kernel.elf";
    size_t ti = 0;
    while (tail[ti] != '\0' && ui + 1 < sizeof(url)) {
        url[ui++] = tail[ti++];
    }
    url[ui] = '\0';

    /* 起動パーティション (FAT16) を開く。 */
    static fat16_fs fs;
    fat16_dev dev;
    dev.ctx = NULL;
    dev.read_sector = fat16_block_read;
    dev.write_sector = fat16_block_write;
    dev.total_sectors = (uint32_t)block_total_sectors();
    int open_rc = fat16_open(&dev, &fs);
    if (open_rc == -2) {
        serial_write_line("Update failed: boot partition is not FAT16 "
                          "(kernel update needs a FAT16 boot partition).");
        return -1;
    }
    if (open_rc != 0) {
        serial_write_line("Update failed: no boot partition found "
                          "(kernel update needs the boot disk).");
        return -1;
    }
    uint64_t free_bytes = 0;
    if (fat16_free_bytes(&fs, &free_bytes) != 0) {
        serial_write_line("Update failed: could not read the boot "
                          "partition.");
        return -1;
    }
    if (free_bytes < 1024 * 1024) {
        serial_write("Update failed: not enough space on the boot "
                     "partition (");
        print_u64_serial(free_bytes / 1024);
        serial_write_line(" KB free, need room for a second kernel "
                          "image).");
        return -1;
    }
    /* 古い一時ファイルを片付ける。 */
    (void)fat16_delete(&fs, "/boot/prismos.new");
    (void)fat16_delete(&fs, "/boot/prismos.old");
    static fat16_writer w;
    if (fat16_write_begin(&fs, "/boot/prismos.new", &w) != 0) {
        serial_write_line("Update failed: could not create the new "
                          "kernel file on the boot partition.");
        return -1;
    }
    static crypto_sha256_ctx sha;
    crypto_sha256_init(&sha);
    static kernel_stream_ctx kctx;
    kctx.writer = &w;
    kctx.sha = &sha;
    kctx.failed = 0;
    size_t total = 0;
    int status = 0;
    int fetch_rc = net_fetch_http_stream(s->update_host, url,
                                         kernel_stream_cb, &kctx, &total,
                                         &status);
    if (fetch_rc != 0 || kctx.failed) {
        (void)fat16_write_abort(&w);
        serial_write("kernel download: got ");
        print_u64_serial((uint64_t)total);
        serial_write(" bytes, status ");
        print_u64_serial((uint64_t)(status < 0 ? 0 : status));
        serial_write_line(".");
        if (status == 404) {
            serial_write_line("Update failed: kernel image not found "
                              "on the update server.");
        } else if (kctx.failed) {
            serial_write_line("Update failed: boot partition ran out "
                              "of space for the new kernel.");
        } else {
            serial_write_line("Update failed: could not download the "
                              "kernel image.");
        }
        return -1;
    }
    if (total == 0) {
        (void)fat16_write_abort(&w);
        serial_write_line("Update failed: empty kernel image.");
        return -1;
    }
    uint8_t got_hash[32];
    crypto_sha256_final(&sha, got_hash);
    if (memcmp(got_hash, m->kernel_sha256, 32) != 0) {
        (void)fat16_write_abort(&w);
        serial_write_line("Update failed: bad kernel image "
                          "(hash mismatch).");
        return -1;
    }
    if (fat16_write_finish(&w) != 0) {
        (void)fat16_write_abort(&w);
        serial_write_line("Update failed: could not finish writing "
                          "the new kernel.");
        return -1;
    }
    /* 原子的な置き換え: 2 つのエントリのクラスタ番号と大きさを、
     * 同じセクタへの 1 回の書き込みで入れ替える。 */
    if (fat16_swap_entries(&fs, "/boot/prismos.elf",
                           "/boot/prismos.new") != 0) {
        (void)fat16_delete(&fs, "/boot/prismos.new");
        serial_write_line("Update failed: could not switch to the "
                          "new kernel.");
        return -1;
    }
    /* 入れ替え後の prismos.new は古いカーネルを指す。消して
     * クラスタを返す。消せなくても起動には差し支えない。 */
    if (fat16_delete(&fs, "/boot/prismos.new") != 0) {
        serial_write_line("Warning: could not remove the old kernel "
                          "backup.");
    }
    serial_write("Kernel updated: ");
    serial_write(m->version);
    serial_write(" (");
    print_u64_serial((uint64_t)total);
    serial_write_line(" bytes). Reboot to use it.");
    return 0;
}

/* ---- 更新の適用 (コマンドと自動適用で共用) ----
 *
 * 手順: 復元ポイントを作る (失敗時の巻き戻し用) → ハッシュが
 * 違うファイルだけ取ってきて検証する → 検証が済んだものから
 * VFS に書き、ディスクに同期する。途中で壊れたファイルが
 * あれば、復元ポイントに戻して失敗を報告する。マニフェストは
 * 呼び出し側が取っておく (二重に取らないため)。
 * 戻り値: 0 = 適用した、-1 = 失敗した。 */
static int update_apply_manifest(const prism_settings *s,
                                 const update_manifest *mp) {
    const update_manifest *m = mp;
    serial_write("Update available: ");
    serial_write(m->version);
    serial_write(" (");
    serial_write(m->type);
    serial_write_line(")");
    if (m->has_kernel) {
        serial_write_line("This update includes a kernel update "
                          "(reboot after applying).");
    }
    /* 失敗時の巻き戻し用に復元ポイントを作る。 */
    char snap_name[40];
    {
        const char *prefix = "pre-update-";
        size_t i = 0;
        while (prefix[i] != '\0') {
            snap_name[i] = prefix[i];
            i++;
        }
        const char *v = m->version;
        while (*v != '\0' && i + 1 < sizeof(snap_name)) {
            snap_name[i++] = *v++;
        }
        snap_name[i] = '\0';
    }
    int have_snap = 0;
    if (persist_disk_present() && persist_installed()) {
        if (persist_snapshot_create(snap_name) == 0) {
            have_snap = 1;
        }
    }
    if (!have_snap) {
        serial_write_line("Warning: no restore point (no installed disk).");
    }
    /* 中断マーカー: 書き込みの前に update.state へ「適用中」を
     * 残し、先にディスクへ出す。電源断などで終わらないまま
     * 再起動したら、次回起動時に検出して知らせる。 */
    {
        char st[64];
        size_t n = 0;
        const char *pfx = "applying ";
        while (*pfx != '\0' && n + 1 < sizeof(st)) {
            st[n++] = *pfx++;
        }
        const char *v = m->version;
        while (*v != '\0' && n + 1 < sizeof(st)) {
            st[n++] = *v++;
        }
        st[n] = '\0';
        vfs_write("update.state", st, n);
        persist_maybe_sync();
    }
    /* ハッシュが違うファイルだけ取ってくる。 */
    static uint8_t file_buf[DISKFS_MAX_FILE_SIZE];
    int downloaded = 0;
    int unchanged = 0;
    for (int i = 0; i < m->file_count; i++) {
        const char *name = m->files[i].name;
        size_t len = 0;
        const char *local = vfs_read_ptr(name, &len);
        if (local != NULL) {
            char local_hex[65];
            update_sha256_hex((const uint8_t *)local, len, local_hex);
            char want_hex[65];
            update_bytes_to_hex(m->files[i].sha256, want_hex);
            if (strcmp(local_hex, want_hex) == 0) {
                unchanged++;
                continue;
            }
        }
        /* 取ってくる場所: <base>/<名前>。 */
        char url[128];
        size_t ui = 0;
        const char *base = m->base;
        while (*base != '\0' && ui + 1 < sizeof(url)) {
            url[ui++] = *base++;
        }
        if (ui + 1 < sizeof(url)) {
            url[ui++] = '/';
        }
        const char *np = name;
        while (*np != '\0' && ui + 1 < sizeof(url)) {
            url[ui++] = *np++;
        }
        url[ui] = '\0';
        size_t fgot = 0;
        int fcode = 0;
        if (net_fetch_http(s->update_host, url, (char *)file_buf,
                           sizeof(file_buf), &fgot, &fcode) != 0) {
            serial_write("Update failed: could not download ");
            serial_write_line(name);
            goto failed;
        }
        char got_hex[65];
        update_sha256_hex(file_buf, fgot, got_hex);
        char want_hex[65];
        update_bytes_to_hex(m->files[i].sha256, want_hex);
        if (strcmp(got_hex, want_hex) != 0) {
            serial_write("Update failed: bad file ");
            serial_write_line(name);
            goto failed;
        }
        /* 検証が済んだものだけ、ここで書き込む。 */
        if (vfs_write(name, (const char *)file_buf, fgot) != 0) {
            serial_write("Update failed: could not write ");
            serial_write_line(name);
            goto failed;
        }
        downloaded++;
    }
    /* カーネル更新がある場合は、ファイルのあとに行う。
     * 失敗したらファイルも巻き戻す (カーネル側は .new を
     * 消してあり、旧カーネルは残っている)。 */
    if (m->has_kernel) {
        if (update_apply_kernel(s, m) != 0) {
            goto failed;
        }
    }
    vfs_write("update.state", "idle", 4);
    persist_maybe_sync();
    serial_write("Update applied: ");
    serial_write(m->version);
    serial_write(" (");
    print_u64_serial((uint64_t)downloaded);
    serial_write(" downloaded, ");
    print_u64_serial((uint64_t)unchanged);
    serial_write_line(" unchanged).");
    return 0;

failed:
    vfs_write("update.state", "idle", 4);
    persist_maybe_sync();
    if (have_snap) {
        if (persist_snapshot_restore(snap_name) == 0) {
            desktop_notify("Update failed: rolled back");
            persist_load();
            /* ログイン後の再読込は暗号文のまま載るので復号する
             * (鍵が無ければ何もしない)。 */
            persist_decrypt_loaded();
            serial_write_line("Rolled back to the restore point.");
        } else {
            serial_write_line("Rollback failed: restore point is broken.");
        }
    }
    return -1;
}

/* ---- 更新チェック/適用/ロールバックの中核 ----
 * `update` コマンドと設定アプリの更新ページで共用する。
 * マニフェスト取得・署名検証・適用の実体はここだけにあり、
 * 並行実装を作らない。チェックで得たマニフェストと設定は
 * 静的に保持し、apply_pending がそのまま使う。 */
static update_manifest update_pending;
static prism_settings update_pending_settings;
static int update_pending_valid;

int shell_update_check(shell_update_info *info) {
    update_pending_valid = 0;
    prism_settings s;
    load_settings(&s);
    if (s.update_host[0] == '\0') {
        return SHELL_UPDATE_NO_SERVER;
    }
    char path[160];
    manifest_path_for(&s, path, sizeof(path));
    static char mbuf[16385];
    size_t got = 0;
    int code = 0;
    if (net_fetch_http(s.update_host, path, mbuf, sizeof(mbuf), &got,
                       &code) != 0) {
        return SHELL_UPDATE_UNREACHABLE;
    }
    if (verify_manifest_signature(&s, path, mbuf, got) != 0) {
        return SHELL_UPDATE_BAD_SIGNATURE;
    }
    if (update_parse_manifest(mbuf, got, &update_pending) != 0) {
        return SHELL_UPDATE_BAD_MANIFEST;
    }
    /* 新旧比較は表示ラベル (27H1) ではなく内部順序バージョン
     * で行う (version.h の PRISM_VERSION_ORDER の説明参照)。 */
    if (update_version_cmp(update_pending.version,
                           PRISM_VERSION_ORDER) <= 0) {
        return SHELL_UPDATE_UP_TO_DATE;
    }
    update_pending_settings = s;
    update_pending_valid = 1;
    if (info != NULL) {
        size_t i = 0;
        while (update_pending.version[i] != '\0' &&
               i + 1 < sizeof(info->version)) {
            info->version[i] = update_pending.version[i];
            i++;
        }
        info->version[i] = '\0';
        i = 0;
        while (update_pending.type[i] != '\0' &&
               i + 1 < sizeof(info->type)) {
            info->type[i] = update_pending.type[i];
            i++;
        }
        info->type[i] = '\0';
        i = 0;
        while (update_pending.channel[i] != '\0' &&
               i + 1 < sizeof(info->channel)) {
            info->channel[i] = update_pending.channel[i];
            i++;
        }
        info->channel[i] = '\0';
        info->has_kernel = update_pending.has_kernel;
        info->file_count = update_pending.file_count;
    }
    return SHELL_UPDATE_AVAILABLE;
}

int shell_update_apply_pending(void) {
    if (!update_pending_valid) {
        return -1;
    }
    int rc = update_apply_manifest(&update_pending_settings,
                                   &update_pending);
    update_pending_valid = 0;
    return rc;
}

int shell_update_rollback(char *name_out, size_t cap) {
    diskfs_snapshot_info infos[DISKFS_SNAP_SLOTS];
    int count = 0;
    if (persist_snapshot_list(infos, DISKFS_SNAP_SLOTS, &count) !=
            0 ||
        count == 0) {
        return -1;
    }
    /* いちばん新しい pre-update-* を探す。 */
    int best = -1;
    for (int i = 0; i < count; i++) {
        if (strncmp(infos[i].name, "pre-update-", 11) != 0) {
            continue;
        }
        if (best < 0 || infos[i].seq > infos[best].seq) {
            best = i;
        }
    }
    if (best < 0) {
        return -2;
    }
    if (persist_snapshot_restore(infos[best].name) != 0) {
        return -3;
    }
    persist_load();
    /* ログイン後の再読込は暗号文のまま載るので復号する
     * (鍵が無ければ何もしない)。 */
    persist_decrypt_loaded();
    if (name_out != NULL && cap > 0) {
        size_t i = 0;
        while (infos[best].name[i] != '\0' && i + 1 < cap) {
            name_out[i] = infos[best].name[i];
            i++;
        }
        name_out[i] = '\0';
    }
    return 0;
}

/* 起動時の自動セキュリティ更新。auto_security が on のときだけ
 * 動く。セキュリティ更新があれば適用し、機能更新があれば
 * あることだけ知らせる。 */
static void auto_security_update(void) {
    prism_settings s;
    load_settings(&s);
    if (!s.auto_security || s.update_host[0] == '\0') {
        return;
    }
    shell_update_info info;
    int rc = shell_update_check(&info);
    if (rc == SHELL_UPDATE_UNREACHABLE) {
        serial_write_line("[update] auto check failed.");
        return;
    }
    if (rc == SHELL_UPDATE_BAD_SIGNATURE) {
        serial_write_line("[update] auto check failed: bad signature.");
        return;
    }
    if (rc == SHELL_UPDATE_BAD_MANIFEST) {
        serial_write_line("[update] auto check failed: bad manifest.");
        return;
    }
    if (rc != SHELL_UPDATE_AVAILABLE) {
        return;
    }
    if (strcmp(info.type, "security") != 0) {
        serial_write("[update] feature update ");
        serial_write(info.version);
        serial_write_line(" is available (manual).");
        return;
    }
    serial_write("[update] auto-applying security update ");
    serial_write_line(info.version);
    shell_update_apply_pending();
}

/* ---- 互換レイヤー ---- */

/* `run <file> [args...]`: VFS の実行ファイルを互換レイヤーで動かす。 */
static void run_flow(const char *line) {
    const char *p = skip_spaces(line + 3); /* skip "run" */
    if (p[0] == '\0') {
        serial_write_line("Usage: run <file> [args...]");
        return;
    }
    char name[32];
    size_t n = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' &&
           n + 1 < sizeof(name)) {
        name[n++] = *p++;
    }
    name[n] = '\0';
    const char *args[15];
    char argbuf[15][32];
    int nargs = 0;
    while (nargs < 15) {
        p = skip_spaces(p);
        if (*p == '\0') {
            break;
        }
        size_t m = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               m + 1 < sizeof(argbuf[0])) {
            argbuf[nargs][m++] = *p++;
        }
        argbuf[nargs][m] = '\0';
        args[nargs] = argbuf[nargs];
        nargs++;
    }
    int code = -1;
    int rc = compat_run_file(name, args, nargs, &code);
    if (rc == 0) {
        serial_write("compat: ");
        serial_write(name);
        serial_write(" exited with code ");
        print_u64_serial((uint64_t)code);
        serial_write_line(".");
        audit_log_event("compat-run", name);
    }
}

/* `compat info <file> | compat list | compat calls`。 */
static void compat_flow(const char *line) {
    const char *arg = skip_spaces(line + 6); /* skip "compat" */
    if (arg[0] == '\0') {
        serial_write_line(
            "Usage: compat info <file> | compat list | compat calls");
        return;
    }
    if (starts_with_word(arg, "info")) {
        const char *p = skip_spaces(arg + 4);
        if (p[0] == '\0') {
            serial_write_line("Usage: compat info <file>");
            return;
        }
        char name[32];
        size_t n = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               n + 1 < sizeof(name)) {
            name[n++] = *p++;
        }
        name[n] = '\0';
        char info[256];
        int rc = compat_info_file(name, info, sizeof(info));
        if (rc == -1) {
            serial_write_line("compat: no such file");
        } else if (rc == -2) {
            serial_write_line("compat: not a supported executable");
        } else {
            serial_write_line(info);
        }
        return;
    }
    if (strcmp(arg, "list") == 0) {
        int count = vfs_file_count();
        int found = 0;
        for (int i = 0; i < count; i++) {
            const char *name = vfs_file_name(i);
            if (name != NULL && compat_is_app(name)) {
                serial_write_line(name);
                found++;
            }
        }
        if (found == 0) {
            serial_write_line("No compat apps.");
        }
        return;
    }
    if (strcmp(arg, "calls") == 0) {
        serial_write_line("Compat calls (int 0x80):");
        for (uint64_t nr = 0; nr <= 8; nr++) {
            serial_write("  ");
            print_u64_serial(nr);
            serial_write("  ");
            serial_write_line(compat_syscall_name(nr));
        }
        serial_write_line(
            "Other calls return -38 (ENOSYS: not supported).");
        return;
    }
    serial_write_line(
        "Usage: compat info <file> | compat list | compat calls");
}

/* `snapshot` / `update` / `recovery` コマンドの判定用。 */
static int starts_with_word(const char *line, const char *word) {
    size_t i = 0;
    while (word[i] != '\0') {
        if (line[i] != word[i]) {
            return 0;
        }
        i++;
    }
    return line[i] == '\0' || line[i] == ' ' || line[i] == '\t';
}

static const char *skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/* 復元ポイントの名前を時計から作る。"snap-YYYYMMDD-HHMMSS"。 */
static void snapshot_auto_name(char *out, size_t cap) {
    char ts[20];
    persist_timestamp_now(ts);
    const char *prefix = "snap-";
    size_t i = 0;
    while (prefix[i] != '\0' && i + 1 < cap) {
        out[i] = prefix[i];
        i++;
    }
    /* ts は "YYYY-MM-DD HH:MM:SS"。区切りを抜いて詰める。 */
    for (int j = 0; ts[j] != '\0' && i + 1 < cap; j++) {
        char c = ts[j];
        if (c == '-' || c == ' ' || c == ':') {
            continue;
        }
        out[i++] = c;
    }
    out[i] = '\0';
}

void shell_snapshot_auto_name(char *out, size_t cap) {
    snapshot_auto_name(out, cap);
}

static void snapshot_list_print(void) {
    diskfs_snapshot_info infos[DISKFS_SNAP_SLOTS];
    int count = 0;
    if (persist_snapshot_list(infos, DISKFS_SNAP_SLOTS, &count) != 0 ||
        count == 0) {
        serial_write_line("No restore points.");
        return;
    }
    for (int i = 0; i < count; i++) {
        serial_write("  ");
        serial_write(infos[i].name);
        serial_write("  ");
        serial_write(infos[i].timestamp);
        serial_write("  (");
        print_u64_serial((uint64_t)infos[i].file_count);
        serial_write_line(" files)");
    }
}

static void snapshot_flow(const char *line) {
    const char *arg = skip_spaces(line + 8); /* skip "snapshot" */
    if (arg[0] == '\0') {
        /* 復元ポイントを作る。 */
        char name[40];
        snapshot_auto_name(name, sizeof(name));
        int rc = persist_snapshot_create(name);
        if (rc == 0) {
            serial_write("Restore point created: ");
            serial_write_line(name);
        } else {
            serial_write_line("Snapshot failed: no installed disk.");
        }
        return;
    }
    if (strcmp(arg, "list") == 0) {
        snapshot_list_print();
        return;
    }
    if (starts_with_word(arg, "restore")) {
        const char *name = skip_spaces(arg + 7);
        if (name[0] == '\0') {
            serial_write_line("Usage: snapshot restore <name>");
            return;
        }
        int rc = persist_snapshot_restore(name);
        if (rc == 0) {
            persist_load();
            persist_decrypt_loaded();
            serial_write("Restored snapshot '");
            serial_write(name);
            serial_write_line("'.");
        } else if (rc == -1) {
            serial_write_line("No such restore point.");
        } else {
            serial_write_line("Restore failed: snapshot is broken.");
        }
        return;
    }
    if (starts_with_word(arg, "delete")) {
        const char *name = skip_spaces(arg + 6);
        if (name[0] == '\0') {
            serial_write_line("Usage: snapshot delete <name>");
            return;
        }
        if (persist_snapshot_delete(name) == 0) {
            serial_write("Deleted restore point '");
            serial_write(name);
            serial_write_line("'.");
        } else {
            serial_write_line("No such restore point.");
        }
        return;
    }
    serial_write_line("Usage: snapshot | snapshot list | snapshot restore <name> | snapshot delete <name>");
}

static void update_flow(const char *line) {
    const char *arg = skip_spaces(line + 6); /* skip "update" */
    if (arg[0] == '\0') {
        serial_write_line("Usage: update check | update apply [yes] | update channel <stable|beta> | update auto <on|off> | update server <host> [path] | update rollback");
        return;
    }
    if (strcmp(arg, "check") == 0) {
        shell_update_info info;
        int rc = shell_update_check(&info);
        if (rc == SHELL_UPDATE_NO_SERVER) {
            serial_write_line("No update server configured.");
            serial_write_line("Set it with `update server <host> [path]`.");
            return;
        }
        if (rc == SHELL_UPDATE_UNREACHABLE) {
            serial_write_line("Update check failed: could not reach the server.");
            return;
        }
        if (rc == SHELL_UPDATE_BAD_SIGNATURE) {
            serial_write_line("Update check failed: bad signature.");
            return;
        }
        if (rc == SHELL_UPDATE_BAD_MANIFEST) {
            serial_write_line("Update check failed: bad manifest.");
            return;
        }
        if (rc == SHELL_UPDATE_UP_TO_DATE) {
            serial_write("Already up to date (");
            serial_write(PRISM_VERSION);
            serial_write_line(").");
            return;
        }
        serial_write("Update available: ");
        serial_write(info.version);
        serial_write(" (");
        serial_write(info.type);
        serial_write(", ");
        serial_write(info.channel);
        serial_write_line(")");
        if (info.has_kernel) {
            serial_write_line("This update includes a kernel update "
                              "(reboot after applying).");
        }
        return;
    }
    if (starts_with_word(arg, "apply")) {
        const char *rest = skip_spaces(arg + 5);
        /* 機能更新は確認が要る。セキュリティ更新はそのまま。 */
        shell_update_info info;
        int rc = shell_update_check(&info);
        if (rc == SHELL_UPDATE_NO_SERVER) {
            serial_write_line("No update server configured.");
            serial_write_line("Set it with `update server <host> [path]`.");
            return;
        }
        if (rc == SHELL_UPDATE_UNREACHABLE) {
            serial_write_line("Update check failed: could not reach the server.");
            return;
        }
        if (rc == SHELL_UPDATE_BAD_SIGNATURE) {
            serial_write_line("Update check failed: bad signature.");
            return;
        }
        if (rc == SHELL_UPDATE_BAD_MANIFEST) {
            serial_write_line("Update check failed: bad manifest.");
            return;
        }
        if (rc == SHELL_UPDATE_UP_TO_DATE) {
            serial_write("Already up to date (");
            serial_write(PRISM_VERSION);
            serial_write_line(").");
            return;
        }
        if (strcmp(info.type, "feature") == 0 &&
            strcmp(rest, "yes") != 0) {
            serial_write_line("This is a feature update.");
            serial_write_line("Run `update apply yes` to apply it.");
            return;
        }
        shell_update_apply_pending();
        return;
    }
    if (starts_with_word(arg, "channel")) {
        const char *v = skip_spaces(arg + 7);
        prism_settings s;
        load_settings(&s);
        if (strcmp(v, "beta") == 0) {
            s.update_channel = 1;
            save_settings(&s);
            serial_write_line("Update channel: beta.");
        } else if (strcmp(v, "stable") == 0) {
            s.update_channel = 0;
            save_settings(&s);
            serial_write_line("Update channel: stable.");
        } else {
            serial_write_line("Usage: update channel <stable|beta>");
        }
        return;
    }
    if (starts_with_word(arg, "auto")) {
        const char *v = skip_spaces(arg + 4);
        prism_settings s;
        load_settings(&s);
        if (strcmp(v, "on") == 0) {
            s.auto_security = 1;
            save_settings(&s);
            serial_write_line("Auto security updates: on.");
        } else if (strcmp(v, "off") == 0) {
            s.auto_security = 0;
            save_settings(&s);
            serial_write_line("Auto security updates: off.");
        } else {
            serial_write_line("Usage: update auto <on|off>");
        }
        return;
    }
    if (starts_with_word(arg, "server")) {
        const char *v = skip_spaces(arg + 6);
        if (v[0] == '\0') {
            serial_write_line("Usage: update server <host> [path]");
            return;
        }
        char host[64];
        size_t hi = 0;
        while (v[hi] != '\0' && v[hi] != ' ' && v[hi] != '\t' &&
               hi + 1 < sizeof(host)) {
            host[hi] = v[hi];
            hi++;
        }
        host[hi] = '\0';
        const char *p = skip_spaces(v + hi);
        prism_settings s;
        load_settings(&s);
        size_t j = 0;
        while (host[j] != '\0' && j + 1 < sizeof(s.update_host)) {
            s.update_host[j] = host[j];
            j++;
        }
        s.update_host[j] = '\0';
        if (p[0] != '\0') {
            size_t k = 0;
            while (p[k] != '\0' && k + 1 < sizeof(s.update_path)) {
                s.update_path[k] = p[k];
                k++;
            }
            s.update_path[k] = '\0';
        }
        save_settings(&s);
        serial_write("Update server: ");
        serial_write(s.update_host);
        serial_write(" ");
        serial_write_line(s.update_path);
        return;
    }
    if (strcmp(arg, "rollback") == 0) {
        char name[40];
        int rc = shell_update_rollback(name, sizeof(name));
        if (rc == -1) {
            serial_write_line("No restore points to roll back to.");
            return;
        }
        if (rc == -2) {
            serial_write_line("No update restore point found.");
            return;
        }
        if (rc == 0) {
            desktop_notify("Update rolled back");
            serial_write("Rolled back to '");
            serial_write(name);
            serial_write_line("'.");
        } else {
            serial_write_line("Rollback failed.");
        }
        return;
    }
    serial_write_line("Usage: update check | update apply [yes] | update channel <stable|beta> | update auto <on|off> | update server <host> [path] | update rollback");
}

/* ---- リモート操作とセキュリティ ---- */

#define REMOTE_CONF_FILE "remote.conf"
#define REMOTE_LOCK_FILE "remote.lock"

static void load_remote_config(sec_remote_config *cfg) {
    remote_config_defaults(cfg);
    size_t len = 0;
    const char *text = vfs_read_ptr(REMOTE_CONF_FILE, &len);
    if (text != NULL) {
        remote_config_parse(cfg, text, len);
    }
}

static void save_remote_config(const sec_remote_config *cfg) {
    char buf[512];
    size_t n = remote_config_serialize(cfg, buf, sizeof(buf));
    if (vfs_write(REMOTE_CONF_FILE, buf, n) != 0) {
        serial_write_line("remote: could not save remote.conf.");
    }
}

/* PIN からリモート操作のトークンを作る。
 * SHA-256(PIN || "remote-lock") の 16 進 64 桁。 */
static void remote_token_for_pin(const char *pin, char *hex_out) {
    size_t pin_len = 0;
    while (pin[pin_len] != '\0') {
        pin_len++;
    }
    uint8_t buf[16 + 11];
    for (size_t i = 0; i < pin_len; i++) {
        buf[i] = (uint8_t)pin[i];
    }
    const char *tag = "remote-lock";
    for (int i = 0; i < 11; i++) {
        buf[pin_len + (size_t)i] = (uint8_t)tag[i];
    }
    uint8_t digest[32];
    crypto_sha256(buf, pin_len + 11, digest);
    static const char D[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i * 2] = D[(digest[i] >> 4) & 0xF];
        hex_out[i * 2 + 1] = D[digest[i] & 0xF];
    }
    hex_out[64] = '\0';
}

/* 管理者 PIN を確認し、通った PIN を pin_out に入れる。
 * 失敗したら 0。pin_out は呼び出し側で使い終わったら消すこと。 */
static int admin_check_pin(char *pin_out, size_t pin_cap) {
    size_t len = 0;
    const char *text = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
    if (text == NULL) {
        serial_write_line("No PIN is set. Run `passwd` first.");
        return 0;
    }
    char pin[PIN_CAP];
    serial_write("Admin PIN: ");
    read_secret(pin, sizeof(pin));
    int rc = account_verify(pin, text, len);
    if (rc != 1) {
        serial_write_line("Wrong PIN.");
        audit_log_event("security-fail", "admin PIN rejected");
        for (size_t i = 0; pin[i] != '\0'; i++) {
            pin[i] = 0;
        }
        return 0;
    }
    if (pin_out != NULL && pin_cap > 0) {
        size_t i = 0;
        while (pin[i] != '\0' && i + 1 < pin_cap) {
            pin_out[i] = pin[i];
            i++;
        }
        pin_out[i] = '\0';
    }
    for (size_t i = 0; pin[i] != '\0'; i++) {
        pin[i] = 0;
    }
    return 1;
}

/* 利用者のデータを消す (組み込みファイルは残す)。
 * リモートワイプ用。アカウントも消すので、次回起動は
 * 初期状態になる。 */
static void remote_wipe_device(void) {
    int n = vfs_file_count();
    for (int i = n - 1; i >= 0; i--) {
        const char *name = vfs_file_name(i);
        if (name == NULL) {
            continue;
        }
        if (vfs_is_builtin(name)) {
            continue;
        }
        vfs_delete(name);
    }
    persist_clear_key();
    persist_force_sync();
    serial_write_line("Device wiped by the administrator.");
    prism_halt();
}

/* サーバの指示を 1 回確認する。戻り値: 0 = 何もしない、
 * 1 = ロックした (remote.lock を書いた)、2 = ワイプした。
 * ロック・ワイプのときはこの関数が止まる (戻らない)。
 * ネットワークが無い・設定が無いときは 0 を返す。 */
static int remote_check_once(void) {
    sec_remote_config cfg;
    load_remote_config(&cfg);
    if (!cfg.enabled || cfg.host[0] == '\0' || cfg.token[0] == '\0') {
        return 0;
    }
    if (net_ensure() != 0) {
        serial_write_line("remote: network unavailable, skipping check.");
        return 0;
    }
    static char body[512];
    size_t got = 0;
    int code = 0;
    if (net_fetch_http(cfg.host, cfg.path, body, sizeof(body), &got,
                       &code) != 0) {
        serial_write_line("remote: could not fetch the instruction.");
        return 0;
    }
    char action[16];
    char token[65];
    if (remote_parse(body, got, action, sizeof(action), token,
                     sizeof(token)) != 0) {
        serial_write_line("remote: instruction has no action.");
        return 0;
    }
    if (strcmp(token, cfg.token) != 0) {
        serial_write_line("remote: token mismatch, ignoring.");
        audit_log_event("remote-fail", "token mismatch");
        return 0;
    }
    if (strcmp(action, "lock") == 0) {
        char ts[20];
        persist_timestamp_now(ts);
        char lock_text[32];
        size_t p = 0;
        const char *head = "locked\n";
        while (head[p] != '\0') {
            lock_text[p] = head[p];
            p++;
        }
        for (int i = 0; i < 19; i++) {
            lock_text[p++] = ts[i];
        }
        lock_text[p++] = '\n';
        lock_text[p] = '\0';
        if (vfs_write(REMOTE_LOCK_FILE, lock_text, p) != 0) {
            serial_write_line("remote: could not write the lock file.");
            return 0;
        }
        persist_maybe_sync();
        audit_log_event("remote-lock", "locked by administrator");
        serial_write_line("This device was locked by the administrator.");
        prism_halt();
        return 1;
    }
    if (strcmp(action, "wipe") == 0) {
        audit_log_event("remote-wipe", "wiped by administrator");
        serial_write_line("Wiping this device by administrator order...");
        remote_wipe_device();
        return 2;
    }
    return 0;
}

/* 起動時のリモート確認 (ログインの前)。
 * remote.lock があれば PIN で解除する。無ければサーバの
 * 指示を確認する。 */
static void remote_boot_check(void) {
    size_t len = 0;
    if (vfs_read_ptr(REMOTE_LOCK_FILE, &len) != NULL) {
        serial_write_line("");
        serial_write_line("This device is locked by the administrator.");
        serial_write_line("Enter the admin PIN to unlock.");
        size_t alen = 0;
        const char *atext = vfs_read_ptr(PRISM_ACCOUNT_FILE, &alen);
        if (atext == NULL) {
            serial_write_line("No PIN is set. Cannot unlock. Halting.");
            prism_halt();
        }
        for (int attempt = 0; attempt < 3; attempt++) {
            char pin[PIN_CAP];
            serial_write("PIN: ");
            read_secret(pin, sizeof(pin));
            int rc = account_verify(pin, atext, alen);
            for (size_t i = 0; pin[i] != '\0'; i++) {
                pin[i] = 0;
            }
            if (rc == 1) {
                vfs_delete(REMOTE_LOCK_FILE);
                /* 解除したらリモート確認を止める (サーバが
                 * まだ lock のままだと、すぐにまた止まるため)。
                 * 管理者が `security remote on` で再開できる。 */
                sec_remote_config cfg;
                load_remote_config(&cfg);
                cfg.enabled = 0;
                save_remote_config(&cfg);
                audit_log_event("remote-unlock", "unlocked by admin PIN");
                persist_maybe_sync();
                serial_write_line("Unlocked.");
                return;
            }
            serial_write_line("Wrong PIN.");
        }
        serial_write_line("Too many attempts. Halting.");
        prism_halt();
    }
    remote_check_once();
}

/* 10 進の整数を読む。読めなければ -1。 */
static int parse_u32_arg(const char *s, uint32_t *out) {
    if (s == NULL || *s < '0' || *s > '9') {
        return -1;
    }
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
        if (v > 100000u) {
            return -1;
        }
    }
    if (*s != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static void security_status(void) {
    serial_write("External media: ");
    serial_write_line(shell_settings.usb_media ? "block" : "allow");
    serial_write("PIN policy: min ");
    print_u64_serial((uint64_t)shell_settings.pin_min_len);
    serial_write(" chars");
    if (shell_settings.pin_require_digit) {
        serial_write(", digit required");
    }
    serial_write_line("");
    serial_write("Lockout: ");
    print_u64_serial((uint64_t)shell_settings.lockout_attempts);
    serial_write(" attempts, ");
    print_u64_serial((uint64_t)shell_settings.lockout_seconds);
    serial_write_line(" seconds");
    int allowed[SEC_APP_COUNT];
    size_t len = 0;
    const char *text = vfs_read_ptr("allowlist.txt", &len);
    allowlist_parse(text, len, allowed);
    int denied = 0;
    for (int i = 0; i < SEC_APP_COUNT; i++) {
        if (!allowed[i]) {
            denied++;
        }
    }
    serial_write("Allowlist: ");
    if (denied == 0) {
        serial_write_line("all apps allowed");
    } else {
        print_u64_serial((uint64_t)denied);
        serial_write(" app(s) denied:");
        for (int i = 0; i < SEC_APP_COUNT; i++) {
            if (!allowed[i]) {
                serial_write(" ");
                serial_write(sec_app_name(i));
            }
        }
        serial_write_line("");
    }
    sec_remote_config cfg;
    load_remote_config(&cfg);
    serial_write("Remote lock/wipe: ");
    serial_write(cfg.enabled ? "on" : "off");
    if (cfg.host[0] != '\0') {
        serial_write(" (");
        serial_write(cfg.host);
        serial_write(cfg.path);
        serial_write(")");
    }
    serial_write_line("");
    size_t alen = 0;
    const char *alog = vfs_read_ptr("audit.log", &alen);
    serial_write("Audit log: ");
    print_u64_serial((uint64_t)audit_count(alog, alen));
    serial_write_line(" entries (see `audit`)");
}

static void security_flow(const char *line) {
    const char *arg = skip_spaces(line + 8); /* skip "security" */
    if (arg[0] == '\0' || strcmp(arg, "status") == 0) {
        security_status();
        return;
    }
    if (starts_with_word(arg, "usb")) {
        const char *v = skip_spaces(arg + 3);
        if (strcmp(v, "allow") != 0 && strcmp(v, "block") != 0) {
            serial_write_line("Usage: security usb <allow|block>");
            return;
        }
        if (!admin_check()) {
            return;
        }
        /* Reload before modifying: shell_settings is a
         * boot-time copy, and saving it wholesale would
         * roll back changes the desktop saved since. */
        load_settings(&shell_settings);
        shell_settings.usb_media = strcmp(v, "block") == 0 ? 1 : 0;
        save_settings(&shell_settings);
        audit_log_event("security", v);
        serial_write("External media: ");
        serial_write(v);
        serial_write_line(" (takes effect at next boot).");
        return;
    }
    if (starts_with_word(arg, "pin")) {
        const char *p = skip_spaces(arg + 3);
        uint32_t min_len = 0;
        uint32_t digit = 0;
        char num1[16];
        char num2[16];
        size_t n = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               n + 1 < sizeof(num1)) {
            num1[n++] = *p++;
        }
        num1[n] = '\0';
        p = skip_spaces(p);
        n = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               n + 1 < sizeof(num2)) {
            num2[n++] = *p++;
        }
        num2[n] = '\0';
        if (parse_u32_arg(num1, &min_len) != 0 ||
            parse_u32_arg(num2, &digit) != 0 || min_len < 4 ||
            min_len > 16 || digit > 1) {
            serial_write_line(
                "Usage: security pin <min_len 4-16> <digit 0|1>");
            return;
        }
        if (!admin_check()) {
            return;
        }
        load_settings(&shell_settings);
        shell_settings.pin_min_len = (int)min_len;
        shell_settings.pin_require_digit = (int)digit;
        save_settings(&shell_settings);
        audit_log_event("security", "PIN policy changed");
        serial_write_line("PIN policy updated.");
        return;
    }
    if (starts_with_word(arg, "lockout")) {
        const char *p = skip_spaces(arg + 7);
        uint32_t attempts = 0;
        uint32_t seconds = 0;
        char num1[16];
        char num2[16];
        size_t n = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               n + 1 < sizeof(num1)) {
            num1[n++] = *p++;
        }
        num1[n] = '\0';
        p = skip_spaces(p);
        n = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               n + 1 < sizeof(num2)) {
            num2[n++] = *p++;
        }
        num2[n] = '\0';
        if (parse_u32_arg(num1, &attempts) != 0 ||
            parse_u32_arg(num2, &seconds) != 0 || attempts < 1 ||
            attempts > 10 || seconds > 3600) {
            serial_write_line(
                "Usage: security lockout <attempts 1-10> <seconds 0-3600>");
            return;
        }
        if (!admin_check()) {
            return;
        }
        load_settings(&shell_settings);
        shell_settings.lockout_attempts = (int)attempts;
        shell_settings.lockout_seconds = (int)seconds;
        save_settings(&shell_settings);
        audit_log_event("security", "lockout changed");
        serial_write_line("Lockout updated.");
        return;
    }
    if (starts_with_word(arg, "allow") ||
        starts_with_word(arg, "deny")) {
        int is_allow = starts_with_word(arg, "allow");
        const char *name = skip_spaces(arg + (is_allow ? 5 : 4));
        if (name[0] == '\0') {
            serial_write_line(
                "Usage: security allow <app|all> | security deny <app>");
            return;
        }
        int allowed[SEC_APP_COUNT];
        size_t len = 0;
        const char *text = vfs_read_ptr("allowlist.txt", &len);
        allowlist_parse(text, len, allowed);
        if (!is_allow) {
            int id = sec_app_id(name);
            if (id < 0) {
                serial_write("Unknown app: ");
                serial_write_line(name);
                return;
            }
            if (id == 0 || id == 1 || id == 2) {
                serial_write_line(
                    "Terminal, Files and Settings cannot be denied.");
                return;
            }
        } else if (strcmp(name, "all") != 0 && sec_app_id(name) < 0) {
            serial_write("Unknown app: ");
            serial_write_line(name);
            return;
        }
        if (!admin_check()) {
            return;
        }
        if (is_allow) {
            if (strcmp(name, "all") == 0) {
                for (int i = 0; i < SEC_APP_COUNT; i++) {
                    allowed[i] = 1;
                }
            } else {
                allowed[sec_app_id(name)] = 1;
            }
        } else {
            allowed[sec_app_id(name)] = 0;
        }
        char buf[256];
        size_t m = allowlist_serialize(allowed, buf, sizeof(buf));
        if (vfs_write("allowlist.txt", buf, m) != 0) {
            serial_write_line("security: could not save allowlist.txt.");
            return;
        }
        desktop_reload_allowlist();
        audit_log_event("security", is_allow ? "app allowed"
                                             : "app denied");
        serial_write(is_allow ? "Allowed: " : "Denied: ");
        serial_write_line(name);
        return;
    }
    if (starts_with_word(arg, "remote")) {
        const char *sub = skip_spaces(arg + 6);
        if (starts_with_word(sub, "setup")) {
            const char *p = skip_spaces(sub + 5);
            char host[64];
            char path[128];
            size_t n = 0;
            while (*p != '\0' && *p != ' ' && *p != '\t' &&
                   n + 1 < sizeof(host)) {
                host[n++] = *p++;
            }
            host[n] = '\0';
            p = skip_spaces(p);
            n = 0;
            while (*p != '\0' && *p != ' ' && *p != '\t' &&
                   n + 1 < sizeof(path)) {
                path[n++] = *p++;
            }
            path[n] = '\0';
            if (host[0] == '\0') {
                serial_write_line(
                    "Usage: security remote setup <host> [path]");
                return;
            }
            char pin[PIN_CAP];
            if (!admin_check_pin(pin, sizeof(pin))) {
                return;
            }
            char token[65];
            remote_token_for_pin(pin, token);
            for (size_t i = 0; pin[i] != '\0'; i++) {
                pin[i] = 0;
            }
            sec_remote_config cfg;
            remote_config_defaults(&cfg);
            size_t h = 0;
            while (host[h] != '\0' && h + 1 < sizeof(cfg.host)) {
                cfg.host[h] = host[h];
                h++;
            }
            cfg.host[h] = '\0';
            if (path[0] != '\0') {
                size_t q = 0;
                while (path[q] != '\0' && q + 1 < sizeof(cfg.path)) {
                    cfg.path[q] = path[q];
                    q++;
                }
                cfg.path[q] = '\0';
            }
            size_t t = 0;
            while (token[t] != '\0' && t + 1 < sizeof(cfg.token)) {
                cfg.token[t] = token[t];
                t++;
            }
            cfg.token[t] = '\0';
            cfg.enabled = 1;
            save_remote_config(&cfg);
            audit_log_event("security", "remote setup");
            serial_write("Remote lock/wipe: on (");
            serial_write(cfg.host);
            serial_write(cfg.path);
            serial_write_line(").");
            return;
        }
        if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
            if (!admin_check()) {
                return;
            }
            sec_remote_config cfg;
            load_remote_config(&cfg);
            cfg.enabled = strcmp(sub, "on") == 0 ? 1 : 0;
            save_remote_config(&cfg);
            audit_log_event("security", sub);
            serial_write("Remote lock/wipe: ");
            serial_write_line(sub);
            return;
        }
        if (strcmp(sub, "check") == 0) {
            if (remote_check_once() == 0) {
                serial_write_line("Remote check: no action.");
            }
            return;
        }
        serial_write_line(
            "Usage: security remote setup <host> [path] | security remote <on|off|check>");
        return;
    }
    serial_write_line("Usage: security | security usb <allow|block> | security pin <min_len> <digit> | security lockout <attempts> <seconds> | security allow <app|all> | security deny <app> | security remote setup <host> [path] | security remote <on|off|check>");
}

/* `audit` / `audit verify`: 監査ログを見る・検証する。 */
static void audit_flow(const char *line) {
    const char *arg = skip_spaces(line + 5); /* skip "audit" */
    if (persist_name_pending_decrypt("audit.log")) {
        serial_write_line(
            "Audit log is sealed (encrypted) until a successful login.");
        return;
    }
    size_t len = 0;
    const char *log = vfs_read_ptr("audit.log", &len);
    if (strcmp(arg, "verify") == 0) {
        int count = 0;
        int bad = 0;
        int rc = audit_verify(log, len, &count, &bad);
        if (rc == 0) {
            serial_write("Audit log: ");
            print_u64_serial((uint64_t)count);
            serial_write_line(" entries, chain OK.");
        } else if (rc == -1) {
            serial_write("Audit log: entry ");
            print_u64_serial((uint64_t)bad);
            serial_write_line(" is broken (tampered?).");
        } else {
            serial_write_line("Audit log: format is broken.");
        }
        return;
    }
    if (log == NULL || len == 0) {
        serial_write_line("No audit entries yet.");
        return;
    }
    /* 後ろの最大 10 件だけ出す。 */
    int total = audit_count(log, len);
    int skip = total > 10 ? total - 10 : 0;
    int shown = 0;
    size_t pos = 0;
    int idx = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && log[eol] != '\n') {
            eol++;
        }
        if (eol == pos) {
            break;
        }
        if (idx >= skip) {
            for (size_t i = pos; i < eol; i++) {
                serial_write_char(log[i]);
            }
            serial_write_char('\n');
            shown++;
        }
        idx++;
        pos = eol + 1;
    }
    if (shown == 0) {
        serial_write_line("No audit entries yet.");
    }
}

void shell_reboot_now(void) {
    serial_write_line("Rebooting PrismOS...");
    /* キーボードコントローラ経由でリセットを要求する。
     * 効かなければ hlt ループに落ち、QEMU の -no-reboot が
     * あれば終了する。 */
    prism_outb(0x64, 0xFE);
    prism_halt();
}

/* ---- 環境変数 (set / env / unset、$NAME 展開) ----
 * 値はメモリ上だけに持ち、再起動で消える (設定ファイルには
 * 書かない)。展開は通常コマンドの実行直前にだけ行い、
 * mail や pq のような専用フローの引数には触らない。 */
#define ENV_MAX 16
static char env_names[ENV_MAX][32];
static char env_vals[ENV_MAX][128];
static int env_count;

static const char *env_get(const char *name, size_t nlen) {
    for (int i = 0; i < env_count; i++) {
        size_t j = 0;
        while (j < nlen && env_names[i][j] != '\0' &&
               env_names[i][j] == name[j]) {
            j++;
        }
        if (j == nlen && env_names[i][nlen] == '\0') {
            return env_vals[i];
        }
    }
    return NULL;
}

static void env_set(const char *name, size_t nlen, const char *val) {
    for (int i = 0; i < env_count; i++) {
        size_t j = 0;
        while (j < nlen && env_names[i][j] != '\0' &&
               env_names[i][j] == name[j]) {
            j++;
        }
        if (j == nlen && env_names[i][nlen] == '\0') {
            size_t k = 0;
            while (val[k] != '\0' && k + 1 < sizeof(env_vals[0])) {
                env_vals[i][k] = val[k];
                k++;
            }
            env_vals[i][k] = '\0';
            return;
        }
    }
    if (env_count >= ENV_MAX || nlen + 1 > sizeof(env_names[0])) {
        serial_write_line("env: too many variables");
        return;
    }
    int slot = env_count++;
    for (size_t j = 0; j < nlen; j++) {
        env_names[slot][j] = name[j];
    }
    env_names[slot][nlen] = '\0';
    size_t k = 0;
    while (val[k] != '\0' && k + 1 < sizeof(env_vals[0])) {
        env_vals[slot][k] = val[k];
        k++;
    }
    env_vals[slot][k] = '\0';
}

static void env_unset(const char *name, size_t nlen) {
    for (int i = 0; i < env_count; i++) {
        size_t j = 0;
        while (j < nlen && env_names[i][j] != '\0' &&
               env_names[i][j] == name[j]) {
            j++;
        }
        if (j == nlen && env_names[i][nlen] == '\0') {
            for (int m = i + 1; m < env_count; m++) {
                for (size_t k = 0; k < sizeof(env_names[0]); k++) {
                    env_names[m - 1][k] = env_names[m][k];
                }
                for (size_t k = 0; k < sizeof(env_vals[0]); k++) {
                    env_vals[m - 1][k] = env_vals[m][k];
                }
            }
            env_count--;
            return;
        }
    }
}

/* $NAME と ${NAME} を展開する。未知の変数は空になる。 */
static void env_expand(const char *line, char *dst, size_t cap) {
    size_t w = 0;
    size_t i = 0;
    while (line[i] != '\0' && w + 1 < cap) {
        if (line[i] == '$' && line[i + 1] != '\0' &&
            line[i + 1] != ' ' && line[i + 1] != '\t') {
            size_t nstart = i + 1;
            size_t nlen = 0;
            int brace = 0;
            if (line[nstart] == '{') {
                brace = 1;
                nstart++;
            }
            while (line[nstart + nlen] != '\0' &&
                   line[nstart + nlen] != ' ' &&
                   line[nstart + nlen] != '\t' &&
                   line[nstart + nlen] != '$' &&
                   (!brace || line[nstart + nlen] != '}') &&
                   (brace || ((line[nstart + nlen] >= 'A' &&
                               line[nstart + nlen] <= 'Z') ||
                              (line[nstart + nlen] >= 'a' &&
                               line[nstart + nlen] <= 'z') ||
                              (line[nstart + nlen] >= '0' &&
                               line[nstart + nlen] <= '9') ||
                              line[nstart + nlen] == '_'))) {
                nlen++;
            }
            if (nlen > 0) {
                const char *v = env_get(line + nstart, nlen);
                if (v != NULL) {
                    size_t k = 0;
                    while (v[k] != '\0' && w + 1 < cap) {
                        dst[w++] = v[k++];
                    }
                }
                i = nstart + nlen + (brace ? 1 : 0);
                continue;
            }
        }
        dst[w++] = line[i++];
    }
    dst[w] = '\0';
}

/* ---- スクリプト (script <file>) ----
 * ファイルの各行をコマンドとしてキューに入れ、メイン
 * ループが順に実行する。入れ子は禁止 (キューが空でない
 * 間は追加できない)。'#' で始まる行と空行は飛ばす。 */
#define SCRIPT_MAX_LINES 32
static char script_queue[SCRIPT_MAX_LINES][512];
static int script_count;
static int script_head;

static int script_pop(char *dst, size_t cap) {
    if (script_head >= script_count) {
        script_count = 0;
        script_head = 0;
        return 0;
    }
    const char *src = script_queue[script_head++];
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return 1;
}

static void script_flow(const char *line) {
    const char *name = skip_spaces(line + 6); /* skip "script" */
    if (name[0] == '\0') {
        serial_write_line("Usage: script <file>");
        return;
    }
    if (script_count > script_head) {
        serial_write_line("script: already running (nesting is not supported)");
        return;
    }
    size_t len = 0;
    const char *data = vfs_read_ptr(name, &len);
    if (data == NULL) {
        serial_write_line("script: file not found");
        return;
    }
    script_count = 0;
    script_head = 0;
    size_t i = 0;
    while (i < len && script_count < SCRIPT_MAX_LINES) {
        size_t j = i;
        while (j < len && data[j] != '\n') {
            j++;
        }
        size_t n = j - i;
        while (n > 0 && (data[i + n - 1] == '\r' ||
                         data[i + n - 1] == ' ')) {
            n--;
        }
        size_t off = 0;
        while (off < n && data[i + off] == ' ') {
            off++;
        }
        if (n > off && data[i + off] != '#') {
            size_t w = 0;
            while (off < n && w + 1 < sizeof(script_queue[0])) {
                script_queue[script_count][w++] = data[i + off++];
            }
            script_queue[script_count][w] = '\0';
            script_count++;
        }
        i = j + 1;
    }
    serial_write("script: queued ");
    print_u64_serial((uint64_t)script_count);
    serial_write_line(" lines");
}

/* テキストブロックをシリアルと画面の両方に出す。 */
static void print_block(const char *text) {
    if (text[0] == '\0') {
        return;
    }
    serial_write(text);
    desktop_console_write(text);
    size_t n = 0;
    while (text[n] != '\0') {
        n++;
    }
    if (n > 0 && text[n - 1] != '\n') {
        serial_write_char('\n');
        desktop_console_putc('\n');
    }
    desktop_refresh_console();
}

/* ---- パイプ (cmd | grep/tee/run/cat) ----
 * 左側は通常コマンド (出力を捕捉できるもの) だけ対応。
 * 右側は grep <語>、tee <file>、run <guest>、cat の
 * いずれか。専用フロー (mail 等) は左側に置けない。 */
static int pipe_is_flow(const char *left) {
    static const char *const flows[] = {
        "security", "audit", "fde", "snapshot", "update", "mail", "pq",
        "bio", "driver", "compat", "run", "install", "lock",
        "recovery", "script", "history", "set", "env", "unset",
        "play", "mem",
    };
    for (size_t i = 0; i < sizeof(flows) / sizeof(flows[0]); i++) {
        if (starts_with_word(left, flows[i])) {
            return 1;
        }
    }
    return 0;
}

static void pipe_flow(prism_shell_state *state, char *line,
                      char *out) {
    char *bar = NULL;
    for (size_t i = 0; line[i] != '\0'; i++) {
        if (line[i] == '|' && i > 0 && line[i - 1] == ' ' &&
            line[i + 1] == ' ') {
            bar = line + i;
            break;
        }
    }
    if (bar == NULL) {
        return;
    }
    *bar = '\0';
    const char *left = line;
    const char *right = skip_spaces(bar + 2);
    if (pipe_is_flow(left)) {
        serial_write_line("pipe: the left side must be a plain shell command");
        return;
    }
    prism_shell_execute(state, left, out, OUT_CAP);
    if (line_eq(right, "cat")) {
        print_block(out);
        return;
    }
    if (starts_with_word(right, "grep")) {
        const char *pat = skip_spaces(right + 4);
        if (pat[0] == '\0') {
            serial_write_line("Usage: ... | grep <word>");
            return;
        }
        size_t plen = 0;
        while (pat[plen] != '\0') {
            plen++;
        }
        const char *p = out;
        while (*p != '\0') {
            const char *eol = p;
            while (*eol != '\0' && *eol != '\n') {
                eol++;
            }
            size_t llen = (size_t)(eol - p);
            int found = 0;
            if (llen >= plen) {
                for (size_t k = 0; k + plen <= llen; k++) {
                    size_t m = 0;
                    while (m < plen && p[k + m] == pat[m]) {
                        m++;
                    }
                    if (m == plen) {
                        found = 1;
                        break;
                    }
                }
            }
            if (found) {
                for (size_t k = 0; k < llen; k++) {
                    serial_write_char(p[k]);
                    desktop_console_putc(p[k]);
                }
                serial_write_char('\n');
                desktop_console_putc('\n');
            }
            p = (*eol == '\n') ? eol + 1 : eol;
        }
        desktop_refresh_console();
        return;
    }
    if (starts_with_word(right, "tee")) {
        const char *file = skip_spaces(right + 3);
        if (file[0] == '\0') {
            serial_write_line("Usage: ... | tee <file>");
            return;
        }
        size_t n = 0;
        while (out[n] != '\0') {
            n++;
        }
        if (vfs_write(file, out, n) == 0) {
            print_block(out);
        } else {
            serial_write_line("tee: write failed");
        }
        return;
    }
    if (starts_with_word(right, "run")) {
        size_t n = 0;
        while (out[n] != '\0') {
            n++;
        }
        compat_set_stdin(out, n);
        run_flow(right);
        compat_set_stdin(NULL, 0);
        return;
    }
    serial_write_line("pipe: the right side must be grep <word>, tee <file>, run <guest>, or cat");
}

/* ---- Prism Store (store コマンド) ----
 * 信頼の根は store.pub (ML-DSA-44 公開鍵)。パッケージ
 * (shell/storepkg.c の形式) は署名検証を通ったもの
 * だけを導入する。store keygen / store sign は
 * 開発用 (本来の署名は OS の外で行う前提で、
 * store pubkey <hex> で信頼の根を差し替えられる)。 */


static int store_hexval(char c) {
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

static uint8_t store_pk[MLDSA44_PK_LEN];
static uint8_t store_sk[MLDSA44_SK_LEN];
static uint8_t store_sig[MLDSA44_SIG_LEN];
static uint8_t store_pkg_buf[20480];
static uint8_t store_payload_buf[PRISM_VFS_MAX_FILE_SIZE];

static int store_load_pub(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr("store.pub", &len);
    if (text == NULL || len != MLDSA44_PK_LEN) {
        return -1;
    }
    memcpy(store_pk, text, MLDSA44_PK_LEN);
    return 0;
}

/* 信頼ルートの指紋 (SHA-256 全長 64 桁) を出す。 */
static void store_print_fingerprint(const uint8_t *pk) {
    uint8_t digest[32];
    crypto_sha256(pk, MLDSA44_PK_LEN, digest);
    static const char *H = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = H[digest[i] >> 4];
        hex[i * 2 + 1] = H[digest[i] & 15];
    }
    hex[64] = '\0';
    serial_write_line(hex);
}

static void store_flow(char *line) {
    if (guest_session) {
        serial_write_line("Guests cannot use the store.");
        return;
    }
    const char *arg = skip_spaces(line + 5);
    if (line_eq(arg, "keygen")) {
        if (!current_is_admin()) {
            serial_write_line("Admin only.");
            return;
        }
        if (vfs_exists("store.pub")) {
            serial_write_line("store: key already exists.");
            return;
        }
        uint8_t seed[MLDSA44_SEED_LEN];
        entropy_reseed();
        entropy_bytes(seed, sizeof(seed));
        if (mldsa44_keygen_from_seed(seed, store_pk, store_sk) != 0) {
            serial_write_line("store: keygen failed.");
            return;
        }
        if (vfs_write("store.pub", (const char *)store_pk,
                      MLDSA44_PK_LEN) == 0 &&
            vfs_write("store.key", (const char *)store_sk,
                      MLDSA44_SK_LEN) == 0) {
            persist_maybe_sync();
            serial_write_line("store: key pair created "
                              "(store.pub / store.key).");
        } else {
            serial_write_line("store: could not save the key pair.");
        }
        return;
    }
    if (starts_with_word(arg, "pubkey fetch")) {
        /* 信頼の根の差し替えは管理者専用。平文 HTTP で取る
         * ため、取得した鍵の指紋 (全長) を示して明示的な
         * 確認を取るまでは書き込まない (監査 F3)。 */
        if (!current_is_admin()) {
            serial_write_line("Admin only.");
            return;
        }
        /* store pubkey fetch <host> <port> <path> */
        const char *p = skip_spaces(arg + 12);
        char host[64];
        size_t q = 0;
        while (p[q] != '\0' && p[q] != ' ' && q + 1 < sizeof(host)) {
            host[q] = p[q];
            q++;
        }
        host[q] = '\0';
        p = skip_spaces(p + q);
        int port = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p - '0');
            p++;
        }
        p = skip_spaces(p);
        char path[96];
        q = 0;
        while (p[q] != '\0' && p[q] != ' ' && q + 1 < sizeof(path)) {
            path[q] = p[q];
            q++;
        }
        path[q] = '\0';
        if (host[0] == '\0' || port <= 0 || path[0] == '\0') {
            serial_write_line("Usage: store pubkey fetch <host> "
                              "<port> <path>");
            return;
        }
        static uint8_t pkbuf[MLDSA44_PK_LEN + 1024];
        size_t got = 0;
        if (net_fetch_http_px(host, port, path, (char *)pkbuf,
                              sizeof(pkbuf), &got, NULL) != 0 ||
            got != MLDSA44_PK_LEN) {
            serial_write_line("store: pubkey fetch failed.");
            return;
        }
        serial_write("store: fetched pubkey sha256: ");
        store_print_fingerprint(pkbuf);
        if (vfs_exists("store.pub")) {
            serial_write_line("store: a trust root is already set; "
                              "this would replace it.");
        }
        serial_write("Type yes to trust this key: ");
        char ans[16];
        read_line_merged(ans, sizeof(ans));
        if (strcmp(ans, "yes") != 0) {
            serial_write_line("store: fetch aborted; "
                              "trust root unchanged.");
            audit_log_event("store-trust", "pubkey fetch declined");
            return;
        }
        if (vfs_write("store.pub", (const char *)pkbuf,
                      MLDSA44_PK_LEN) == 0) {
            persist_maybe_sync();
            serial_write_line("store: trust root updated (fetched).");
            audit_log_event("store-trust", "pubkey fetch accepted");
        } else {
            serial_write_line("store: could not save store.pub.");
        }
        return;
    }
    if (starts_with_word(arg, "pubkey")) {
        if (!current_is_admin()) {
            serial_write_line("Admin only.");
            return;
        }
        const char *p = skip_spaces(arg + 6);
        if (p[0] == '\0') {
            if (store_load_pub() != 0) {
                serial_write_line("store: no trust root set.");
                return;
            }
            serial_write("store pubkey sha256: ");
            store_print_fingerprint(store_pk);
            return;
        }
        /* hex で信頼の根を設定する (管理者のプロビジョニング)。 */
        if (!current_is_admin()) {
            serial_write_line("Admin only.");
            return;
        }
        size_t need = MLDSA44_PK_LEN * 2;
        size_t n = 0;
        while (p[n] != '\0') {
            n++;
        }
        if (n != need) {
            serial_write_line("store: pubkey hex has wrong length.");
            return;
        }
        for (size_t i = 0; i < MLDSA44_PK_LEN; i++) {
            int hi = store_hexval(p[i * 2]);
            int lo = store_hexval(p[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                serial_write_line("store: pubkey is not hex.");
                return;
            }
            store_pk[i] = (uint8_t)(hi * 16 + lo);
        }
        if (vfs_write("store.pub", (const char *)store_pk,
                      MLDSA44_PK_LEN) == 0) {
            persist_maybe_sync();
            serial_write_line("store: trust root updated.");
        } else {
            serial_write_line("store: could not save store.pub.");
        }
        return;
    }
    if (starts_with_word(arg, "server")) {
        /* store server <host> <port> <base> — store.conf を書く。 */
        const char *p = skip_spaces(arg + 6);
        char conf[160];
        size_t q = 0;
        while (p[q] != '\0' && q + 1 < sizeof(conf)) {
            conf[q] = p[q];
            q++;
        }
        conf[q] = '\0';
        if (q == 0) {
            serial_write_line("Usage: store server <host> <port> "
                              "<base>");
            return;
        }
        if (vfs_write("store.conf", conf, q) == 0) {
            persist_maybe_sync();
            serial_write_line("store: server configured.");
        } else {
            serial_write_line("store: could not save store.conf.");
        }
        return;
    }
    if (starts_with_word(arg, "sign")) {
        /* store sign <elf> <pkg> — 開発用。署名鍵を使うので
         * 管理者専用 (監査 F3)。 */
        if (!current_is_admin()) {
            serial_write_line("Admin only.");
            return;
        }
        size_t len = 0;
        const char *key = vfs_read_ptr("store.key", &len);
        if (key == NULL || len != MLDSA44_SK_LEN) {
            serial_write_line("store: no store.key (run store keygen).");
            return;
        }
        memcpy(store_sk, key, MLDSA44_SK_LEN);
        const char *p = skip_spaces(arg + 4);
        char elf[32], pkgname[32];
        size_t q = 0;
        while (p[q] != '\0' && p[q] != ' ' && q + 1 < sizeof(elf)) {
            elf[q] = p[q];
            q++;
        }
        elf[q] = '\0';
        p = skip_spaces(p + q);
        q = 0;
        while (p[q] != '\0' && p[q] != ' ' && q + 1 < sizeof(pkgname)) {
            pkgname[q] = p[q];
            q++;
        }
        pkgname[q] = '\0';
        size_t elen = 0;
        const char *edata = vfs_read_ptr(elf, &elen);
        if (edata == NULL || pkgname[0] == '\0') {
            serial_write_line("Usage: store sign <elf> <pkg>");
            return;
        }
        if (mldsa44_sign(store_sk, (const uint8_t *)edata, elen,
                         store_sig) != 0) {
            serial_write_line("store: signing failed.");
            return;
        }
        /* 名前はファイル名から .elf を除いたもの。 */
        char name[STOREPKG_NAME_MAX + 1];
        size_t nn = 0;
        while (elf[nn] != '\0' && elf[nn] != '.' &&
               nn < STOREPKG_NAME_MAX) {
            name[nn] = elf[nn];
            nn++;
        }
        name[nn] = '\0';
        size_t out_len = 0;
        if (storepkg_build(name, elf, "1.0", (const uint8_t *)edata,
                           elen, store_sig, store_pkg_buf,
                           sizeof(store_pkg_buf), &out_len) != 0) {
            serial_write_line("store: could not build the package.");
            return;
        }
        if (vfs_write(pkgname, (const char *)store_pkg_buf,
                      out_len) == 0) {
            persist_maybe_sync();
            serial_write("store: signed package written to ");
            serial_write_line(pkgname);
        } else {
            serial_write_line("store: package too large for the VFS.");
        }
        return;
    }
    if (starts_with_word(arg, "install")) {
        const char *p = skip_spaces(arg + 7);
        char target[40];
        size_t q = 0;
        while (p[q] != '\0' && p[q] != ' ' && q + 1 < sizeof(target)) {
            target[q] = p[q];
            q++;
        }
        target[q] = '\0';
        if (target[0] == '\0') {
            serial_write_line("Usage: store install <pkg file or "
                              "catalog name>");
            return;
        }
        size_t len = 0;
        const char *data = vfs_read_ptr(target, &len);
        if (data != NULL) {
            char err[64];
            if (prism_store_install_mem((const uint8_t *)data, len, err,
                                  sizeof(err)) != 0) {
                serial_write("store: install failed: ");
                serial_write_line(err);
            }
            return;
        }
        /* カタログから名前で探して取得する。 */
        char host[64], base[64];
        int port = 0;
        if (prism_store_server(host, sizeof(host), &port, base,
                         sizeof(base)) != 0) {
            serial_write_line("store: no such file, and no store "
                              "server configured (store.conf).");
            return;
        }
        static char cat[4096];
        char url[160];
        size_t up = 0;
        while (base[up] != '\0' && up + 1 < sizeof(url)) {
            url[up] = base[up];
            up++;
        }
        const char *cf = "catalog.txt";
        size_t cq = 0;
        if (up > 0 && url[up - 1] != '/' && up + 1 < sizeof(url)) {
            url[up++] = '/';
        }
        while (cf[cq] != '\0' && up + 1 < sizeof(url)) {
            url[up++] = cf[cq++];
        }
        url[up] = '\0';
        size_t clen = 0;
        if (net_fetch_http_px(host, port, url, cat, sizeof(cat),
                              &clen, NULL) != 0) {
            serial_write_line("store: catalog fetch failed.");
            return;
        }
        cat[clen] = '\0';
        /* 行: name version pkgpath */
        const char *pkgpath = NULL;
        size_t pkgpath_len = 0;
        size_t off = 0;
        while (off < clen) {
            size_t eol = off;
            while (eol < clen && cat[eol] != '\n') {
                eol++;
            }
            /* 先頭の単語が target と一致するか */
            size_t wlen = 0;
            while (off + wlen < eol && cat[off + wlen] != ' ') {
                wlen++;
            }
            size_t tlen2 = 0;
            while (target[tlen2] != '\0') {
                tlen2++;
            }
            if (wlen == tlen2 &&
                memcmp(cat + off, target, wlen) == 0) {
                /* 3 番目の単語 */
                size_t i = off;
                int words = 0;
                while (i < eol) {
                    while (i < eol && cat[i] == ' ') {
                        i++;
                    }
                    size_t st = i;
                    while (i < eol && cat[i] != ' ') {
                        i++;
                    }
                    words++;
                    if (words == 3) {
                        pkgpath = cat + st;
                        pkgpath_len = i - st;
                    }
                }
                break;
            }
            off = eol + 1;
        }
        if (pkgpath == NULL) {
            serial_write_line("store: not in the catalog.");
            return;
        }
        char purl[192];
        size_t pp = 0;
        if (pkgpath[0] != '/') {
            while (base[pp] != '\0' && pp + 1 < sizeof(purl)) {
                purl[pp] = base[pp];
                pp++;
            }
            if (pp > 0 && purl[pp - 1] != '/' &&
                pp + 1 < sizeof(purl)) {
                purl[pp++] = '/';
            }
        }
        for (size_t i = 0; i < pkgpath_len && pp + 1 < sizeof(purl);
             i++) {
            purl[pp++] = pkgpath[i];
        }
        purl[pp] = '\0';
        size_t pklen = 0;
        if (net_fetch_http_px(host, port, purl,
                              (char *)store_pkg_buf,
                              sizeof(store_pkg_buf), &pklen,
                              NULL) != 0) {
            serial_write_line("store: package fetch failed.");
            return;
        }
        char err[64];
        if (prism_store_install_mem(store_pkg_buf, pklen, err,
                              sizeof(err)) != 0) {
            serial_write("store: install failed: ");
            serial_write_line(err);
        }
        return;
    }
    if (starts_with_word(arg, "uninstall")) {
        const char *p = skip_spaces(arg + 9);
        char name[STOREPKG_NAME_MAX + 1];
        size_t q = 0;
        while (p[q] != '\0' && p[q] != ' ' &&
               q < STOREPKG_NAME_MAX) {
            name[q] = p[q];
            q++;
        }
        name[q] = '\0';
        if (prism_store_uninstall(name) == 0) {
            serial_write("store: uninstalled ");
            serial_write_line(name);
        } else {
            serial_write_line("store: not installed.");
        }
        return;
    }
    if (line_eq(arg, "list")) {
        size_t tlen = 0;
        const char *text = vfs_read_ptr("store-installed.txt", &tlen);
        serial_write_line("Installed:");
        if (text == NULL || tlen == 0) {
            serial_write_line("  (none)");
        } else {
            size_t off = 0;
            while (off < tlen) {
                size_t eol = off;
                while (eol < tlen && text[eol] != '\n') {
                    eol++;
                }
                char line[96];
                size_t ll = eol - off;
                if (ll + 1 > sizeof(line)) {
                    ll = sizeof(line) - 1;
                }
                memcpy(line, text + off, ll);
                line[ll] = '\0';
                serial_write("  ");
                serial_write_line(line);
                off = eol + 1;
            }
        }
        char host[64], base[64];
        int port = 0;
        if (prism_store_server(host, sizeof(host), &port, base,
                         sizeof(base)) != 0) {
            serial_write_line("Catalog: (no server configured; "
                              "write store.conf)");
            return;
        }
        static char cat[4096];
        char url[160];
        size_t up = 0;
        while (base[up] != '\0' && up + 1 < sizeof(url)) {
            url[up] = base[up];
            up++;
        }
        if (up > 0 && url[up - 1] != '/' && up + 1 < sizeof(url)) {
            url[up++] = '/';
        }
        const char *cf = "catalog.txt";
        size_t cq = 0;
        while (cf[cq] != '\0' && up + 1 < sizeof(url)) {
            url[up++] = cf[cq++];
        }
        url[up] = '\0';
        size_t clen = 0;
        if (net_fetch_http_px(host, port, url, cat, sizeof(cat),
                              &clen, NULL) != 0) {
            serial_write_line("Catalog: fetch failed.");
            return;
        }
        cat[clen] = '\0';
        serial_write_line("Catalog:");
        size_t off = 0;
        while (off < clen) {
            size_t eol = off;
            while (eol < clen && cat[eol] != '\n') {
                eol++;
            }
            char line[128];
            size_t ll = eol - off;
            if (ll > 0 && cat[off + ll - 1] == '\r') {
                ll--;
            }
            if (ll + 1 > sizeof(line)) {
                ll = sizeof(line) - 1;
            }
            memcpy(line, cat + off, ll);
            line[ll] = '\0';
            if (ll > 0) {
                serial_write("  ");
                serial_write_line(line);
            }
            off = eol + 1;
        }
        return;
    }
    serial_write_line("Usage: store list | store install <pkg|name> "
                      "| store uninstall <name> | store keygen | "
                      "store sign <elf> <pkg> | store pubkey [hex]");
}



/* 全ディスク暗号化 (FDE) の管理コマンド。
 * docs/FDE_DESIGN.md。 */
static void fde_flow(const char *line) {
    const char *arg = skip_spaces(line + 3);
    if (line_eq(line, "fde") || strcmp(arg, "status") == 0) {
        if (!persist_disk_present()) {
            serial_write_line("fde: no disk.");
            return;
        }
        if (!persist_fde_present()) {
            serial_write_line("Full-disk encryption: off");
            return;
        }
        serial_write_line(persist_fde_unlocked()
                              ? "Full-disk encryption: on (unlocked)"
                              : "Full-disk encryption: on (locked)");
        int mig = persist_fde_migration_pending();
        if (mig == 1) {
            serial_write_line(
                "Encryption setup is incomplete; it resumes "
                "at the next unlock.");
        }
        if (mig == 2) {
            serial_write_line(
                "Encryption removal is incomplete; it resumes "
                "at the next unlock.");
        }
        return;
    }
    if (strcmp(arg, "enable") == 0) {
        if (!current_is_admin()) {
            serial_write_line("fde: administrators only.");
            return;
        }
        if (!persist_installed()) {
            serial_write_line(
                "fde: PrismOS is not installed on the disk.");
            return;
        }
        if (persist_fde_present()) {
            serial_write_line("fde: already enabled.");
            return;
        }
        if (!persist_key_ready()) {
            serial_write_line(
                "fde: log in first (the disk key must be active).");
            return;
        }
        char p1[64], p2[64];
        serial_write("New disk passphrase (8+ characters): ");
        read_secret(p1, sizeof(p1));
        if (strlen(p1) < 8) {
            serial_write_line(
                "fde: passphrase too short (8+ characters).");
            memset(p1, 0, sizeof(p1));
            return;
        }
        serial_write("Confirm passphrase: ");
        read_secret(p2, sizeof(p2));
        int match = strcmp(p1, p2) == 0;
        memset(p2, 0, sizeof(p2));
        if (!match) {
            serial_write_line("fde: passphrases do not match.");
            memset(p1, 0, sizeof(p1));
            return;
        }
        char recovery[25];
        serial_write_line(
            "Encrypting the disk. This can take a while...");
        int rc = persist_fde_enable(p1, recovery, sizeof(recovery));
        memset(p1, 0, sizeof(p1));
        if (rc != 0) {
            serial_write_line("fde: could not enable encryption.");
            return;
        }
        serial_write_line("Full-disk encryption enabled.");
        serial_write(
            "Recovery key (write it down and keep it safe): ");
        serial_write_line(recovery);
        memset(recovery, 0, sizeof(recovery));
        audit_log_event("fde-enable",
                        "full-disk encryption enabled");
        return;
    }
    if (strcmp(arg, "disable") == 0) {
        if (!current_is_admin()) {
            serial_write_line("fde: administrators only.");
            return;
        }
        if (!persist_fde_present()) {
            serial_write_line("fde: not enabled.");
            return;
        }
        if (!persist_fde_unlocked()) {
            serial_write_line("fde: disk is locked.");
            return;
        }
        char p1[64];
        serial_write("Disk passphrase: ");
        read_secret(p1, sizeof(p1));
        int ok = persist_fde_verify(p1);
        memset(p1, 0, sizeof(p1));
        if (!ok) {
            serial_write_line("fde: wrong passphrase.");
            return;
        }
        serial_write_line(
            "Decrypting the disk. This can take a while...");
        if (persist_fde_disable() != 0) {
            serial_write_line("fde: could not remove encryption.");
            return;
        }
        serial_write_line("Full-disk encryption disabled.");
        audit_log_event("fde-disable",
                        "full-disk encryption removed");
        return;
    }
    serial_write_line("Usage: fde [status] | fde enable | fde disable");
}

/* FDE のロック解除フロー。shell_run が persist_load() の
 * 前に呼ぶ。解除できたら 1、できなければ 0 (ディスク無し
 * で起動を続け、ディスクのファイルは読めない)。 */
static int fde_unlock_flow(void) {
    serial_write_line("");
    serial_write_line("This disk is encrypted (full-disk encryption).");
    if (persist_fde_migration_pending() != 0) {
        serial_write_line(
            "Encryption setup was interrupted and resumes "
            "after unlock.");
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        char pass[64];
        serial_write("Disk passphrase: ");
        read_secret(pass, sizeof(pass));
        int rc = persist_fde_try_unlock(pass);
        memset(pass, 0, sizeof(pass));
        if (rc == 2) {
            serial_write_line("Resuming encryption setup...");
            rc = persist_fde_resume_migration() == 0 ? 1 : -1;
            if (rc < 0) {
                serial_write_line(
                    "Encryption setup could not finish. "
                    "Disk stays locked.");
                return 0;
            }
        }
        if (rc == 1) {
            serial_write_line("Disk unlocked.");
            return 1;
        }
        serial_write_line("Wrong passphrase.");
    }
    serial_write_line(
        "Disk stays locked; files on disk are unavailable.");
    return 0;
}

void shell_run(void) {
    prism_shell_state state;
    char line[LINE_CAP];
    /* 出力バッファはヒープから取る (固定配列だけに頼らない)。
     * ヒープが使えない場合は静的な予備バッファに落とす。 */
    static char out_fallback[OUT_CAP];
    char *out = kmalloc(OUT_CAP);
    if (out == NULL) {
        out = out_fallback;
    }

    prism_shell_init(&state);
    /* エントロピープールを初期化し、ハードウェア乱数が
     * あれば混ぜる。キーボード・マウスの割り込みも、以後は
     * プールに混ざる。 */
    entropy_init();
    {
        uint64_t hw = 0;
        if (entropy_hw_random(&hw) == 0) {
            entropy_add((uint32_t)hw);
            entropy_add((uint32_t)(hw >> 32));
            serial_write_line("[boot] entropy: RDRAND available");
        } else {
            serial_write_line(
                "[boot] entropy: no RDRAND, using event pool");
        }
        entropy_add(entropy_hw_tick());
        prism_rtc_time rt;
        if (rtc_read(&rt) == 0) {
            entropy_add((uint32_t)rtc_epoch_seconds(&rt));
        }
        entropy_add((uint32_t)pmm_total_pages());
    }
    /* 監査ログの書き込み先を登録する (以後の login や
     * security の操作が audit.log に残る)。 */
    audit_set_sink(shell_audit_sink);
    /* 全ディスク暗号化: ヘッダがあれば、ディスクを読む前に
     * パスフレーズでロックを解除する。 */
    int fde_boot_unlocked = 0;
    if (persist_fde_present() && !persist_fde_unlocked()) {
        fde_boot_unlocked = fde_unlock_flow();
    }
    /* インストール済みならディスクからファイルを載せる。
     * 暗号化されたファイルは、この時点では暗号文のまま。 */
    persist_load();
    if (fde_boot_unlocked) {
        audit_log_event("fde-unlock", "unlocked at boot");
    }
    /* 設定を読む (ログインのロック回数や PIN ポリシーに使う)。 */
    load_settings(&shell_settings);
    if (shell_settings.net_static) {
        uint8_t ip[4];
        uint8_t mask[4];
        uint8_t gw[4];
        uint8_t dns[4];
        if (net_parse_ipv4(shell_settings.net_ip, ip) == 0 &&
            net_parse_ipv4(shell_settings.net_mask, mask) == 0 &&
            net_parse_ipv4(shell_settings.net_gw, gw) == 0 &&
            net_parse_ipv4(shell_settings.net_dns, dns) == 0) {
            net_set_static_config(ip, mask, gw, dns);
        }
    }
    state.language = shell_settings.language;
    keyboard_set_layout(shell_settings.kbd_layout);
    {
        /* 前回の更新が終わらないまま再起動していないか。 */
        size_t ulen = 0;
        const char *ust = vfs_read_ptr("update.state", &ulen);
        if (ust != NULL && ulen >= 8 &&
            strncmp(ust, "applying", 8) == 0) {
            serial_write_line(
                "WARNING: the previous update did not finish.");
            serial_write_line(
                "Files may be mixed. Restore a restore point "
                "(`snapshot list`, `snapshot restore <name>`) "
                "or apply the update again.");
            audit_log_event("update-interrupted", "marker found");
        }
    }
    /* リモート操作の確認 (ログインの前)。ロックされていれば
     * ここで PIN による解除を求める。ユーザー切替のときは
     * ここへ戻ってログインからやり直す。 */
session_start:
    remote_boot_check();
    /* アカウントがあればログインを通す。通ったら PIN から鍵を
     * 作り、暗号文のまま読み込んだファイルを復号する。
     * v1 のアカウントは、古い鍵で復号してから v2 に移行し、
     * 全ファイルを新しい鍵で書き直す。 */
    char pin[PIN_CAP];
    pin[0] = '\0';
    if (login_flow(pin, sizeof(pin), shell_settings.lockout_attempts,
                   (uint32_t)shell_settings.lockout_seconds) == 1) {
        if (login_kind != 2) {
            /* ゲストセッションからの切替では、同期ブロックを
             * 解除する (通常の起動では最初から解除済み)。 */
            persist_block_sync(0);
        }
        if (login_kind == 1) {
            /* 追加ユーザー: 包みを開いた主鍵で復号する。 */
            persist_set_key(login_mk);
            persist_decrypt_loaded();
            audit_flush_pending();
            for (int i = 0; i < 32; i++) {
                login_mk[i] = 0;
            }
        } else if (login_kind == 2) {
            /* ゲスト: 保存しないセッション。 */
            persist_block_sync(1);
        }
        size_t alen = 0;
        const char *atext = vfs_read_ptr(PRISM_ACCOUNT_FILE, &alen);
        uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
        uint32_t iter = 0;
        if (login_kind == 0 && atext != NULL &&
            account_get_salt(atext, alen, salt, &iter) == 0) {
            int ver = account_version(atext, alen);
            if (ver == 1) {
                /* 古い鍵 (SHA-256 1 回) でまず復号する。 */
                uint8_t old_key[32];
                crypto_derive_key(pin, salt, old_key);
                persist_set_key(old_key);
                persist_decrypt_loaded();
                audit_flush_pending();
                for (int i = 0; i < 32; i++) {
                    old_key[i] = 0;
                }
                /* v2 に移行する。 */
                uint8_t new_salt[PRISM_ACCOUNT_SALT_LEN];
                make_salt(new_salt);
                uint8_t new_key[32];
                crypto_kdf(pin, new_salt, PRISM_ACCOUNT_SALT_LEN,
                           PRISM_ACCOUNT_ITERATIONS, new_key);
                char account_text[PRISM_ACCOUNT_TEXT_MAX];
                if (account_make_text(
                        new_salt, PRISM_ACCOUNT_ITERATIONS, pin,
                        account_text,
                        sizeof(account_text)) == 0) {
                    size_t tl = 0;
                    while (account_text[tl] != '\0') {
                        tl++;
                    }
                    if (vfs_write(PRISM_ACCOUNT_FILE, account_text,
                                  tl) == 0) {
                        persist_set_key(new_key);
                        audit_log_event("account-migrate", "v1 -> v2");
                        persist_force_sync();
                        serial_write_line(
                            "Account upgraded to the stronger PIN format.");
                    }
                }
                for (int i = 0; i < 32; i++) {
                    new_key[i] = 0;
                }
            } else {
                uint8_t key[32];
                crypto_kdf(pin, salt, PRISM_ACCOUNT_SALT_LEN, iter,
                           key);
                persist_set_key(key);
                persist_decrypt_loaded();
                audit_flush_pending();
                for (int i = 0; i < 32; i++) {
                    key[i] = 0;
                }
            }
        }
        for (size_t i = 0; pin[i] != '\0'; i++) {
            pin[i] = 0;
        }
    }
    /* 回復モード: R キーか `recovery` コマンドの印で入る。
     * ログインの後なので、暗号化されたファイルは復号済み。 */
    if (recovery_requested()) {
        recovery_run();
    }
    /* 自動セキュリティ更新: on のときだけ、起動時に適用する。 */
    auto_security_update();
    fw_load();
    desktop_init();

    for (;;) {
        if (switch_user_flag) {
            /* ユーザー切替: セッション状態を初期化して
             * ログインへ戻る。ファイルは読み込み済み (復号
             * 済み) のまま使うので persist_load はしない。 */
            switch_user_flag = 0;
            strcpy(current_user, "admin");
            guest_session = 0;
            login_kind = 0;
            serial_write_line("[session] back to the login screen");
            goto session_start;
        }
        serial_write("prismos> ");
        desktop_console_write("prismos> ");
        desktop_refresh_console();
        if (script_pop(line, sizeof(line))) {
            /* スクリプトの行は見えるようにエコーする。 */
            serial_write(line);
            serial_write_char('\n');
            desktop_console_write(line);
            desktop_console_putc('\n');
            desktop_refresh_console();
        } else {
            read_line_merged(line, sizeof(line));
        }
        if (pending_language >= 0) {
            /* 設定アプリで切り替えた言語を実行中のシェルへ
             * 反映する (再起動しなくても help が切り替わる)。 */
            state.language = pending_language;
            shell_settings.language = pending_language;
            pending_language = -1;
        }
        if (line[0] == '\0') {
            continue;
        }
        hist_remember(line);

        /* パイプ: 左側の出力を右側へ渡す。 */
        {
            int has_pipe = 0;
            for (size_t i = 1; line[i] != '\0'; i++) {
                if (line[i] == '|' && line[i - 1] == ' ' &&
                    line[i + 1] == ' ') {
                    has_pipe = 1;
                    break;
                }
            }
            if (has_pipe) {
                pipe_flow(&state, line, out);
                persist_maybe_sync();
                continue;
            }
        }
        if (line_eq(line, "history")) {
            for (int i = 0; i < hist_count; i++) {
                print_u64_serial((uint64_t)(i + 1));
                serial_write("  ");
                serial_write_line(hist_list[i]);
            }
            desktop_refresh_console();
            continue;
        }
        if (starts_with_word(line, "script")) {
            script_flow(line);
            continue;
        }
        if (line_eq(line, "env")) {
            for (int i = 0; i < env_count; i++) {
                serial_write(env_names[i]);
                serial_write("=");
                serial_write_line(env_vals[i]);
            }
            desktop_refresh_console();
            continue;
        }
        if (starts_with_word(line, "set")) {
            const char *p = skip_spaces(line + 3);
            size_t nlen = 0;
            while (p[nlen] != '\0' && p[nlen] != '=' &&
                   p[nlen] != ' ') {
                nlen++;
            }
            if (p[nlen] == '=') {
                env_set(p, nlen, p + nlen + 1);
            } else {
                serial_write_line("Usage: set NAME=value");
            }
            continue;
        }
        if (starts_with_word(line, "unset")) {
            const char *p = skip_spaces(line + 5);
            size_t nlen = 0;
            while (p[nlen] != '\0' && p[nlen] != ' ') {
                nlen++;
            }
            if (nlen > 0) {
                env_unset(p, nlen);
            }
            continue;
        }

        if (is_mem_command(line)) {
            print_memory_info();
            persist_maybe_sync();
            continue;
        }

        if (line_eq(line, "install")) {
            serial_write_line("Install PrismOS to the disk.");
            serial_write_line("This erases the disk. Run `install yes` to proceed.");
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "install yes")) {
            /* セットアップの一部として言語を聞く (仕様の
             * 明示決定)。選んだ言語はインストールされる
             * settings.txt に入り、ヘルプと設定画面に
             * 反映する。 */
            serial_write_line(
                "Choose a language / 言語を選んでください:");
            serial_write_line("  [1] English");
            serial_write_line("  [2] 日本語");
            serial_write("language> ");
            {
                /* 最大で約 3 秒だけ待つ。入力が無ければ
                 * 英語で進む (自動テストや無人インストール
                 * を止めないため)。 */
                char lbuf[8];
                size_t ln = 0;
                prism_rtc_time t0;
                int have_t0 = (rtc_read(&t0) == 0);
                int waited = 0;
                int done = 0;
                while (!done) {
                    int c = keyboard_get_char();
                    if (c < 0) {
                        c = serial_try_read_char();
                    }
                    if (c < 0) {
                        desktop_tick();
                        __asm__ volatile("pause");
                        if (have_t0) {
                            prism_rtc_time tn;
                            if (rtc_read(&tn) == 0 &&
                                tn.second != t0.second) {
                                t0 = tn;
                                if (++waited >= 3) {
                                    done = 1;
                                }
                            }
                        }
                        continue;
                    }
                    if (c == '\r' || c == '\n') {
                        serial_write_char('\n');
                        done = 1;
                    } else if (ln + 1 < sizeof(lbuf)) {
                        lbuf[ln++] = (char)c;
                        serial_write_char((char)c);
                    }
                }
                lbuf[ln] = '\0';
                shell_settings.language = (lbuf[0] == '2') ? 1 : 0;
                state.language = shell_settings.language;
                save_settings(&shell_settings);
                serial_write_line(shell_settings.language
                                      ? "言語: 日本語"
                                      : "Language: English");
            }
            if (!persist_disk_present()) {
                serial_write_line("Install failed: no disk found.");
            } else if (persist_media_blocked()) {
                serial_write_line("Install failed: external media is blocked by the administrator.");
            } else if (persist_install() == 0) {
                serial_write_line("Installed to disk. Reboot with the same disk attached to use the installed files.");
            } else {
                serial_write_line("Install failed: could not write to disk.");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "install dual")) {
            serial_write_line("Install PrismOS to the data partition of a dual-boot disk.");
            serial_write_line("The boot menu and the other system are left untouched.");
            serial_write_line("Run `install dual yes` to proceed.");
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "install dual yes")) {
            if (!persist_disk_present()) {
                serial_write_line("Install failed: no disk found.");
            } else {
                int r = persist_install_dual();
                if (r == 0) {
                    serial_write_line("Installed to the dual-boot disk's data partition. The boot menu and the other system are untouched. Reboot with the same disk attached to use the installed files.");
                } else if (r == -2) {
                    serial_write_line("Install failed: no data partition found. This disk is not a dual-boot disk.");
                } else {
                    serial_write_line("Install failed: could not write to disk.");
                }
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "passwd")) {
            passwd_flow();
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "security")) {
            security_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "audit") || starts_with_word(line, "audit")) {
            audit_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "fde")) {
            fde_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "snapshot")) {
            snapshot_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "update")) {
            update_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "date") ||
            starts_with_word(line, "date") ||
            starts_with_word(line, "tz") ||
            starts_with_word(line, "ntp")) {
            clock_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "user") ||
            starts_with_word(line, "totp")) {
            users_flow(line);
            persist_maybe_sync();
            continue;
        }

        if (starts_with_word(line, "proxy")) {
            proxy_flow(line);
            continue;
        }
        if (starts_with_word(line, "serve2")) {
            serve2_flow(line);
            continue;
        }
        if (starts_with_word(line, "serve")) {
            serve_flow(line);
            continue;
        }
        if (starts_with_word(line, "store")) {
            store_flow(line);
            continue;
        }
        if (starts_with_word(line, "ping6")) {
            ping6_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "dns6")) {
            dns6_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "udp6")) {
            udp6_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "tcp6")) {
            tcp6_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "serve6")) {
            serve6_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "ping")) {
            ping_flow(line);
            continue;
        }
        if (line_eq(line, "ip6")) {
            ip6_flow();
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "fw")) {
            fw_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "mail")) {
            mail_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "pq")) {
            pq_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "bio")) {
            bio_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "driver")) {
            driver_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (strcmp(line, "lock") == 0) {
            lock_flow();
            continue;
        }
        if (starts_with_word(line, "run") &&
            (line[3] == '\0' || line[3] == ' ' || line[3] == '\t')) {
            run_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "compat")) {
            compat_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "appguard")) {
            serial_write_line(
                "appguard: built-in app fault containment is on "
                "(draw/key/mouse dispatch is guarded)");
            continue;
        }
        if (line_eq(line, "appguard test")) {
            desktop_appguard_selftest();
            continue;
        }
        if (line_eq(line, "uvc")) {
            if (!uvc_present()) {
                serial_write_line(
                    "uvc: no USB video device found");
            } else {
                serial_write("uvc: camera present, ");
                print_u64_serial((uint64_t)uvc_format_count());
                serial_write_line(" formats:");
                for (int i = 0; i < uvc_format_count(); i++) {
                    const uvc_format_frame *f = uvc_format_at(i);
                    serial_write("  [");
                    print_u64_serial((uint64_t)i);
                    serial_write("] ");
                    serial_write(f->is_mjpeg ? "MJPEG " : "YUY2  ");
                    print_u64_serial(f->width);
                    serial_write("x");
                    print_u64_serial(f->height);
                    serial_write_line("");
                }
            }
            continue;
        }
        if (line_eq(line, "recovery")) {
            if (persist_request_recovery() == 0) {
                serial_write_line("Rebooting into recovery mode...");
                prism_outb(0x64, 0xFE);
                prism_halt();
            } else {
                serial_write_line("Recovery mode needs an installed disk.");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "net")) {
            print_net_status();
            persist_maybe_sync();
            continue;
        }
        if (line[0] == 'r' && line[1] == 'e' && line[2] == 's' &&
            line[3] == ' ') {
            /* res <width> <height>: 解像度を変更する。 */
            const char *p = line + 4;
            uint32_t w = 0;
            uint32_t hh = 0;
            while (*p >= '0' && *p <= '9') {
                w = w * 10 + (uint32_t)(*p - '0');
                p++;
            }
            while (*p == ' ') {
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                hh = hh * 10 + (uint32_t)(*p - '0');
                p++;
            }
            if (w > 0 && hh > 0 &&
                vbe_set_mode(w, hh) == 0) {
                mouse_set_bounds((int)w, (int)hh);
                mouse_set_position((int)w / 2, (int)hh / 2);
                /* 解像度を永続化する (次回起動時に
                 * desktop_init が適用する)。 */
                shell_settings.res_w = (int)w;
                shell_settings.res_h = (int)hh;
                save_settings(&shell_settings);
                desktop_redraw();
                serial_write("res: ");
                print_u64_serial(w);
                serial_write("x");
                print_u64_serial(hh);
                serial_write_line("");
            } else {
                serial_write_line("res: failed (usage: res "
                                  "<width> <height>)");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "tablet")) {
            int rx, ry, bt, mx, my;
            vinput_state(&rx, &ry, &bt, &mx, &my);
            if (vinput_present()) {
                serial_write("tablet: raw ");
                print_u64_serial((uint64_t)rx);
                serial_write(",");
                print_u64_serial((uint64_t)ry);
                serial_write(" max ");
                print_u64_serial((uint64_t)mx);
                serial_write(",");
                print_u64_serial((uint64_t)my);
                serial_write(" buttons ");
                print_u64_serial((uint64_t)bt);
                serial_write_line("");
            } else {
                serial_write_line("tablet: (no virtio tablet)");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "hda")) {
            if (hda_present()) {
                serial_write("hda: ");
                serial_write(hda_codec_name());
                serial_write(" stream pos ");
                print_u64_serial(hda_stream_pos());
                serial_write_line("");
                hda_debug_dump();
            } else {
                serial_write_line("hda: (no HD Audio device)");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "hda beep")) {
            if (hda_beep() == 0) {
                serial_write_line("hda beep: playing");
            } else {
                serial_write_line("hda beep: no device");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "hda record") ||
            strncmp(line, "hda record ", 11) == 0) {
            /* 録音: 入力ストリームで採取し rec.wav に保存。
             * 秒数は 1〜3 (既定 2)。 */
            int secs = 2;
            if (line[10] == ' ' && line[11] >= '1' &&
                line[11] <= '3') {
                secs = line[11] - '0';
            }
            if (!hda_capture_supported()) {
                serial_write_line(
                    "hda record: no capture device (no ADC)");
            } else {
                uint32_t bytes = 0, peak = 0;
                if (hda_record((uint32_t)secs * 1000u, &bytes,
                               &peak) != 0) {
                    serial_write_line("hda record: failed");
                } else {
                    static uint8_t wavbuf[44 + 589824];
                    const int16_t *pcm = hda_capture_data();
                    if (pcm == NULL ||
                        wav_build_header16(wavbuf, sizeof(wavbuf),
                                           bytes, 48000, 2) != 0) {
                        serial_write_line("hda record: failed");
                    } else {
                        const uint8_t *s = (const uint8_t *)pcm;
                        for (uint32_t i = 0; i < bytes; i++) {
                            wavbuf[44 + i] = s[i];
                        }
                        if (vfs_write("rec.wav", (const char *)wavbuf,
                                      44 + bytes) == 0) {
                            serial_write("hda record: saved "
                                         "rec.wav (");
                            print_u64_serial(44 + bytes);
                            serial_write(" bytes, peak ");
                            print_u64_serial(peak);
                            serial_write_line(")");
                        } else {
                            serial_write_line(
                                "hda record: save failed");
                        }
                    }
                }
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "hda playrec")) {
            size_t len = 0;
            const char *data = vfs_read_ptr("rec.wav", &len);
            if (data == NULL || len < 44) {
                serial_write_line("hda playrec: no rec.wav");
            } else if (hda_capture_load(
                           (const int16_t *)(data + 44),
                           (uint32_t)(len - 44)) != 0 ||
                       hda_play_capture((uint32_t)(len - 44)) !=
                           0) {
                serial_write_line("hda playrec: failed");
            } else {
                serial_write_line("hda playrec: playing");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "smp")) {
            serial_write("smp: ");
            print_u64_serial((uint64_t)smp_cpu_count());
            serial_write_line(" CPU(s) online");
            smp_ping_all();
            for (int i = 0; i < 200000; i++) {
                __asm__ volatile("pause");
            }
            for (int i = 0; i < smp_cpu_count(); i++) {
                serial_write(" cpu APIC ");
                print_u64_serial(smp_apic_id(i));
                serial_write(smp_is_bsp(i) ? " (BSP)" : " (AP)");
                serial_write(" pings ");
                print_u64_serial(smp_ping_count(i));
                serial_write_line("");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "smp work")) {
            /* 実作業分担の確認: バッファを CPU 数で割り、
             * 各 AP に自分の分の FNV-1a 64 ハッシュを
             * 計算させ、BSP が先に計算した参照値と
             * 一致するか確かめる。 */
            static uint8_t sbuf[256 * 1024];
            for (size_t i = 0; i < sizeof(sbuf); i++) {
                sbuf[i] = (uint8_t)(i * 31u + 7u);
            }
            int ncpu = smp_cpu_count();
            if (ncpu < 2) {
                serial_write_line(
                    "smp work: single CPU, nothing to share");
                persist_maybe_sync();
                continue;
            }
            static struct smp_hash_job jobs[16];
            size_t chunk = sizeof(sbuf) / (size_t)ncpu;
            int ok = 1;
            for (int i = 0; i < ncpu; i++) {
                size_t off = (size_t)i * chunk;
                size_t n = (i == ncpu - 1) ? sizeof(sbuf) - off
                                           : chunk;
                jobs[i].p = sbuf + off;
                jobs[i].n = n;
                uint64_t h = 1469598103934665603ULL;
                for (size_t k = 0; k < n; k++) {
                    h ^= sbuf[off + k];
                    h *= 1099511628211ULL;
                }
                jobs[i].hash = h; /* 参照値 */
            }
            /* 仕事用の関数 (AP が割り込み文脈で実行)。 */
            /* jobs[i].hash を結果で上書きさせるため、参照は
             * 別に保持する。 */
            static uint64_t ref[16];
            for (int i = 0; i < ncpu; i++) {
                ref[i] = jobs[i].hash;
            }
            for (int i = 0; i < ncpu; i++) {
                if (smp_is_bsp(i)) {
                    continue;
                }
                jobs[i].hash = 0;
                if (smp_post_work(i, smp_hash_work, &jobs[i]) !=
                    0) {
                    serial_write_line("smp work: post failed");
                    ok = 0;
                }
            }
            /* BSP は自分の分をその場で計算する。 */
            for (int i = 0; i < ncpu; i++) {
                if (smp_is_bsp(i)) {
                    smp_hash_work(&jobs[i]);
                }
            }
            /* AP の完了を待つ (上限つき)。 */
            for (int i = 0; i < ncpu && ok; i++) {
                if (smp_is_bsp(i)) {
                    continue;
                }
                long spins = 0;
                while (!smp_work_finished(i)) {
                    __asm__ volatile("pause");
                    if (++spins > 200000000L) {
                        serial_write_line("smp work: timeout");
                        ok = 0;
                        break;
                    }
                }
            }
            for (int i = 0; i < ncpu; i++) {
                if (!smp_is_bsp(i) && jobs[i].hash != ref[i]) {
                    serial_write_line("smp work: hash mismatch");
                    ok = 0;
                }
            }
            if (ok) {
                serial_write("smp work: all chunk hashes match (");
                print_u64_serial((uint64_t)ncpu);
                serial_write_line(" cpus shared the work)");
            } else {
                serial_write_line("smp work: FAILED");
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "power")) {
            if (acpi_pm1_cnt_port() == 0) {
                serial_write_line(
                    "power: no ACPI FADT (power management off)");
            } else {
                serial_write("power: ACPI present, PM1a_CNT port 0x");
                print_hex16_serial(acpi_pm1_cnt_port());
                serial_write_line("");
                if (acpi_s3_available()) {
                    serial_write("power: S3 sleep available (type ");
                    print_u64_serial((uint64_t)acpi_s3_slp_typ());
                    serial_write_line(") - 'sleep' enters it");
                } else {
                    serial_write_line(
                        "power: S3 not offered by firmware");
                }
                if (acpi_s5_slp_typ() >= 0) {
                    serial_write("power: S5 power-off type ");
                    print_u64_serial((uint64_t)acpi_s5_slp_typ());
                    serial_write_line(" ('poweroff')");
                }
                if (acpi_battery_present()) {
                    serial_write_line(
                        "power: battery device present (ACPI)");
                } else {
                    serial_write_line(
                        "power: battery: none in ACPI namespace");
                }
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "sleep")) {
            if (!acpi_s3_available()) {
                serial_write_line(
                    "power: S3 sleep not available on this machine");
            } else {
                serial_write_line(
                    "power: entering S3 sleep (wake: keyboard or "
                    "monitor system_wakeup)");
                if (acpi_enter_s3() == 0) {
                    serial_write_line("power: resumed from S3");
                }
            }
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "poweroff")) {
            serial_write_line("power: powering off (S5)");
            (void)acpi_poweroff();
            serial_write_line("power: power-off did not happen");
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "disk")) {
            print_disk_status();
            persist_maybe_sync();
            continue;
        }
        if (starts_with_word(line, "disk test")) {
            disk_test_flow(line);
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "usb")) {
            print_usb_status();
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "usb test")) {
            usb_storage_test();
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "ehci")) {
            print_ehci_status();
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "ehci test")) {
            ehci_storage_test();
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "xhci")) {
            print_xhci_status();
            persist_maybe_sync();
            continue;
        }
        if (line_eq(line, "xhci test")) {
            xhci_storage_test();
            persist_maybe_sync();
            continue;
        }
        if (strncmp(line, "dnstest", 7) == 0 &&
            (line[7] == ' ' || line[7] == '\t')) {
            const char *a = line + 7;
            while (*a == ' ' || *a == '\t') {
                a++;
            }
            if (net_dns_test(a) == 0) {
                serial_write_line("dnstest: ok");
            } else {
                serial_write_line("dnstest: failed");
            }
            persist_maybe_sync();
            continue;
        }
        if (strncmp(line, "fetch", 5) == 0 &&
            (line[5] == '\0' || line[5] == ' ' || line[5] == '\t')) {
            fetch_flow(line);
            persist_maybe_sync();
            continue;
        }

        /* beep <freq_hz> <ms>: PC スピーカーで矩形波を鳴らす。 */
        if (strncmp(line, "beep", 4) == 0 &&
            (line[4] == ' ' || line[4] == '\t')) {
            const char *a = line + 4;
            while (*a == ' ' || *a == '\t') {
                a++;
            }
            uint32_t freq = 0;
            while (*a >= '0' && *a <= '9') {
                freq = freq * 10 + (uint32_t)(*a - '0');
                a++;
            }
            while (*a == ' ' || *a == '\t') {
                a++;
            }
            uint32_t ms = 0;
            while (*a >= '0' && *a <= '9') {
                ms = ms * 10 + (uint32_t)(*a - '0');
                a++;
            }
            if (freq < 20 || freq > 20000 || ms == 0 || ms > 10000) {
                serial_write_line(
                    "usage: beep <freq 20-20000> <ms 1-10000>");
            } else {
                speaker_tone(freq, ms);
                serial_write_line("beep: done");
            }
            persist_maybe_sync();
            continue;
        }

        /* play <file>: 8 ビット・モノラル WAV を SB16 で鳴らす。
         * 形式が違う場合や音源が無い場合は鳴らさない。 */
        if (strncmp(line, "play", 4) == 0 &&
            (line[4] == ' ' || line[4] == '\t')) {
            const char *a = line + 4;
            while (*a == ' ' || *a == '\t') {
                a++;
            }
            char name[32];
            size_t p = 0;
            while (*a != '\0' && *a != ' ' && *a != '\t' &&
                   p + 1 < sizeof(name)) {
                name[p++] = *a++;
            }
            name[p] = '\0';
            size_t len = 0;
            const char *data = vfs_read_ptr(name, &len);
            if (data == NULL) {
                serial_write_line("play: no such file");
            } else {
                wav_info info;
                if (wav_parse((const uint8_t *)data, len, &info) != 0) {
                    serial_write_line(
                        "play: not an 8-bit mono WAV file");
                } else if (!sb16_present()) {
                    serial_write_line(
                        "play: no audio device (SB16 not found)");
                } else {
                    serial_write("playing ");
                    serial_write_line(name);
                    if (sb16_play_8bit(info.samples, info.sample_count,
                                       info.sample_rate) != 0) {
                        serial_write_line("play: failed");
                    } else {
                        serial_write_line("play: done");
                    }
                }
            }
            persist_maybe_sync();
            continue;
        }

        {
            static char expanded[1024];
            env_expand(line, expanded, sizeof(expanded));
            prism_shell_execute(&state, expanded, out, OUT_CAP);
        }

        if (state.should_clear) {
            state.should_clear = 0;
            serial_write("\x1b[2J\x1b[H");
            desktop_console_clear();
        }
        if (out[0] != '\0') {
            serial_write(out);
            desktop_console_write(out);
            if (out[strlen(out) - 1] != '\n') {
                serial_write_char('\n');
                desktop_console_putc('\n');
            }
            desktop_refresh_console();
        }
        if (state.should_reboot) {
            serial_write_line("Rebooting PrismOS...");
            /* キーボードコントローラ経由でリセットを要求する。
             * 効かなければ hlt ループに落ち、QEMU の -no-reboot が
             * あれば終了する。 */
            prism_outb(0x64, 0xFE);
            prism_halt();
        }
        persist_maybe_sync();
    }
}
