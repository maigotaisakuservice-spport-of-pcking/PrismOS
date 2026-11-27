/* PrismOS — 回復モード (シリアルのメニュー)。
 *
 * 回復モードは、起動時に R キーを押すか、シェルで `recovery`
 * と打って再起動することで入る。ログインの後に開くため、
 * 暗号化されたファイルがある場合は PIN で復号してから使う。
 * 物理的に本体を触れる人が使うことを前提にした仕組みで、
 * PIN を知らない人が暗号化されたファイルを読むことはできない
 * (鍵は PIN から作るため)。ただし回復モード自体は PIN を
 * 知らなくても開ける場合がある (アカウントが無い場合など)。
 * これは他の OS の回復モードと同じ考え方で、ここでは
 * その限界を正直に書いておく。
 */
#include "recovery.h"

#include "bootui.h"
#include "crypto.h"
#include "fb.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "netstack.h"
#include "persist.h"
#include "rtc.h"
#include "serial.h"
#include "settings.h"
#include "string.h"
#include "update.h"
#include "../shell/vfs.h"

#define LINE_CAP 64

static int key_requested;
/* 回復画面の言語 (0 = 英語、1 = 日本語)。recovery_run が
 * 設定から決める。 */
static int rec_lang;
/* 行入力のグラフィカルエコー用ラベル (NULL で無効)。 */
static const char *rec_echo_label;

/* キーボードとシリアルのどちらからでも 1 文字読む。
 * 無ければ -1。 */
static int recovery_get_char(void) {
    int c = keyboard_get_char();
    if (c < 0) {
        c = serial_try_read_char();
    }
    return c;
}

/* 行入力の内容を回復画面にも映す (rec_echo_label が
 * 設定されている間)。 */
static void rec_echo_update(const char *buf) {
    if (rec_echo_label == NULL || !bootui_available()) {
        return;
    }
    char line[96];
    size_t p = 0;
    const char *s = rec_echo_label;
    while (*s != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *s++;
    }
    if (p + 1 < sizeof(line)) {
        line[p++] = ' ';
    }
    s = buf;
    while (*s != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *s++;
    }
    line[p] = '\0';
    const char *lines[1] = { line };
    bootui_text_lines(lines, 1);
}

/* 回復モードのメニュー用に 1 行読む。エコーする。
 * 入力の意味は従来どおりシリアル/キーボードで、画面へは
 * rec_echo_update で映す。 */
static size_t recovery_read_line(char *buf, size_t cap) {
    size_t len = 0;
    buf[0] = '\0';
    rec_echo_update(buf);
    for (;;) {
        int c = recovery_get_char();
        if (c < 0) {
            __asm__ volatile("pause");
            continue;
        }
        if (c >= 0x80) {
            continue; /* 特別なキー (F1 など) は無視する */
        }
        if (c == '\r' || c == '\n') {
            serial_write_char('\n');
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                serial_write_char('\b');
                serial_write_char(' ');
                serial_write_char('\b');
                buf[len] = '\0';
                rec_echo_update(buf);
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) {
            continue;
        }
        if (len + 1 < cap) {
            buf[len++] = (char)c;
            buf[len] = '\0';
            serial_write_char((char)c);
            rec_echo_update(buf);
        }
    }
    buf[len] = '\0';
    return len;
}

static void print_u64(uint64_t value) {
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

int recovery_key_check(void) {
    serial_write_line("[boot] Press R for recovery...");
    /* ロゴ画面の下部にも案内を出す (D1)。ロゴ画面は白地
     * なので濃い色で描く。 */
    if (fb_ready()) {
        uint64_t w = fb_width();
        uint64_t h = fb_height();
        const char *ja = "回復モードに入るには R キーを押してください";
        const char *en = "Press R for recovery mode";
        uint64_t tw = fb_text_ui_width(ja, 1);
        fb_draw_text_ui((w - tw) / 2, h - 64, ja,
                        fb_rgb(0x33, 0x33, 0x33),
                        fb_rgb(0xFF, 0xFF, 0xFF), 1);
        tw = fb_text_ui_width(en, 1);
        fb_draw_text_ui((w - tw) / 2, h - 40, en,
                        fb_rgb(0x77, 0x77, 0x77),
                        fb_rgb(0xFF, 0xFF, 0xFF), 1);
        fb_flush();
    }
    prism_rtc_time start;
    int have_time = (rtc_read(&start) == 0);
    uint8_t start_sec = have_time ? start.second : 0;
    /* 時計が読めれば 2 秒、読めなければ決まった回数だけ待つ。 */
    for (uint32_t spin = 0;; spin++) {
        int c = recovery_get_char();
        if (c == 'r' || c == 'R') {
            serial_write_line("Recovery requested.");
            key_requested = 1;
            return 1;
        }
        if (have_time) {
            prism_rtc_time now;
            if (rtc_read(&now) == 0) {
                int diff = (int)now.second - (int)start_sec;
                if (diff < 0) {
                    diff += 60;
                }
                if (diff >= 2) {
                    return 0;
                }
            }
        } else if (spin > 20000000u) {
            return 0;
        }
        __asm__ volatile("pause");
    }
}

int recovery_requested(void) {
    return key_requested || persist_recovery_requested();
}

/* 結果を回復画面の下部にも出す (シリアルと併記)。 */
static void rec_result(const char *en, const char *ja) {
    if (!bootui_available()) {
        return;
    }
    const char *lines[1] = { rec_lang ? ja : en };
    bootui_text_lines(lines, 1);
}

/* "en + 数 + en2" / "ja1 + 数 + ja2" の結果行を出す。 */
static void rec_result_num(const char *en1, const char *en2,
                           const char *ja1, const char *ja2,
                           uint64_t v) {
    char num[21];
    int i = 0;
    if (v == 0) {
        num[i++] = '0';
    } else {
        char rev[21];
        int r = 0;
        while (v > 0) {
            rev[r++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (r > 0) {
            num[i++] = rev[--r];
        }
    }
    num[i] = '\0';
    char en[96], ja[96];
    size_t p = 0;
    const char *s = en1;
    while (*s != '\0' && p + 1 < sizeof(en)) {
        en[p++] = *s++;
    }
    s = num;
    while (*s != '\0' && p + 1 < sizeof(en)) {
        en[p++] = *s++;
    }
    s = en2;
    while (*s != '\0' && p + 1 < sizeof(en)) {
        en[p++] = *s++;
    }
    en[p] = '\0';
    p = 0;
    s = ja1;
    while (*s != '\0' && p + 1 < sizeof(ja)) {
        ja[p++] = *s++;
    }
    s = num;
    while (*s != '\0' && p + 1 < sizeof(ja)) {
        ja[p++] = *s++;
    }
    s = ja2;
    while (*s != '\0' && p + 1 < sizeof(ja)) {
        ja[p++] = *s++;
    }
    ja[p] = '\0';
    rec_result(en, ja);
}

static void print_menu(void) {
    serial_write_line("");
    serial_write_line("PrismOS Recovery");
    serial_write_line("  1) Repair (keep data)");
    serial_write_line("  2) Restore from a restore point");
    serial_write_line("  3) Initialize (erase everything)");
    serial_write_line("  4) Download previous version and repair damaged files");
    serial_write_line("  5) Continue normal boot");
    serial_write_line("  6) Reboot");
    serial_write("Choice: ");
}

/* 1) データを残して修復。組み込みファイルだけを直す。 */
static void do_repair(void) {
    int n = vfs_repair_builtins();
    if (n < 0) {
        serial_write_line("Repair failed: no memory for files.");
        rec_result("Repair failed.", "修復に失敗しました");
        return;
    }
    persist_maybe_sync();
    serial_write("Repaired ");
    print_u64((uint64_t)n);
    serial_write_line(" file(s).");
    if (n == 0) {
        serial_write_line("Built-in files are already correct.");
        rec_result("Built-in files are already correct.",
                   "組み込みファイルは正常です");
    } else {
        rec_result_num("Repaired ", " file(s).",
                       "", " ファイルを修復しました",
                       (uint64_t)n);
    }
}

/* 2) 復元ポイントから復元。 */
static void do_restore(void) {
    diskfs_snapshot_info infos[DISKFS_SNAP_SLOTS];
    int count = 0;
    if (persist_snapshot_list(infos, DISKFS_SNAP_SLOTS, &count) != 0 ||
        count == 0) {
        serial_write_line("No restore points.");
        rec_result("No restore points.", "復元ポイントがありません");
        return;
    }
    for (int i = 0; i < count; i++) {
        serial_write("  ");
        print_u64((uint64_t)(i + 1));
        serial_write(") ");
        serial_write(infos[i].name);
        serial_write("  ");
        serial_write(infos[i].timestamp);
        serial_write("  (");
        print_u64((uint64_t)infos[i].file_count);
        serial_write_line(" files)");
    }
    if (bootui_available()) {
        /* 復元ポイント一覧を画面にも出す。 */
        static char lines_buf[8][96];
        const char *lines[8];
        int nl = 0;
        for (int i = 0; i < count && nl < 8; i++) {
            size_t p = 0;
            lines_buf[nl][p++] = (char)('1' + i);
            lines_buf[nl][p++] = ')';
            lines_buf[nl][p++] = ' ';
            const char *s = infos[i].name;
            while (*s != '\0' && p + 1 < sizeof(lines_buf[nl])) {
                lines_buf[nl][p++] = *s++;
            }
            if (p + 1 < sizeof(lines_buf[nl])) {
                lines_buf[nl][p++] = ' ';
            }
            s = infos[i].timestamp;
            while (*s != '\0' && p + 1 < sizeof(lines_buf[nl])) {
                lines_buf[nl][p++] = *s++;
            }
            lines_buf[nl][p] = '\0';
            lines[nl] = lines_buf[nl];
            nl++;
        }
        bootui_text_lines(lines, nl);
    }
    rec_echo_label = rec_lang ? "復元する番号 (0で取消):"
                              : "Restore which (0 = cancel):";
    serial_write("Restore which one (number, 0 to cancel)? ");
    char line[LINE_CAP];
    recovery_read_line(line, sizeof(line));
    rec_echo_label = NULL;
    if (line[0] < '1' || line[0] > '9' || line[1] != '\0') {
        serial_write_line("Cancelled.");
        rec_result("Cancelled.", "取り消しました");
        return;
    }
    int idx = line[0] - '1';
    if (idx >= count) {
        serial_write_line("No such restore point.");
        rec_result("No such restore point.",
                   "その復元ポイントはありません");
        return;
    }
    int rc = persist_snapshot_restore(infos[idx].name);
    if (rc != 0) {
        serial_write_line("Restore failed.");
        rec_result("Restore failed.", "復元に失敗しました");
        return;
    }
    /* VFS をディスクの内容で読み直す。 */
    persist_load();
    serial_write("Restored snapshot '");
    serial_write(infos[idx].name);
    serial_write_line("'.");
    rec_result("Restored.", "復元しました");
}

/* 3) 初期化。確認に YES と打たせる。 */
static void do_initialize(void) {
    serial_write_line("This erases all files on the disk, including");
    serial_write_line("your files and the login PIN. Built-in files");
    serial_write_line("will be restored.");
    if (bootui_available()) {
        const char *warn[3];
        if (rec_lang) {
            warn[0] = "ディスク上のすべてのファイルを消去します";
            warn[1] = "あなたのファイルとログイン PIN も消えます";
            warn[2] = "組み込みファイルだけが残ります";
        } else {
            warn[0] = "This erases all files on the disk,";
            warn[1] = "including your files and the login PIN.";
            warn[2] = "Only built-in files will remain.";
        }
        bootui_text_lines(warn, 3);
    }
    rec_echo_label = rec_lang ? "消去するには YES と入力:"
                              : "Type YES to erase:";
    serial_write("Type YES to erase everything: ");
    char line[LINE_CAP];
    recovery_read_line(line, sizeof(line));
    rec_echo_label = NULL;
    if (strcmp(line, "YES") != 0) {
        serial_write_line("Cancelled.");
        rec_result("Cancelled.", "取り消しました");
        return;
    }
    /* ユーザーのファイルを全部消して組み込みだけに戻す。 */
    vfs_init();
    persist_clear_key();
    if (!persist_disk_present()) {
        serial_write_line("Initialized in memory (no disk found).");
        rec_result("Initialized (memory only).",
                   "初期化しました (メモリ上のみ)");
        return;
    }
    if (persist_initialize_disk() != 0) {
        serial_write_line("Initialize failed: could not write to disk.");
        rec_result("Initialize failed.", "初期化に失敗しました");
        return;
    }
    serial_write_line("Initialized. Only built-in files remain.");
    rec_result("Initialized.", "初期化しました");
}

/* 設定 (settings.txt) を読む。無ければ既定値。 */
static void load_settings(prism_settings *s) {
    settings_defaults(s);
    size_t len = 0;
    const char *text = vfs_read_ptr("settings.txt", &len);
    if (text != NULL) {
        settings_parse(s, text, len);
    }
}

/* 4) 前のバージョンをダウンロードして壊れたファイルだけ修復。
 * マニフェストは更新サーバの <update_path>.prev に置く。 */

/* 前バージョンマニフェストの署名 (<path>.sig) を取ってきて検証する。
 * 署名がなければ・長さが違えば・正しくなければ -1 を返す。 */
static int verify_prev_signature(const char *host, const char *path,
                                 const char *manifest, size_t manifest_len) {
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
    if (net_fetch_http(host, sig_path, (char *)sigbuf, sizeof(sigbuf),
                       &got, &code) != 0) {
        return -1;
    }
    return update_verify_manifest_sig((const uint8_t *)manifest, manifest_len,
                                      sigbuf, got);
}

static void do_prev_repair(void) {
    prism_settings s;
    load_settings(&s);
    if (s.update_host[0] == '\0') {
        serial_write_line("No update server configured.");
        serial_write_line("Set it with `update server <host> [path]`.");
        rec_result("No update server configured.",
                   "更新サーバーが設定されていません");
        return;
    }
    rec_result("Downloading the previous version...",
               "前のバージョンをダウンロードしています...");
    char prev_path[160];
    size_t pi = 0;
    const char *suffix = ".prev";
    while (s.update_path[pi] != '\0' && pi + 1 < sizeof(prev_path)) {
        prev_path[pi] = s.update_path[pi];
        pi++;
    }
    size_t si = 0;
    while (suffix[si] != '\0' && pi + 1 < sizeof(prev_path)) {
        prev_path[pi++] = suffix[si++];
    }
    prev_path[pi] = '\0';
    static char mbuf[16385];
    size_t got = 0;
    int code = 0;
    serial_write("Downloading previous version list from ");
    serial_write(s.update_host);
    serial_write(prev_path);
    serial_write_line(" ...");
    if (net_fetch_http(s.update_host, prev_path, mbuf, sizeof(mbuf), &got,
                       &code) != 0) {
        serial_write_line("Download failed: could not reach the server.");
        return;
    }
    if (verify_prev_signature(s.update_host, prev_path, mbuf, got) != 0) {
        serial_write_line("Download failed: bad signature.");
        return;
    }
    static update_manifest m;
    if (update_parse_manifest(mbuf, got, &m) != 0) {
        serial_write_line("Download failed: bad manifest.");
        return;
    }
    serial_write("Previous version: ");
    serial_write_line(m.version);
    int checked = 0;
    int repaired = 0;
    static uint8_t file_buf[DISKFS_SMALL_FILE_SIZE];
    for (int i = 0; i < m.file_count; i++) {
        const char *name = m.files[i].name;
        /* 組み込みファイルだけが修復の対象。 */
        if (!vfs_is_builtin(name)) {
            continue;
        }
        size_t len = 0;
        const char *local = vfs_read_ptr(name, &len);
        char local_hex[65];
        int damaged = 0;
        if (local == NULL) {
            damaged = 1;
        } else {
            update_sha256_hex((const uint8_t *)local, len, local_hex);
            char want_hex[65];
            update_bytes_to_hex(m.files[i].sha256, want_hex);
            if (strcmp(local_hex, want_hex) != 0) {
                damaged = 1;
            }
        }
        checked++;
        if (!damaged) {
            continue;
        }
        /* 壊れているものだけ取ってくる。 */
        char url[128];
        size_t ui = 0;
        const char *base = m.base;
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
        serial_write("Repairing ");
        serial_write(name);
        serial_write_line(" ...");
        if (net_fetch_http(s.update_host, url, (char *)file_buf,
                           sizeof(file_buf), &fgot, &fcode) != 0) {
            serial_write("Download failed for ");
            serial_write_line(name);
            continue;
        }
        char got_hex[65];
        update_sha256_hex(file_buf, fgot, got_hex);
        char want_hex[65];
        update_bytes_to_hex(m.files[i].sha256, want_hex);
        if (strcmp(got_hex, want_hex) != 0) {
            serial_write("Hash mismatch for ");
            serial_write_line(name);
            continue;
        }
        if (vfs_write(name, (const char *)file_buf, fgot) == 0) {
            repaired++;
        }
    }
    persist_maybe_sync();
    serial_write("Repaired ");
    print_u64((uint64_t)repaired);
    serial_write(" damaged file(s) (");
    print_u64((uint64_t)checked);
    serial_write_line(" checked).");
    rec_result_num("Repaired ", " damaged file(s).",
                   "壊れた ", " ファイルを修復しました",
                   (uint64_t)repaired);
}

static void do_reboot(void) {
    serial_write_line("Rebooting PrismOS...");
    prism_outb(0x64, 0xFE);
    prism_halt();
}

/* 回復メニューの項目 (画面表示用)。 */
static const char *const rec_items_en[6] = {
    "1) Repair (keep data)",
    "2) Restore from a restore point",
    "3) Initialize (erase everything)",
    "4) Repair with the previous version",
    "5) Continue normal boot",
    "6) Reboot"
};
static const char *const rec_items_ja[6] = {
    "1) 修復する (データを残す)",
    "2) 復元ポイントから復元する",
    "3) 初期化する (すべて消去)",
    "4) 前のバージョンで修復する",
    "5) 通常起動を続ける",
    "6) 再起動する"
};

/* マウスカーソルの位置 (回復メニュー内)。 */
static int rec_mx = 640;
static int rec_my = 400;
static int rec_prev_buttons;

/* 回復メニューを画面に描く (選択行 + 操作ヒント + カーソル)。 */
static void rec_show_menu(int selected) {
    if (!bootui_available()) {
        return;
    }
    bootui_menu("PrismOS Recovery", "PrismOS 回復",
                rec_items_en, rec_items_ja, 6, selected);
    const char *hint[1];
    hint[0] = rec_lang
                  ? "↑↓ で選択、Enter で実行 (数字キーでも選べます)"
                  : "Up/Down + Enter, digits, or click";
    bootui_text_lines(hint, 1);
    bootui_draw_cursor(rec_mx, rec_my);
}

/* メニュー用の 1 行入力。行入力の意味は従来どおりだが、
 * 矢印キーで選択を動かし、マウスクリックで項目を即実行
 * できる (クリックは数字の行として返す)。 */
static size_t recovery_menu_read_line(char *buf, size_t cap,
                                      int *selected) {
    size_t len = 0;
    buf[0] = '\0';
    for (;;) {
        /* マウス: 移動でカーソル、クリックで項目実行。 */
        if (bootui_available() && mouse_present()) {
            int dx = 0, dy = 0, buttons = 0;
            if (mouse_poll(&dx, &dy, &buttons)) {
                int moved = (dx != 0 || dy != 0);
                mouse_clamp_move(&rec_mx, &rec_my, dx, dy,
                                 (int)fb_width(), (int)fb_height());
                if ((buttons & 1) && !(rec_prev_buttons & 1)) {
                    int hit = bootui_menu_hit(rec_mx, rec_my);
                    if (hit >= 0) {
                        *selected = hit;
                        buf[0] = (char)('1' + hit);
                        buf[1] = '\0';
                        rec_prev_buttons = buttons;
                        return 1;
                    }
                }
                rec_prev_buttons = buttons;
                if (moved || (buttons & 1)) {
                    rec_show_menu(*selected);
                }
            }
        }
        int c = recovery_get_char();
        if (c < 0) {
            __asm__ volatile("pause");
            continue;
        }
        if (c == PRISM_KEY_UP || c == PRISM_KEY_DOWN) {
            if (c == PRISM_KEY_UP) {
                *selected = (*selected + 5) % 6;
            } else {
                *selected = (*selected + 1) % 6;
            }
            rec_show_menu(*selected);
            continue;
        }
        if (c >= 0x80) {
            continue; /* 特別なキー (F1 など) は無視する */
        }
        if (c == '\r' || c == '\n') {
            serial_write_char('\n');
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                serial_write_char('\b');
                serial_write_char(' ');
                serial_write_char('\b');
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) {
            continue;
        }
        if (len + 1 < cap) {
            buf[len++] = (char)c;
            serial_write_char((char)c);
        }
    }
    buf[len] = '\0';
    return len;
}

void recovery_run(void) {
    /* 言語を設定から決める (ログイン後なので読める)。 */
    {
        prism_settings s;
        load_settings(&s);
        rec_lang = s.language ? 1 : 0;
    }
    if (bootui_available()) {
        bootui_set_language(rec_lang);
        rec_mx = (int)fb_width() / 2;
        rec_my = (int)fb_height() / 2;
    }
    int selected = 0;
    for (;;) {
        print_menu();
        rec_show_menu(selected);
        char line[LINE_CAP];
        recovery_menu_read_line(line, sizeof(line), &selected);
        int choice = -1;
        if (line[0] >= '1' && line[0] <= '6' && line[1] == '\0') {
            choice = line[0] - '1';
        } else if (line[0] == '\0') {
            choice = selected; /* Enter = 反転中の項目 */
        }
        if (choice < 0) {
            serial_write_line("Unknown choice.");
            continue;
        }
        selected = choice;
        rec_show_menu(selected);
        if (choice == 0) {
            do_repair();
        } else if (choice == 1) {
            do_restore();
        } else if (choice == 2) {
            do_initialize();
        } else if (choice == 3) {
            do_prev_repair();
        } else if (choice == 4) {
            serial_write_line("Continuing normal boot.");
            return;
        } else if (choice == 5) {
            do_reboot();
        }
    }
}
