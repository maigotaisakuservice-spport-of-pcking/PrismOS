/* PrismOS — ローカルアカウント (PIN / パスワード)
 *
 * 仕組み (v2、フェーズ14):
 *   * PIN は平文では保存しない。16 バイトのソルト付きで
 *     反復ハッシュ (crypto_kdf、既定 10000 回) を取り、
 *     ".account" ファイルに
 *     "v2\nsalt=<32桁16進>\niter=<回数>\nhash=<64桁16進>\n"
 *     として置く。
 *   * v1 (FNV-1a 64 ビット) のファイルも読める。
 *     ログインに成功したら v2 に自動で移行する。
 *     FNV-1a は暗号学的ハッシュではないため、新規作成は
 *     すべて v2 にする。
 *   * ソルトは呼び出し側が渡す。カーネルではエントロピー
 *     プールから作る (RDRAND があれば混ぜる)。
 *   * PIN の規則: 4〜16 文字、空白を除く表示可能文字。
 *     管理者が設定で最小長と数字必須を強くできる
 *     (sec_pin_policy_ok を使う)。
 */
#ifndef PRISM_ACCOUNT_H
#define PRISM_ACCOUNT_H

#include <stddef.h>
#include <stdint.h>

#define PRISM_ACCOUNT_FILE ".account"
#define PRISM_ACCOUNT_TEXT_MAX 160
#define PRISM_ACCOUNT_SALT_LEN 16
#define PRISM_ACCOUNT_ITERATIONS 10000

/* PIN が規則に合うかどうか。1 = 合う、0 = 合わない。 */
int account_pin_ok(const char *pin);

/* PIN がポリシーに合うかどうか。min_len (4〜16) 以上で、
 * require_digit が 1 なら数字を 1 つ以上含む。 */
int account_pin_ok_policy(const char *pin, int min_len,
                          int require_digit);

/* 64 ビット FNV-1a ハッシュ (ソルト 8 バイト + PIN)。
 * v1 の検証用に残す。新規作成には使わない。 */
uint64_t account_hash(const uint8_t *salt, const char *pin);

/* ".account" の中身 (v2) を作る。salt は 16 バイト、
 * buf は 160 バイト以上あること。
 * 成功したら 0、PIN が不正なら -1。 */
int account_make_text(const uint8_t *salt, uint32_t iterations,
                      const char *pin, char *buf, size_t cap);

/* ".account" の形式番号。1 = v1、2 = v2、-1 = 壊れている。 */
int account_version(const char *text, size_t len);

/* ".account" の中身と PIN が合うかどうか。
 * 1 = 合う、0 = 合わない、-1 = 中身が壊れている。
 * v1 と v2 の両方に対応する。 */
int account_verify(const char *pin, const char *text, size_t len);

/* ".account" の中身からソルトと反復回数を取り出す。
 * salt_out は 16 バイト以上あること。v1 なら先頭 8 バイト
 * だけ埋めて *iter_out に 0 を入れる。v2 なら 16 バイトと
 * 記録された回数を入れる。成功したら 0、壊れていたら -1。 */
int account_get_salt(const char *text, size_t len, uint8_t *salt_out,
                     uint32_t *iter_out);

#endif /* PRISM_ACCOUNT_H */
