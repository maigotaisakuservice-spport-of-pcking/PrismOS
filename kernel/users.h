/* PrismOS — 複数ユーザー (users.txt)
 *
 * 主アカウント (.account、名前は "admin") に加えて、
 * 追加ユーザーを "users.txt" に 1 行 1 レコードで持つ:
 *
 *   user <名前> <salt 32桁hex> <反復回数> <hash 64桁hex> <役割>
 *   wrap <名前> <包んだ主鍵 64桁hex> <検査値 16桁hex>
 *   totp <名前> <Base32 秘密>
 *
 * PIN 検証は .account と同じ v2 形式 (ソルト付き反復
 * ハッシュ) を組み立てて account_verify に委ねる。
 * ファイル暗号の主鍵 (MK) は管理者 PIN から導いた鍵
 * そのものなので、追加ユーザーは自分の PIN から導いた
 * 鍵で MK を包んだ "wrap" を持ち、ログイン時に開いて
 * persist_set_key(MK) する。包みは AES-256-CTR
 * (IV 0、鍵はユーザーごとに一意) + SHA-256 検査値。
 * 役割は "admin" または "user"。
 * ここはテキスト処理だけの純粋な層で、ホストテストする。
 */
#ifndef PRISM_USERS_H
#define PRISM_USERS_H

#include <stddef.h>
#include <stdint.h>

#define PRISM_USERS_FILE "users.txt"
#define PRISM_USER_NAME_MAX 16
#define PRISM_USERS_TEXT_MAX 4096

/* 名前が規則に合うか (1-16 文字、英数字と _-、
 * "guest" と "admin" は予約)。 */
int users_name_ok(const char *name);

/* user レコードの数。 */
int users_count(const char *text, size_t len);

/* その名前の user レコードがあるか。 */
int users_exists(const char *text, size_t len, const char *name);

/* PIN 検証。1 = 合う、0 = 合わない/無い、-1 = 壊れている。 */
int users_verify(const char *text, size_t len, const char *name,
                 const char *pin);

/* user レコード行を作る (改行なし、NUL 終端)。
 * salt は 16 バイト。成功 0、失敗 -1。 */
int users_make_line(const char *name, const uint8_t *salt,
                    uint32_t iterations, const char *pin,
                    const char *role, char *out, size_t cap);

/* ユーザーのソルトと反復回数を取り出す。salt_out は
 * 16 バイト。無ければ -1。 */
int users_salt_iter(const char *text, size_t len, const char *name,
                    uint8_t *salt_out, uint32_t *iter_out);

/* ユーザーの役割を out に書く。無ければ -1。 */
int users_role(const char *text, size_t len, const char *name,
               char *out, size_t cap);

/* wrap を取り出す。wrap_out は 32 バイト、check_out は
 * 8 バイト。無ければ -1。 */
int users_get_wrap(const char *text, size_t len, const char *name,
                   uint8_t *wrap_out, uint8_t *check_out);

/* wrap 行を作る。mk は主鍵 32 バイト、user_key は
 * ユーザー PIN から導いた鍵 32 バイト。 */
int users_make_wrap_line(const char *name, const uint8_t *mk,
                         const uint8_t *user_key, char *out,
                         size_t cap);

/* wrap を開く。検査値が合えば 0、合わなければ -1。
 * 検査値は第 1 形式 (利用者の鍵+主鍵) と、再ラップで
 * 付け直される第 2 形式 (主鍵のみ) の両方を受ける。 */
int users_unwrap(const uint8_t *wrap, const uint8_t *check,
                 const uint8_t *user_key, uint8_t *mk_out);

/* プライマリ PIN 変更時: 全 wrap 行を旧主鍵から新主鍵へ
 * 包み直した users テキストを out に作る。包んだ件数を
 * count_out に返す (NULL 可)。成功 0、失敗 -1。 */
int users_rewrap_all(const char *text, size_t len,
                     const uint8_t *old_mk, const uint8_t *new_mk,
                     char *out, size_t cap, int *count_out);

/* 同じ種類+名前の行を置き換えるか、無ければ末尾に足す。
 * text 全体を out に書き直す。成功 0、容量不足 -1。 */
int users_upsert(const char *text, size_t len, const char *line,
                 char *out, size_t cap);

/* その名前の user/wrap/totp 行をすべて消す。 */
int users_remove_user(const char *text, size_t len,
                      const char *name, char *out, size_t cap);

/* totp 秘密 (Base32) を out に書く。無ければ -1。 */
int users_totp(const char *text, size_t len, const char *name,
               char *out, size_t cap);

#endif /* PRISM_USERS_H */
