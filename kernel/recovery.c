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

#include "crypto.h"
#include "io.h"
#include "keyboard.h"
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

/* キーボードとシリアルのどちらからでも 1 文字読む。
 * 無ければ -1。 */
static int recovery_get_char(void) {
    int c = keyboard_get_char();
    if (c < 0) {
        c = serial_try_read_char();
    }
    return c;
}

/* 回復モードのメニュー用に 1 行読む。エコーする。
 * デスクトップはまだ初期化されていないため、画面には出さない。 */
static size_t recovery_read_line(char *buf, size_t cap) {
    size_t len = 0;
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
        return;
    }
    persist_maybe_sync();
    serial_write("Repaired ");
    print_u64((uint64_t)n);
    serial_write_line(" file(s).");
    if (n == 0) {
        serial_write_line("Built-in files are already correct.");
    }
}

/* 2) 復元ポイントから復元。 */
static void do_restore(void) {
    diskfs_snapshot_info infos[DISKFS_SNAP_SLOTS];
    int count = 0;
    if (persist_snapshot_list(infos, DISKFS_SNAP_SLOTS, &count) != 0 ||
        count == 0) {
        serial_write_line("No restore points.");
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
    serial_write("Restore which one (number, 0 to cancel)? ");
    char line[LINE_CAP];
    recovery_read_line(line, sizeof(line));
    if (line[0] < '1' || line[0] > '9' || line[1] != '\0') {
        serial_write_line("Cancelled.");
        return;
    }
    int idx = line[0] - '1';
    if (idx >= count) {
        serial_write_line("No such restore point.");
        return;
    }
    int rc = persist_snapshot_restore(infos[idx].name);
    if (rc != 0) {
        serial_write_line("Restore failed.");
        return;
    }
    /* VFS をディスクの内容で読み直す。 */
    persist_load();
    serial_write("Restored snapshot '");
    serial_write(infos[idx].name);
    serial_write_line("'.");
}

/* 3) 初期化。確認に YES と打たせる。 */
static void do_initialize(void) {
    serial_write_line("This erases all files on the disk, including");
    serial_write_line("your files and the login PIN. Built-in files");
    serial_write_line("will be restored.");
    serial_write("Type YES to erase everything: ");
    char line[LINE_CAP];
    recovery_read_line(line, sizeof(line));
    if (strcmp(line, "YES") != 0) {
        serial_write_line("Cancelled.");
        return;
    }
    /* ユーザーのファイルを全部消して組み込みだけに戻す。 */
    vfs_init();
    persist_clear_key();
    if (!persist_disk_present()) {
        serial_write_line("Initialized in memory (no disk found).");
        return;
    }
    if (persist_initialize_disk() != 0) {
        serial_write_line("Initialize failed: could not write to disk.");
        return;
    }
    serial_write_line("Initialized. Only built-in files remain.");
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
        return;
    }
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
    static uint8_t file_buf[DISKFS_MAX_FILE_SIZE];
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
}

static void do_reboot(void) {
    serial_write_line("Rebooting PrismOS...");
    prism_outb(0x64, 0xFE);
    prism_halt();
}

void recovery_run(void) {
    for (;;) {
        print_menu();
        char line[LINE_CAP];
        recovery_read_line(line, sizeof(line));
        if (strcmp(line, "1") == 0) {
            do_repair();
        } else if (strcmp(line, "2") == 0) {
            do_restore();
        } else if (strcmp(line, "3") == 0) {
            do_initialize();
        } else if (strcmp(line, "4") == 0) {
            do_prev_repair();
        } else if (strcmp(line, "5") == 0) {
            serial_write_line("Continuing normal boot.");
            return;
        } else if (strcmp(line, "6") == 0) {
            do_reboot();
        } else {
            serial_write_line("Unknown choice.");
        }
    }
}
