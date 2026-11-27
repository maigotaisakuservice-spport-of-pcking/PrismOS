/* PrismOS — シェルコア (カーネルとホストテストで共有)
 *
 * 仮想ファイルシステム: v0.2 フェーズ2から、ls / cat が見る
 * ファイルは shell/vfs.c の RAM 上のファイル。作る・読む・
 * 消す・一覧ができる。ディスク書き込み (永続化) はこの
 * フェーズでは対象外で、電源を切ると消える。
 */
#include "core.h"

#include "../kernel/security.h"
#include "../kernel/settings.h"
#include "../kernel/version.h"
#include "vfs.h"

/* ---- 自前の文字列ヘルパー (libc 不使用) ---- */

static int s_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void s_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

/* ---- 出力バッファへの追記 ---- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} out_buf;

static void out_append(out_buf *o, const char *s) {
    while (*s != '\0' && o->len + 1 < o->cap) {
        o->buf[o->len++] = *s++;
    }
    o->buf[o->len] = '\0';
}

static void out_line(out_buf *o, const char *s) {
    out_append(o, s);
    out_append(o, "\n");
}

/* 長さ指定で追記する (NUL を含まない内容にも使える)。 */
static void out_append_n(out_buf *o, const char *s, size_t len) {
    for (size_t i = 0; i < len && o->len + 1 < o->cap; i++) {
        o->buf[o->len++] = s[i];
    }
    o->buf[o->len] = '\0';
}

/* 10 進で書く (wrote N bytes の表示用)。 */
static void out_u64(out_buf *o, uint64_t value) {
    char rev[20];
    size_t r = 0;
    if (value == 0) {
        out_append(o, "0");
        return;
    }
    while (value > 0) {
        rev[r++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (r > 0) {
        char one[2] = { rev[--r], '\0' };
        out_append(o, one);
    }
}

/* ---- 別名テーブル ---- */

typedef struct {
    const char *alias;
    const char *canonical;
} alias_entry;

static const alias_entry ALIASES[] = {
    { "dir", "ls" },   { "ls", "ls" },
    { "type", "cat" }, { "cat", "cat" },
    { "cls", "clear" }, { "clear", "clear" },
    { "version", "ver" }, { "ver", "ver" },
    { "?", "help" },   { "help", "help" },
    { "echo", "echo" }, { "pwd", "pwd" }, { "cd", "cd" },
    { "about", "about" }, { "theme", "theme" },
    { "reboot", "reboot" }, { "exit", "exit" },
    { "rm", "rm" }, { "del", "rm" },
    { "touch", "touch" },
    { "cp", "cp" }, { "mv", "mv" },
    { "mkdir", "mkdir" }, { "rmdir", "rmdir" },
    { "trash", "trash" },
};

const char *prism_shell_canonical(const char *name) {
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
        if (s_eq(name, ALIASES[i].alias)) {
            return ALIASES[i].canonical;
        }
    }
    return NULL;
}

/* ---- パーサ: 空白区切り + ダブルクォート対応 ---- */

static int parse_line(const char *line, char *argv_buf, char **argv,
                      int max_argv) {
    int argc = 0;
    size_t pos = 0;
    const char *p = line;

    while (*p != '\0' && argc < max_argv) {
        while (is_space(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        /* バッファが満杯なら残りの入力は捨てる。ガードが無いと
         * 下の NUL 書き込みが argv_buf[PRISM_SHELL_LINE_CAP] に
         * はみ出し、隣の argv 配列を壊す (v0.2 で修正)。 */
        if (pos >= PRISM_SHELL_LINE_CAP) {
            break;
        }
        argv[argc] = argv_buf + pos;
        if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"' &&
                   pos + 1 < PRISM_SHELL_LINE_CAP) {
                argv_buf[pos++] = *p++;
            }
            if (*p == '"') {
                p++;
            }
        } else {
            while (*p != '\0' && !is_space(*p) &&
                   pos + 1 < PRISM_SHELL_LINE_CAP) {
                argv_buf[pos++] = *p++;
            }
        }
        argv_buf[pos++] = '\0';
        argc++;
    }
    return argc;
}

/* ---- コマンド実装 ---- */


/* ヘルプの 1 行を固定幅カラムに整形する (C1)。
 * "  cmd<空白>desc" の形は cmd を 36 文字幅にそろえ、
 * desc が 78 桁を超えたら継続行を同じ桁から折り返す。 */
static int help_disp_w(const char *s, size_t n) {
    int w = 0;
    size_t i = 0;
    while (i < n && s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            w += 1;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            w += 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            w += 2;
            i += 3;
        } else {
            w += 2;
            i += 4;
        }
    }
    return w;
}

static void help_line(out_buf *o, const char *s) {
    if (s[0] != ' ' || s[1] != ' ') {
        out_line(o, s);
        return;
    }
    int split = -1;
    for (int i = 2; s[i] != '\0'; i++) {
        if (s[i] == ' ' && s[i + 1] == ' ') {
            split = i;
            break;
        }
    }
    if (split <= 2) {
        out_line(o, s);
        return;
    }
    const char *desc = s + split;
    while (*desc == ' ') {
        desc++;
    }
    if (*desc == '\0') {
        out_line(o, s);
        return;
    }
    /* 1 行目: "  " + cmd + 28 桁までの空白 + desc の先頭。 */
    char line[160];
    size_t p = 0;
    line[p++] = ' ';
    line[p++] = ' ';
    for (int i = 2; i < split && p + 1 < sizeof(line); i++) {
        line[p++] = s[i];
    }
    int cw = help_disp_w(s + 2, (size_t)(split - 2));
    while (cw < 28 && p + 1 < sizeof(line)) {
        line[p++] = ' ';
        cw++;
    }
    if (cw >= 28) {
        /* 欄より長いコマンドは空白 2 つで区切る。 */
        if (p + 2 < sizeof(line)) {
            line[p++] = ' ';
            line[p++] = ' ';
        }
        cw += 2;
    }
    line[p] = '\0';
    /* desc を単語単位で 78 桁に折る。 */
    const int indent = 30;
    const int width = 80;
    int col = indent;
    const char *w = desc;
    int first = 1;
    while (*w != '\0') {
        const char *e = w;
        while (*e != '\0' && *e != ' ') {
            e++;
        }
        int ww = help_disp_w(w, (size_t)(e - w));
        if (!first && col + 1 + ww > width) {
            out_line(o, line);
            p = 0;
            for (int i = 0; i < indent && p + 1 < sizeof(line);
                 i++) {
                line[p++] = ' ';
            }
            line[p] = '\0';
            col = indent;
            first = 1;
        }
        if (!first && p + 1 < sizeof(line)) {
            line[p++] = ' ';
            col++;
        }
        for (const char *q = w; q < e && p + 1 < sizeof(line);
             q++) {
            line[p++] = *q;
        }
        line[p] = '\0';
        col += ww;
        first = 0;
        w = e;
        while (*w == ' ') {
            w++;
        }
    }
    out_line(o, line);
}

static void cmd_help_en(out_buf *o) {
    help_line(o, "PrismOS shell — two command styles work side by side.");
    help_line(o, "");
    help_line(o, "  ls, dir          list files");
    help_line(o, "  cat, type <file> show a file");
    help_line(o, "  pwd              print working directory");
    help_line(o, "  cd <dir>         change directory");
    help_line(o, "  echo <text>      print text");
    help_line(o, "  echo <text> > <file>  write text to a file");
    help_line(o, "  touch <file>     create an empty file");
    help_line(o, "  rm, del <file>   remove a file");
    help_line(o, "  cp <src> <dst>   copy a file");
    help_line(o, "  mv <src> <dst>   move (rename) a file");
    help_line(o, "  clear, cls       clear the screen");
    help_line(o, "  ver, version     show PrismOS version");
    help_line(o, "  about            about PrismOS");
    help_line(o, "  theme            show the current theme");
    help_line(o, "  install yes      install PrismOS to the disk");
    help_line(o, "  install dual yes install to a dual-boot disk (menu and other system untouched)");
    help_line(o, "  passwd           set the login PIN");
    help_line(o, "  security         show security settings (admin PIN needed to change)");
    help_line(o, "  security usb <allow|block>  allow or block external media");
    help_line(o, "  security pin <min_len> <digit>  set the PIN policy");
    help_line(o, "  security lockout <attempts> <seconds>  set the lockout");
    help_line(o, "  security allow|deny <app>  allow or deny an app");
    help_line(o, "  security remote setup <host> [path]  arm remote lock/wipe");
    help_line(o, "  audit            show the audit log (last 10 entries)");
    help_line(o, "  audit verify     verify the audit log chain");
    help_line(o, "  fde              full-disk encryption status");
    help_line(o, "  fde enable       encrypt the whole data disk (admin)");
    help_line(o, "  fde disable      remove full-disk encryption (admin)");
    help_line(o, "  snapshot         create a restore point");
    help_line(o, "  snapshot list    list restore points");
    help_line(o, "  snapshot restore <name>  restore a restore point");
    help_line(o, "  snapshot delete <name>   delete a restore point");
    help_line(o, "  update check     check for updates");
    help_line(o, "  update apply [yes]  apply an update (feature updates need yes)");
    help_line(o, "  update channel <stable|beta>  choose the update channel");
    help_line(o, "  update auto <on|off>  auto-apply security updates at boot");
    help_line(o, "  update server <host> [path]  set the update server");
    help_line(o, "  update rollback  roll back to the pre-update restore point");
    help_line(o, "  recovery         reboot into recovery mode");
    help_line(o, "  net              show network status (DHCP)");
    help_line(o, "  disk             show the system disk (kind/model/size)");
    help_line(o, "  usb              list USB devices (UHCI)");
    help_line(o, "  fetch <host> [path]  fetch a plain HTTP page");
    help_line(o, "  mail             list mail");
    help_line(o, "  mail setup <smtp_host> <smtp_port> <pop3_host> <pop3_port> <user> <pass> <from>  set the mail server");
    help_line(o, "  mail send <to> <subject> <body...>  send mail");
    help_line(o, "  mail recv        receive mail (POP3)");
    help_line(o, "  mail read <number>  read a message");
    help_line(o, "  mail del <number>   delete a message");
    help_line(o, "  dnstest <server> resolve example.com via a DNS server");
    help_line(o, "  beep <freq> <ms> play a tone on the PC speaker");
    help_line(o, "  play <file>      play an 8-bit mono WAV file");
    help_line(o, "  run <file> [args...]  run a compat app (ELF executable)");
    help_line(o, "  set NAME=value / env / unset NAME  shell variables ($NAME expands)");
    help_line(o, "  history  list recent commands (Up/Down keys recall them)");
    help_line(o, "  script <file>  run commands from a file, one per line");
    help_line(o, "  <cmd> | grep <word> / tee <file> / run <app> / cat  pipe output");
    help_line(o, "  compat info <file>  show ELF info of a compat app");
    help_line(o, "  compat list      list compat apps");
    help_line(o, "  compat calls     show the supported compat calls");
    help_line(o, "  help             show this help");
    help_line(o, "  reboot           reboot the system");
    help_line(o, "  mem              show memory usage");
    help_line(o, "  date [set YYYY-MM-DD HH:MM:SS]  show or set the clock");
    help_line(o, "  tz [+HHMM]       show or set the timezone offset");
    help_line(o, "  ntp [server]     set the clock from an NTP server");
    help_line(o, "  ping <host>      ICMP echo test (3 tries)");
    help_line(o, "  fw list/add/clear  firewall drop rules (fw.txt)");
    help_line(o, "  mkdir/rmdir/cd   directories (ls shows them)");
    help_line(o, "  trash <file>     move to trash (trash list/restore/empty)");
    help_line(o, "  user list/add/del  user accounts (admin)");
    help_line(o, "  totp setup/off   two-factor authentication");
    help_line(o, "  mail folders / mail move <n> <folder>  mail folders");
    help_line(o, "  power / sleep / poweroff  power status, S3 sleep, S5 off");
    help_line(o, "  mail tls <on|off>  use TLS for mail (with a cert pin)");
    help_line(o, "  mail imap <host> <port>  receive mail via IMAP");
    help_line(o, "  mail pin <sha256>  set the mail server certificate pin");
    help_line(o, "  security status  show security settings");
    help_line(o, "  security remote  show remote lock/wipe state");
    help_line(o, "  pq keygen|encaps|decaps|seal|open  post-quantum key exchange and sealing");
    help_line(o, "  pq signkey|sign|verify  post-quantum signatures");
    help_line(o, "  bio enroll|verify|status  fingerprint/face (simulated sensor)");
    help_line(o, "  lock             lock the session (PIN or biometrics)");
    help_line(o, "  usb test         write/read test on USB storage");
    help_line(o, "  exit             (no exit yet; use reboot)");
}

static void cmd_help_ja(out_buf *o) {
    help_line(o, "PrismOS シェル — 2 つの書き方のコマンドが使えます。");
    help_line(o, "");
    help_line(o, "  ls, dir          ファイルの一覧を表示する");
    help_line(o, "  cat, type <ファイル>  ファイルの中身を表示する");
    help_line(o, "  pwd              今いるフォルダを表示する");
    help_line(o, "  cd <フォルダ>     フォルダを移動する");
    help_line(o, "  echo <文字>       文字を表示する");
    help_line(o, "  echo <文字> > <ファイル>  ファイルに文字を書く");
    help_line(o, "  touch <ファイル>  空のファイルを作る");
    help_line(o, "  rm, del <ファイル>  ファイルを消す");
    help_line(o, "  cp <元> <先>      ファイルを複写する");
    help_line(o, "  mv <元> <先>      ファイルを移動する（名前を変える）");
    help_line(o, "  clear, cls       画面を消す");
    help_line(o, "  ver, version     バージョンを表示する");
    help_line(o, "  about            この OS について表示する");
    help_line(o, "  theme            今のテーマを表示する");
    help_line(o, "  mem              メモリの使用量を表示する");
    help_line(o, "  install yes      ディスクにインストールする");
    help_line(o, "  install dual yes デュアルブート用にインストールする");
    help_line(o, "  passwd           ログイン用の暗証番号を決める");
    help_line(o, "  security         セキュリティ設定を表示する");
    help_line(o, "  security status  セキュリティ設定を表示する");
    help_line(o, "  security usb <allow|block>  外部メディアの許可・禁止");
    help_line(o, "  security pin <最小長> <数字>  暗証番号の規則を決める");
    help_line(o, "  security lockout <回数> <秒>  ロックアウトを決める");
    help_line(o, "  security allow|deny <アプリ>  アプリの許可・禁止");
    help_line(o, "  security remote  遠隔ロック・消去の状態を表示する");
    help_line(o, "  audit            監査ログを表示する（最新 10 件）");
    help_line(o, "  audit verify     監査ログの連鎖を検証する");
    help_line(o, "  fde              全ディスク暗号化の状態を表示する");
    help_line(o, "  fde enable       データディスク全体を暗号化する（管理者）");
    help_line(o, "  fde disable      全ディスク暗号化を解除する（管理者）");
    help_line(o, "  snapshot         復元ポイントを作る");
    help_line(o, "  snapshot list    復元ポイントの一覧を表示する");
    help_line(o, "  snapshot restore <名前>  復元ポイントに戻す");
    help_line(o, "  snapshot delete <名前>   復元ポイントを消す");
    help_line(o, "  update check     更新があるか確認する");
    help_line(o, "  update apply yes 更新を適用する");
    help_line(o, "  update channel <stable|beta>  更新の種類を選ぶ");
    help_line(o, "  update auto <on|off>  セキュリティ更新の自動適用");
    help_line(o, "  update server <ホスト> [パス]  更新サーバを決める");
    help_line(o, "  update rollback  更新前の復元ポイントに戻す");
    help_line(o, "  recovery         回復モードで再起動する");
    help_line(o, "  net              ネットワークの状態を表示する");
    help_line(o, "  disk             システムディスクを表示する");
    help_line(o, "  usb              USB 機器の一覧を表示する");
    help_line(o, "  usb test         USB ストレージの読み書きを試す");
    help_line(o, "  fetch <ホスト> [パス]  HTTP ページを取得する");
    help_line(o, "  mail             メールの一覧を表示する");
    help_line(o, "  mail setup <送信ホスト> <ポート> <受信ホスト> <ポート> <利用者> <パスワード> <差出人>  メールサーバを設定する");
    help_line(o, "  mail send <宛先> <件名> <本文...>  メールを送る");
    help_line(o, "  mail recv        メールを受信する");
    help_line(o, "  mail read <番号>  メールを読む");
    help_line(o, "  mail del <番号>   メールを消す");
    help_line(o, "  mail tls <on|off>  メールの TLS を使う・使わない");
    help_line(o, "  mail imap <ホスト> <ポート>  IMAP で受信する");
    help_line(o, "  mail pin <sha256>  メールサーバの証明書の指紋を決める");
    help_line(o, "  pq keygen|encaps|decaps  耐量子の鍵交換をする");
    help_line(o, "  pq seal|open <ファイル>  耐量子の鍵で封をする・開ける");
    help_line(o, "  pq signkey|sign|verify  耐量子の署名をする・確かめる");
    help_line(o, "  bio enroll|verify|status  指紋・顔の登録と照合（模擬センサー）");
    help_line(o, "  lock             画面をロックする（暗証番号か生体認証）");
    help_line(o, "  dnstest <サーバ>  DNS で名前を引く");
    help_line(o, "  beep <周波数> <ミリ秒>  スピーカーを鳴らす");
    help_line(o, "  play <ファイル>   WAV ファイルを再生する");
    help_line(o, "  run <ファイル> [引数...]  互換アプリを実行する");
    help_line(o, "  compat info <ファイル>  互換アプリの情報を表示する");
    help_line(o, "  compat list      互換アプリの一覧を表示する");
    help_line(o, "  compat calls     互換レイヤーの呼び出し一覧を表示する");
    help_line(o, "  help             このヘルプを表示する");
    help_line(o, "  exit             （今は終了先がありません。再起動を使ってください）");
    help_line(o, "  date [set ...]   日時を表示・設定する");
    help_line(o, "  tz [+HHMM]       タイムゾーンを表示・設定する");
    help_line(o, "  ntp [サーバ]     NTP サーバで時計を合わせる");
    help_line(o, "  ping <ホスト>    ICMP エコーで到達確認 (3 回)");
    help_line(o, "  fw list/add/clear  ファイアウォールの遮断ルール");
    help_line(o, "  mkdir/rmdir/cd   ディレクトリ (ls に表示)");
    help_line(o, "  trash <ファイル> ゴミ箱へ (trash list/restore/empty)");
    help_line(o, "  user list/add/del  ユーザーアカウント (管理者)");
    help_line(o, "  totp setup/off   2 要素認証");
    help_line(o, "  mail folders     メールのフォルダ一覧");
    help_line(o, "  reboot           再起動する");
}

static void cmd_help(out_buf *o, int language) {
    if (language == 1) {
        cmd_help_ja(o);
    } else {
        cmd_help_en(o);
    }
}

static void cmd_about(out_buf *o) {
    out_append(o, "PrismOS ");
    out_line(o, PRISM_VERSION);
    out_line(o, "A customizable OS: modern centered UI, Dock and quick search");
    out_line(o, "(Dock, quick search, window overview), and a shell with");
    out_line(o, "two command styles.");
    out_line(o, "License: MIT — https://github.com/ (see README.md)");
}

/* cwd に対して名前を解決する。"/" 始まりはルートから。
 * 結果は VFS 名 (先頭に "/" なし)。 */
static void resolve_path(prism_shell_state *state, const char *name,
                         char *out, size_t cap) {
    size_t p = 0;
    const char *src = name;
    if (src[0] == '/') {
        src++;
    } else if (!(state->cwd[0] == '/' && state->cwd[1] == '\0')) {
        const char *cwd = state->cwd + 1; /* 先頭の "/" を除く */
        while (*cwd != '\0' && p + 1 < cap) {
            out[p++] = *cwd++;
        }
        if (p + 1 < cap) {
            out[p++] = '/';
        }
    }
    while (*src != '\0' && p + 1 < cap) {
        out[p++] = *src++;
    }
    out[p] = '\0';
}

/* ディレクトリが存在するか (マーカーか、中に
 * ファイルがあるか)。dir は VFS 名 ("" はルート)。 */
static int dir_exists(const char *dir) {
    if (dir[0] == '\0') {
        return 1;
    }
    char marker[40];
    size_t p = 0;
    while (dir[p] != '\0' && p + 1 < sizeof(marker)) {
        marker[p] = dir[p];
        p++;
    }
    const char *sfx = "/.dir";
    size_t q = 0;
    while (sfx[q] != '\0' && p + 1 < sizeof(marker)) {
        marker[p++] = sfx[q++];
    }
    marker[p] = '\0';
    if (vfs_exists(marker)) {
        return 1;
    }
    size_t dl = 0;
    while (dir[dl] != '\0') {
        dl++;
    }
    int count = vfs_file_count();
    for (int i = 0; i < count; i++) {
        const char *n = vfs_file_name(i);
        size_t k = 0;
        while (k < dl && n[k] == dir[k]) {
            k++;
        }
        if (k == dl && n[dl] == '/') {
            return 1;
        }
    }
    return 0;
}

static void cmd_ls(out_buf *o, prism_shell_state *state,
                   const char *canonical, int argc) {
    (void)argc;
    int table = s_eq(canonical, "dir");
    /* cwd の接頭辞 ("" = ルート)。 */
    char prefix[40];
    size_t pl = 0;
    if (!(state->cwd[0] == '/' && state->cwd[1] == '\0')) {
        const char *cwd = state->cwd + 1;
        while (*cwd != '\0' && pl + 1 < sizeof(prefix)) {
            prefix[pl++] = *cwd++;
        }
        if (pl + 1 < sizeof(prefix)) {
            prefix[pl++] = '/';
        }
    }
    prefix[pl] = '\0';
    if (table) {
        out_line(o, " Volume in drive C is PRISMOS");
        out_line(o, " Directory of C:\\");
        out_line(o, "");
    }
    int count = vfs_file_count();
    int shown = 0;
    char dirs[32][32];
    int ndirs = 0;
    for (int i = 0; i < count; i++) {
        const char *n = vfs_file_name(i);
        size_t k = 0;
        while (k < pl && n[k] == prefix[k]) {
            k++;
        }
        if (k != pl) {
            continue;
        }
        const char *rest = n + pl;
        if (rest[0] == '\0') {
            continue;
        }
        /* サブディレクトリか */
        const char *slash = rest;
        while (*slash != '\0' && *slash != '/') {
            slash++;
        }
        if (*slash == '/') {
            char dname[32];
            size_t dl = (size_t)(slash - rest);
            if (dl >= sizeof(dname)) {
                continue;
            }
            for (size_t j = 0; j < dl; j++) {
                dname[j] = rest[j];
            }
            dname[dl] = '\0';
            int seen = 0;
            for (int j = 0; j < ndirs; j++) {
                if (s_eq(dirs[j], dname)) {
                    seen = 1;
                }
            }
            if (!seen && ndirs < 32) {
                s_copy(dirs[ndirs], dname, sizeof(dirs[ndirs]));
                ndirs++;
                if (table) {
                    out_append(o, "  <DIR>   ");
                    out_line(o, dname);
                } else {
                    out_append(o, dname);
                    out_line(o, "/");
                }
                shown++;
            }
            continue;
        }
        if (s_eq(rest, ".dir")) {
            continue; /* マーカーは見せない */
        }
        if (table) {
            out_append(o, "  <FILE>  ");
            out_line(o, rest);
        } else {
            out_line(o, rest);
        }
        shown++;
    }
    if (table) {
        out_line(o, "");
        out_append(o, "               ");
        out_u64(o, (uint64_t)shown);
        out_line(o, " File(s)");
    }
}

static void cmd_cat(out_buf *o, prism_shell_state *state, int argc, char **argv) {
    if (argc < 2) {
        out_line(o, "usage: cat <file>   (type <file> also works)");
        return;
    }
    char path[40];
    resolve_path(state, argv[1], path, sizeof(path));
    size_t len = 0;
    if (!vfs_exists(path)) {
        out_append(o, "cat: ");
        out_append(o, argv[1]);
        out_line(o, ": No such file");
        return;
    }
    const char *content = vfs_read_ptr(path, &len);
    if (len == 0) {
        out_append(o, "\n");
        return;
    }
    out_append_n(o, content, len);
    if (len == 0 || content[len - 1] != '\n') {
        out_append(o, "\n");
    }
}

/* echo: 通常は表示だけ。`>` があればファイルに書き込む。 */
static void cmd_echo(out_buf *o, int argc, char **argv) {
    int redir = -1;
    for (int i = 1; i < argc; i++) {
        if (s_eq(argv[i], ">")) {
            redir = i;
            break;
        }
    }
    if (redir >= 0) {
        if (redir + 1 >= argc || redir + 2 < argc) {
            out_line(o, "usage: echo <text> > <file>");
            return;
        }
        /* 書き込む内容: argv[1..redir-1] を空白でつないで改行を付ける */
        char content[PRISM_SHELL_LINE_CAP];
        size_t len = 0;
        for (int i = 1; i < redir; i++) {
            if (i > 1 && len + 1 < sizeof(content)) {
                content[len++] = ' ';
            }
            for (size_t j = 0; argv[i][j] != '\0' &&
                 len + 1 < sizeof(content); j++) {
                content[len++] = argv[i][j];
            }
        }
        if (len + 1 < sizeof(content)) {
            content[len++] = '\n';
        }
        content[len] = '\0';
        int rc = vfs_write(argv[redir + 1], content, len);
        if (rc == 0) {
            out_append(o, "wrote ");
            out_u64(o, (uint64_t)len);
            out_append(o, " bytes to ");
            out_line(o, argv[redir + 1]);
        } else if (rc == -1) {
            out_line(o, "echo: file is too large");
        } else if (rc == -2) {
            out_line(o, "echo: too many files");
        } else {
            out_line(o, "echo: bad file name");
        }
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            out_append(o, " ");
        }
        out_append(o, argv[i]);
    }
    out_append(o, "\n");
}

static void cmd_rm(out_buf *o, prism_shell_state *state, int argc, char **argv) {
    if (argc < 2) {
        out_line(o, "usage: rm <file>   (del <file> also works)");
        return;
    }
    char path[40];
    resolve_path(state, argv[1], path, sizeof(path));
    if (vfs_delete(path) == 0) {
        audit_log_event("rm", argv[1]);
        out_append(o, "removed ");
        out_line(o, argv[1]);
    } else {
        out_append(o, "rm: ");
        out_append(o, argv[1]);
        out_line(o, ": No such file");
    }
}

static void cmd_touch(out_buf *o, prism_shell_state *state, int argc, char **argv) {
    if (argc < 2) {
        out_line(o, "usage: touch <file>");
        return;
    }
    char path[40];
    resolve_path(state, argv[1], path, sizeof(path));
    if (vfs_exists(path)) {
        return; /* すでにあるファイルの中身は変えない */
    }
    int rc = vfs_write(path, "", 0);
    if (rc == -2) {
        out_line(o, "touch: too many files");
    } else if (rc != 0) {
        out_line(o, "touch: bad file name");
    }
}

static void cmd_cp(out_buf *o, prism_shell_state *state, int argc, char **argv) {
    if (argc < 3) {
        out_line(o, "usage: cp <source> <dest>");
        return;
    }
    char src[40], dst[40];
    resolve_path(state, argv[1], src, sizeof(src));
    resolve_path(state, argv[2], dst, sizeof(dst));
    if (s_eq(src, dst)) {
        out_line(o, "cp: source and dest are the same file");
        return;
    }
    size_t len = 0;
    if (!vfs_exists(src)) {
        out_append(o, "cp: ");
        out_append(o, argv[1]);
        out_line(o, ": No such file");
        return;
    }
    const char *content = vfs_read_ptr(src, &len);
    int rc = vfs_write(dst, content, len);
    if (rc == 0) {
        out_append(o, "copied ");
        out_append(o, argv[1]);
        out_append(o, " to ");
        out_line(o, argv[2]);
    } else if (rc == -1) {
        out_line(o, "cp: file is too large");
    } else if (rc == -2) {
        out_line(o, "cp: too many files");
    } else {
        out_line(o, "cp: bad file name");
    }
}

static void cmd_mv(out_buf *o, prism_shell_state *state, int argc, char **argv) {
    if (argc < 3) {
        out_line(o, "usage: mv <source> <dest>");
        return;
    }
    char src[40], dst[40];
    resolve_path(state, argv[1], src, sizeof(src));
    resolve_path(state, argv[2], dst, sizeof(dst));
    if (s_eq(src, dst)) {
        out_line(o, "mv: source and dest are the same file");
        return;
    }
    size_t len = 0;
    if (!vfs_exists(src)) {
        out_append(o, "mv: ");
        out_append(o, argv[1]);
        out_line(o, ": No such file");
        return;
    }
    const char *content = vfs_read_ptr(src, &len);
    int rc = vfs_write(dst, content, len);
    if (rc == -1) {
        out_line(o, "mv: file is too large");
        return;
    } else if (rc == -2) {
        out_line(o, "mv: too many files");
        return;
    } else if (rc != 0) {
        out_line(o, "mv: bad file name");
        return;
    }
    if (vfs_delete(src) == 0) {
        audit_log_event("mv", argv[1]);
        out_append(o, "moved ");
        out_append(o, argv[1]);
        out_append(o, " to ");
        out_line(o, argv[2]);
    } else {
        out_line(o, "mv: could not remove the source file");
    }
}

static void cmd_mkdir(out_buf *o, prism_shell_state *state, int argc,
                      char **argv) {
    if (argc < 2) {
        out_line(o, "usage: mkdir <dir>");
        return;
    }
    char path[40];
    resolve_path(state, argv[1], path, sizeof(path));
    if (dir_exists(path)) {
        out_append(o, "mkdir: ");
        out_append(o, argv[1]);
        out_line(o, ": already exists");
        return;
    }
    char marker[40];
    size_t p = 0;
    while (path[p] != '\0' && p + 1 < sizeof(marker)) {
        marker[p] = path[p];
        p++;
    }
    const char *sfx = "/.dir";
    size_t q = 0;
    while (sfx[q] != '\0' && p + 1 < sizeof(marker)) {
        marker[p++] = sfx[q++];
    }
    marker[p] = '\0';
    if (p >= sizeof(marker) || vfs_write(marker, "", 0) != 0) {
        out_line(o, "mkdir: could not create (name too long?)");
        return;
    }
    out_append(o, "created directory ");
    out_line(o, argv[1]);
}

static void cmd_rmdir(out_buf *o, prism_shell_state *state, int argc,
                      char **argv) {
    if (argc < 2) {
        out_line(o, "usage: rmdir <dir>");
        return;
    }
    char path[40];
    resolve_path(state, argv[1], path, sizeof(path));
    if (!dir_exists(path) || path[0] == '\0') {
        out_append(o, "rmdir: ");
        out_append(o, argv[1]);
        out_line(o, ": No such directory");
        return;
    }
    /* マーカー以外に中身があれば拒否する。 */
    size_t pl = 0;
    while (path[pl] != '\0') {
        pl++;
    }
    int count = vfs_file_count();
    for (int i = 0; i < count; i++) {
        const char *n = vfs_file_name(i);
        size_t k = 0;
        while (k < pl && n[k] == path[k]) {
            k++;
        }
        if (k == pl && n[pl] == '/' && !s_eq(n + pl + 1, ".dir")) {
            out_line(o, "rmdir: directory not empty");
            return;
        }
    }
    char marker[40];
    size_t p = 0;
    while (path[p] != '\0' && p + 1 < sizeof(marker)) {
        marker[p] = path[p];
        p++;
    }
    const char *sfx = "/.dir";
    size_t q = 0;
    while (sfx[q] != '\0' && p + 1 < sizeof(marker)) {
        marker[p++] = sfx[q++];
    }
    marker[p] = '\0';
    vfs_delete(marker);
    out_append(o, "removed directory ");
    out_line(o, argv[1]);
}

/* ゴミ箱: "trash/" の下へ移動する。restore で cwd へ
 * 戻す。empty で完全に消す。 */
static void cmd_trash(out_buf *o, prism_shell_state *state, int argc,
                      char **argv) {
    if (argc >= 2 && s_eq(argv[1], "list")) {
        int count = vfs_file_count();
        int shown = 0;
        for (int i = 0; i < count; i++) {
            const char *n = vfs_file_name(i);
            if (n[0] == 't' && n[1] == 'r' && n[2] == 'a' &&
                n[3] == 's' && n[4] == 'h' && n[5] == '/' &&
                !s_eq(n + 6, ".dir")) {
                out_line(o, n + 6);
                shown++;
            }
        }
        if (shown == 0) {
            out_line(o, "(trash is empty)");
        }
        return;
    }
    if (argc >= 2 && s_eq(argv[1], "empty")) {
        int count = vfs_file_count();
        char names[PRISM_VFS_MAX_FILES][32];
        int m = 0;
        for (int i = 0; i < count; i++) {
            const char *n = vfs_file_name(i);
            if (n[0] == 't' && n[1] == 'r' && n[2] == 'a' &&
                n[3] == 's' && n[4] == 'h' && n[5] == '/') {
                s_copy(names[m], n, sizeof(names[m]));
                m++;
            }
        }
        for (int i = 0; i < m; i++) {
            vfs_delete(names[i]);
        }
        out_line(o, "trash emptied");
        return;
    }
    if (argc >= 3 && s_eq(argv[1], "restore")) {
        char src[40];
        size_t p = 0;
        const char *pre = "trash/";
        while (pre[p] != '\0' && p + 1 < sizeof(src)) {
            src[p] = pre[p];
            p++;
        }
        size_t q = 0;
        while (argv[2][q] != '\0' && p + 1 < sizeof(src)) {
            src[p++] = argv[2][q++];
        }
        src[p] = '\0';
        if (!vfs_exists(src)) {
            out_append(o, "trash: ");
            out_append(o, argv[2]);
            out_line(o, ": not in trash");
            return;
        }
        char dst[40];
        resolve_path(state, argv[2], dst, sizeof(dst));
        if (vfs_exists(dst)) {
            out_line(o, "trash: a file with that name exists here");
            return;
        }
        size_t len = 0;
        const char *content = vfs_read_ptr(src, &len);
        if (vfs_write(dst, content, len) != 0) {
            out_line(o, "trash: could not restore");
            return;
        }
        vfs_delete(src);
        out_append(o, "restored ");
        out_line(o, argv[2]);
        return;
    }
    if (argc >= 2) {
        char path[40];
        resolve_path(state, argv[1], path, sizeof(path));
        if (!vfs_exists(path)) {
            out_append(o, "trash: ");
            out_append(o, argv[1]);
            out_line(o, ": No such file");
            return;
        }
        /* ベース名を取り出す。 */
        const char *base = path;
        for (const char *s = path; *s != '\0'; s++) {
            if (*s == '/') {
                base = s + 1;
            }
        }
        if (!vfs_exists("trash/.dir")) {
            vfs_write("trash/.dir", "", 0);
        }
        char dst[40];
        for (int attempt = 0; attempt < 100; attempt++) {
            size_t p = 0;
            const char *pre = "trash/";
            while (pre[p] != '\0' && p + 1 < sizeof(dst)) {
                dst[p] = pre[p];
                p++;
            }
            size_t q = 0;
            while (base[q] != '\0' && p + 1 < sizeof(dst)) {
                dst[p++] = base[q++];
            }
            if (attempt > 0) {
                if (p + 2 < sizeof(dst)) {
                    dst[p++] = '-';
                    dst[p++] = (char)('0' + attempt);
                }
            }
            dst[p] = '\0';
            if (!vfs_exists(dst)) {
                break;
            }
            if (attempt == 99) {
                out_line(o, "trash: too many same-name files");
                return;
            }
        }
        size_t len = 0;
        const char *content = vfs_read_ptr(path, &len);
        if (vfs_write(dst, content, len) != 0) {
            out_line(o, "trash: could not move (name too long?)");
            return;
        }
        vfs_delete(path);
        audit_log_event("trash", argv[1]);
        out_append(o, "trashed ");
        out_line(o, argv[1]);
        return;
    }
    out_line(o, "usage: trash <file> | trash list | trash restore "
                "<name> | trash empty");
}

static void cmd_theme(out_buf *o) {
    /* Read the real settings: this used to print fixed
     * strings (the defaults, and a wallpaper file name
     * that does not exist) even after the user changed
     * them in the Settings app. */
    prism_settings s;
    settings_defaults(&s);
    size_t len = 0;
    const char *text = vfs_read_ptr("settings.txt", &len);
    if (text != NULL) {
        settings_parse(&s, text, len);
    }
    char hex[8];
    settings_accent_hex(s.accent_rgb, hex);
    out_line(o, "[theme]  (from settings.txt)");
    out_append(o, "accent = \"");
    out_append(o, hex);
    out_line(o, "\"");
    out_line(o, s.wallpaper_image ? "wallpaper = \"image\""
                                  : "wallpaper = \"drawn\"");
    out_line(o, s.taskbar_top ? "taskbar_position = \"top\""
                              : "taskbar_position = \"bottom\"");
    out_line(o, s.dock_show ? "dock = \"shown\""
                            : "dock = \"hidden\"");
    out_line(o, s.dark_mode ? "mode = \"dark\""
                            : "mode = \"light\"");
}

/* ---- エントリポイント ---- */

void prism_shell_init(prism_shell_state *state) {
    s_copy(state->cwd, "/", sizeof(state->cwd));
    state->should_reboot = 0;
    state->should_clear = 0;
    state->language = 0;
    vfs_init();
}

int prism_shell_execute(prism_shell_state *state, const char *line,
                        char *out, size_t out_cap) {
    char argv_buf[PRISM_SHELL_LINE_CAP];
    char *argv[PRISM_SHELL_MAX_ARGS];
    out_buf o = { out, out_cap, 0 };
    if (out_cap > 0) {
        out[0] = '\0';
    }

    int argc = parse_line(line, argv_buf, argv, PRISM_SHELL_MAX_ARGS);
    if (argc == 0) {
        return 0;
    }

    const char *cmd = prism_shell_canonical(argv[0]);
    if (cmd == NULL) {
        out_append(&o, "Unknown command: '");
        out_append(&o, argv[0]);
        out_line(&o, "'");
        out_line(&o, "Type `help` to see available commands.");
        return 0;
    }

    if (s_eq(cmd, "help")) {
        cmd_help(&o, state->language);
    } else if (s_eq(cmd, "about")) {
        cmd_about(&o);
    } else if (s_eq(cmd, "ver")) {
        out_append(&o, "PrismOS version ");
        out_line(&o, PRISM_VERSION);
    } else if (s_eq(cmd, "echo")) {
        cmd_echo(&o, argc, argv);
    } else if (s_eq(cmd, "rm")) {
        cmd_rm(&o, state, argc, argv);
    } else if (s_eq(cmd, "touch")) {
        cmd_touch(&o, state, argc, argv);
    } else if (s_eq(cmd, "cp")) {
        cmd_cp(&o, state, argc, argv);
    } else if (s_eq(cmd, "mv")) {
        cmd_mv(&o, state, argc, argv);
    } else if (s_eq(cmd, "pwd")) {
        out_line(&o, state->cwd);
    } else if (s_eq(cmd, "ls")) {
        /* 呼び出し名が dir なら表形式、ls なら名前だけの形式で表示 */
        cmd_ls(&o, state, argv[0], argc);
    } else if (s_eq(cmd, "cat")) {
        cmd_cat(&o, state, argc, argv);
    } else if (s_eq(cmd, "mkdir")) {
        cmd_mkdir(&o, state, argc, argv);
    } else if (s_eq(cmd, "rmdir")) {
        cmd_rmdir(&o, state, argc, argv);
    } else if (s_eq(cmd, "trash")) {
        cmd_trash(&o, state, argc, argv);
    } else if (s_eq(cmd, "cd")) {
        if (argc < 2 || s_eq(argv[1], "/")) {
            s_copy(state->cwd, "/", sizeof(state->cwd));
        } else if (s_eq(argv[1], "..")) {
            /* 親へ (ルートの親はルート)。 */
            if (!(state->cwd[0] == '/' && state->cwd[1] == '\0')) {
                size_t n = 0;
                while (state->cwd[n] != '\0') {
                    n++;
                }
                while (n > 1 && state->cwd[n - 1] != '/') {
                    n--;
                }
                state->cwd[n - 1] = '\0';
                if (state->cwd[0] == '\0') {
                    s_copy(state->cwd, "/", sizeof(state->cwd));
                }
            }
        } else if (s_eq(argv[1], ".")) {
            /* そのまま */
        } else {
            char path[40];
            resolve_path(state, argv[1], path, sizeof(path));
            if (dir_exists(path)) {
                state->cwd[0] = '/';
                size_t p = 1;
                size_t q = 0;
                while (path[q] != '\0' &&
                       p + 1 < sizeof(state->cwd)) {
                    state->cwd[p++] = path[q++];
                }
                state->cwd[p] = '\0';
            } else {
                out_append(&o, "cd: ");
                out_append(&o, argv[1]);
                out_line(&o, ": No such directory");
            }
        }
    } else if (s_eq(cmd, "clear")) {
        state->should_clear = 1;
    } else if (s_eq(cmd, "theme")) {
        cmd_theme(&o);
    } else if (s_eq(cmd, "reboot")) {
        state->should_reboot = 1;
        out_line(&o, "Rebooting...");
    } else if (s_eq(cmd, "exit")) {
        out_line(&o, "There is nowhere to exit to in PrismOS yet. Use `reboot`.");
    }
    return 0;
}
