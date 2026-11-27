/* PrismOS — セキュリティの純粋ロジック (フェーズ14)
 *
 * ハードウェアに触らないので、カーネルとホストテストで
 * 同じソースを使う。
 *
 *   * エントロピープール: キー・マウス・時刻などの出来事を
 *     混ぜ、SHA-256 で乱数バイト列を作る。同じ出来事の列
 *     からは同じ列が出る (ホストテストで確かめられる)。
 *     ハードウェア乱数 (RDRAND) は entropy_hw.c が別に
 *     扱い、あればここに混ぜる。
 *   * 監査ログ: 各エントリが前のエントリのハッシュを含む
 *     ハッシュチェーン。改ざんすると検証で見つかる。
 *   * 許可リスト: 起動してよいアプリの一覧。
 *   * リモート操作: サーバの指示ファイルの解析と、設定の
 *     読み書き。
 *   * 監査の書き込み先 (ファイル) はカーネル側が決める:
 *     audit_log_event() は登録された sink が無ければ何も
 *     しない。ホストテストでは sink を付け替えて確かめる。
 */
#ifndef PRISM_SECURITY_H
#define PRISM_SECURITY_H

#include <stddef.h>
#include <stdint.h>

/* ---- エントロピープール ---- */

/* プールを初期化する。 */
void entropy_init(void);

/* 出来事を 1 つ混ぜる (キー入力・マウス・時刻など)。 */
void entropy_add(uint32_t value);

/* プールから n バイトの乱数を作る。out は n バイト以上あること。
 * 同じ初期状態と同じ出来事の列からは、同じ列が出る。 */
void entropy_bytes(uint8_t *out, size_t n);
void entropy_reseed(void);

/* ---- 監査ログ ---- */

/* 監査ログに入るエントリは最大で約 80 件 (1 行約 200 バイト、
 * 容量はファイル上限いっぱいの 16384 バイト)。1 行の形:
 *   seq=<n> time=<YYYY-MM-DD HH:MM:SS> event=<名前>
 *   detail=<内容> prev=<64桁16進> hash=<64桁16進>
 * hash = SHA-256(prev || "|" || seq || "|" || time || "|"
 *                || event || "|" || detail) の 16 進 64 桁。
 * 古いエントリは先頭から捨てる (容量のため)。
 * 捨てた分だけ先頭の prev は検証できないが、それ以降の
 * 連鎖は検証できる。
 * 保存時は他の利用者ファイルと同じく AES-256-CTR で暗号化
 * する (persist 側)。RAM 上 (VFS) では平文で扱い、暗号文の
 * まま読み込まれた間は追記を保留する (shell.c の sink)。 */
#define AUDIT_MAX_ENTRIES 80
#define AUDIT_LOG_CAP 16384

/* log (NUL 終端のテキスト、cap バイト) に 1 件追加する。
 * 成功したら 0、入らなければ -1。time は 19 文字
 * "YYYY-MM-DD HH:MM:SS"。event は 24 文字まで、detail は
 * 48 文字まで (超えた分は切る)。 */
int audit_append(char *log, size_t cap, const char *time19,
                 const char *event, const char *detail);

/* 連鎖を検証する。*out_count に件数、*out_bad_seq に最初に
 * 壊れていたエントリの seq (無ければ 0) を入れる。
 * 戻り値: 0 = 全部正しい、-1 = 壊れている、-2 = 形式が壊れている。 */
int audit_verify(const char *log, size_t len, int *out_count,
                 int *out_bad_seq);

/* エントリの件数を数える。 */
int audit_count(const char *log, size_t len);

/* 監査イベントの書き込み先。カーネル側 (shell.c) が登録する。
 * 未登録なら audit_log_event() は何もしない。 */
typedef void (*audit_sink_fn)(const char *event, const char *detail);
void audit_set_sink(audit_sink_fn sink);

/* 1 件記録する (sink 経由。未登録なら何もしない)。 */
void audit_log_event(const char *event, const char *detail);

/* ---- アプリ許可リスト ---- */

/* デスクトップのアプリ 16 個 (desktop.c の並びと同じ)。 */
#define SEC_APP_COUNT 24

/* アプリ名から番号 (0〜15)、無ければ -1。 */
int sec_app_id(const char *name);

/* 番号からアプリ名。範囲外なら ""。 */
const char *sec_app_name(int id);

/* 許可リストのテキストを allowed[16] に読む。
 *   "all" / 空 / 無い → 全部 1。
 *   "terminal,files,music" → 書かれたものだけ 1。
 * 知らない名前は無視する。戻り値は読めた行数 (0 でもよい)。 */
int allowlist_parse(const char *text, size_t len, int *allowed);

/* allowed[16] をテキストにする。"all" かカンマ区切り。
 * buf には NUL 込みで入る。書いたバイト数 (NUL 除く) を返す。 */
size_t allowlist_serialize(const int *allowed, char *buf, size_t cap);

/* そのアプリを起動してよいか。Terminal / Files / Settings は
 * 常に 1 (これらを止めると管理自体ができなくなるため)。 */
int sec_app_allowed(const int *allowed, int app_id);

/* ---- PIN ポリシー ---- */

/* PIN が規則に合うか。min_len (4〜16) 以上で、require_digit が
 * 1 なら数字を 1 つ以上含む。表示可能文字のみ。 */
int sec_pin_policy_ok(const char *pin, int min_len, int require_digit);

/* ---- リモート操作 ---- */

/* サーバの指示ファイルの形:
 *   action=lock | wipe | none
 *   token=<64桁16進>
 * action_out と token_out に取り出す。戻り値: 0 = 読めた、
 * -1 = action が無い。token が無ければ token_out は ""。 */
int remote_parse(const char *text, size_t len, char *action_out,
                 size_t action_cap, char *token_out, size_t token_cap);

/* リモート設定 "remote.conf" の中身:
 *   host=<ホスト> path=<パス> token=<64桁16進> enabled=on|off */
typedef struct {
    char host[64];
    char path[128];
    char token[65];
    int enabled;
} sec_remote_config;

void remote_config_defaults(sec_remote_config *cfg);

/* 設定をテキストにする。書いたバイト数 (NUL 除く) を返す。 */
size_t remote_config_serialize(const sec_remote_config *cfg, char *buf,
                               size_t cap);

/* テキストから設定を読む。知らない行は無視する。
 * 1 つでも知っているキーが読めたら 1、無ければ 0。 */
int remote_config_parse(sec_remote_config *cfg, const char *text,
                        size_t len);

#endif /* PRISM_SECURITY_H */
