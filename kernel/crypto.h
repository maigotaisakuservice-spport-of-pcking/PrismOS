/* PrismOS — 暗号 (SHA-256 と AES-256-CTR)
 *
 * ディスク暗号化のための純粋関数。ハードウェアに触らないので、
 * カーネルとホストテストで同じソースを使う。
 *
 *   * 鍵: SHA-256(PIN のバイト列 || ソルト 8 バイト) の 32 バイト。
 *     PIN はログイン時にだけメモリに置き、ディスクには書かない。
 *   * ファイルの中身: AES-256-CTR で暗号化する。長さは変わらない。
 *     初期ベクトル (IV) はファイルごとに 16 バイトで、ディスク上の
 *     ファイル表に置く。CTR のカウンタは 128 ビットをビッグエンディ
 *     アンで 1 ずつ増やす (NIST SP 800-38A の流儀)。
 *
 * 限界 (正直に書く): IV の乱数はハードウェア乱数ではなく、時計と
 * カウンタとソルトを混ぜたもの。PIN 自体の強さは FNV-1a ハッシュ
 * の限界と同じく、総当たりに弱い。鍵は起動中のメモリにだけ置く。
 */
#ifndef PRISM_CRYPTO_H
#define PRISM_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* data の SHA-256 を out (32 バイト) に入れる。 */
void crypto_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* SHA-256 のストリーミング計算 (大きな入力を少しずつ渡す用)。
 * 使い方は init → update (何回でも) → final (1 回)。
 * final の後はその ctx を再利用しないこと。crypto_sha256() と
 * 同じ値を出す。カーネル更新で、HTTP から届くチャンクを
 * 書き込みながらハッシュするために使う。 */
typedef struct {
    uint32_t h[8];
    uint8_t block[64];
    size_t block_len; /* block に溜めたバイト数 (0..63) */
    uint64_t total_len; /* これまでに渡した総バイト数 */
} crypto_sha256_ctx;

void crypto_sha256_init(crypto_sha256_ctx *ctx);
void crypto_sha256_update(crypto_sha256_ctx *ctx, const uint8_t *data,
                          size_t len);
void crypto_sha256_final(crypto_sha256_ctx *ctx, uint8_t out[32]);

/* PIN とソルトから 32 バイトの鍵を作る。
 * key_out は 32 バイト以上あること。 */
void crypto_derive_key(const char *pin, const uint8_t *salt,
                       uint8_t key_out[32]);

/* PIN とソルトから反復ハッシュで 32 バイトの鍵を作る。
 * 1 回目: SHA-256(salt || PIN)。2 回目以降は
 * SHA-256(前回の出力 || salt || PIN) を繰り返す。
 * 反復回数 iterations が 0 のときは 1 回として扱う。
 * 総当たりを遅くするための鍵導出関数 (KDF)。
 * key_out は 32 バイト以上あること。 */
void crypto_kdf(const char *pin, const uint8_t *salt, size_t salt_len,
                uint32_t iterations, uint8_t key_out[32]);

/* AES-256-CTR で暗号化する (復号も同じ関数: 暗号化と復号が同じ)。
 * key は 32 バイト、iv は 16 バイト。in と out は別でも同じでもよい。
 * len が 0 なら何もしない。 */
void crypto_aes256_ctr(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *in, uint8_t *out, size_t len);

#endif /* PRISM_CRYPTO_H */
