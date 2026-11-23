/* PrismOS — 全ディスク暗号化 (FDE) の鍵ラップ部品。
 *
 * 設計の全体は docs/FDE_DESIGN.md。ここにあるのは、
 * 起動前パスフレーズでマスター鍵を包む/開く純粋関数と、
 * データ区画の先頭セクタに置くヘッダの形式だけである。
 * 実際の persist 経路への組み込み (起動前プロンプト、
 * ブロック層の暗号シム、移行処理) は設計文書の手順で
 * 別途行う — この部品単体では既存の保存経路は変わらない。
 *
 * 包み方: KEK = crypto_kdf(パスフレーズ, salt, 反復回数)
 * で作り、マスター鍵 (32 バイト) を AES-256-CTR で暗号化し、
 * HMAC-SHA-256 (KEK 鍵) の先頭 16 バイトで完全性を守る。
 * パスフレーズが違えば MAC が合わず開けないので、
 * .account ファイルを平文で置く必要がない (所在問題の
 * 回避がこの方式の要点)。
 */
#ifndef PRISM_FDE_H
#define PRISM_FDE_H

#include <stddef.h>
#include <stdint.h>

#define FDE_MASTER_KEY_LEN 32
#define FDE_SALT_LEN 16
#define FDE_DEFAULT_ITERATIONS 10000
/* KDF 反復回数の上限。ディスク上のヘッダは信頼できないので、
 * 破損・細工された巨大値で起動前ロック解除が実質ハング
 * しないよう、parse / unwrap の両方で弾く (監査 F4)。
 * 既定値 (10000) の 100 倍。 */
#define FDE_MAX_ITERATIONS 1000000

/* ヘッダ (1 セクタ = 512 バイトに収まる)。 */
typedef struct {
    uint8_t salt[FDE_SALT_LEN];
    uint32_t iterations;
    uint8_t iv[16];
    uint8_t wrapped[FDE_MASTER_KEY_LEN];
    uint8_t mac[16];
} fde_header;

/* マスター鍵をパスフレーズで包む。salt と iv は呼び出し側が
 * 乱数で用意する。成功したら 0。 */
int fde_wrap_key(const char *passphrase, const uint8_t *salt,
                 uint32_t iterations, const uint8_t *iv,
                 const uint8_t *master_key, fde_header *out);

/* 包みを開く。パスフレーズが違う・改ざんされていれば
 * -1 (master_key_out は書かない)。 */
int fde_unwrap_key(const char *passphrase, const fde_header *hdr,
                   uint8_t *master_key_out);

/* ヘッダを 512 バイトのセクタ画像にする (magic
 * "PRISMFK1" 付き)。成功したら 0。 */
int fde_header_serialize(const fde_header *hdr, uint8_t *sector);

/* セクタ画像からヘッダを読む。magic が無ければ -1。 */
int fde_header_parse(const uint8_t *sector, fde_header *out);

/* 1 セクタ (512 バイト) をマスター鍵で暗号化/復号する
 * (CTR モードなので同じ関数で両方向)。IV は保存せず、
 * IV = SHA-256(マスター鍵 || LBA (BE64) || 0 (BE32)) の
 * 先頭 16 バイトで決める (docs/FDE_DESIGN.md §3)。
 * in と out は同じバッファでもよい。 */
void fde_sector_crypt(const uint8_t *master_key, uint64_t lba,
                      const uint8_t *in, uint8_t *out);

#endif /* PRISM_FDE_H */
