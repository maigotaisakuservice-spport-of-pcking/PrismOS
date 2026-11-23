/* PrismOS — TOTP / HOTP (RFC 4226 / RFC 6238)
 *
 * 2 要素認証の時間ベースワンタイムパスワード。SHA-1 と
 * HMAC-SHA1 をここに持ち (カーネルには SHA-256 しか
 * 無かった)、6 桁 (または 8 桁) のコードを生成する。
 * 秘密は Base32 で保存する (認証アプリと同じ形式)。
 * 浮動小数点なし、整数演算のみ。ホストテストで RFC の
 * 公開ベクトルと照合する。
 */
#ifndef PRISM_TOTP_H
#define PRISM_TOTP_H

#include <stddef.h>
#include <stdint.h>

/* SHA-1 (FIPS 180-1)。 */
typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t buflen;
} prism_sha1;

void sha1_init(prism_sha1 *c);
void sha1_update(prism_sha1 *c, const uint8_t *data, size_t len);
void sha1_final(prism_sha1 *c, uint8_t out[20]);

/* HOTP: カウンタ値から digits 桁のコードを作る。 */
uint32_t hotp_code(const uint8_t *secret, size_t secret_len,
                   uint64_t counter, int digits);

/* TOTP: 時刻 (Unix 秒) と周期 (通常 30 秒) から。 */
uint32_t totp_code(const uint8_t *secret, size_t secret_len,
                   uint64_t unix_time, uint32_t period, int digits);

/* 入力コードが現在時刻 ±window ステップ以内で一致するか。
 * 1 = 一致、0 = 不一致。 */
int totp_verify(const uint8_t *secret, size_t secret_len,
                uint64_t unix_time, uint32_t code, int window);

/* Base32 (RFC 4648、パディングなしも可) の
 * エンコード/デコード。decode は小文字も受け付ける。
 * 戻り値: 書き込んだバイト数、失敗したら -1。 */
int base32_encode(const uint8_t *in, size_t len, char *out,
                  size_t cap);
int base32_decode(const char *in, uint8_t *out, size_t cap);

#endif /* PRISM_TOTP_H */
