/* PrismOS — 更新システムの純粋ロジック
 *
 * 更新サーバが配るマニフェスト (平文のテキスト) の解析と、
 * バージョンの比較、SHA-256 の 16 進表記だけを扱う。
 * ハードウェアにもネットワークにも触らないので、カーネルと
 * ホストテストで同じソースを使う。
 *
 * マニフェストの形式 (1 行 1 項目):
 *   version=0.3.0          新しいバージョン (必須)
 *   channel=stable         安定版 / beta
 *   type=security          security (自動で適用してよい) /
 *                          feature (手動の確認が要る)
 *   base=/updates          ファイルの取得先の接頭辞
 *   file README.txt <sha256 64 桁>
 *   file VERSION <sha256 64 桁>
 *   kernel <sha256 64 桁>   新しいカーネルイメージのハッシュ
 *                          (ある場合だけ。カーネル更新の対象)
 * 知らない行は無視する。
 */
#ifndef PRISM_UPDATE_H
#define PRISM_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#define UPDATE_MAX_FILES 32
#define UPDATE_NAME_MAX 31

typedef struct {
    char name[UPDATE_NAME_MAX + 1];
    uint8_t sha256[32];
} update_file;

typedef struct {
    char version[16];
    char channel[16]; /* "stable" / "beta" */
    char type[16];    /* "security" / "feature" */
    char base[64];    /* ファイル取得先の接頭辞 (例 "/updates") */
    update_file files[UPDATE_MAX_FILES];
    int file_count;
    /* カーネル更新: ある場合だけ 1。カーネルイメージは
     * <base>/kernel.elf から取り、SHA-256 が合うものだけ
     * 起動パーティションに書き込む。 */
    int has_kernel;
    uint8_t kernel_sha256[32];
} update_manifest;

/* マニフェストを解析する。version が無ければ -1、
 * 壊れた file 行があれば -2、成功したら 0。 */
int update_parse_manifest(const char *text, size_t len,
                          update_manifest *out);

/* バージョンを比較する。a < b なら負、等しければ 0、
 * a > b なら正。"0.10.0" は "0.9.0" より新しい。
 * "-beta" のような接尾辞があるものは、数字が同じなら
 * 接尾辞無しより古い。 */
int update_version_cmp(const char *a, const char *b);

/* 64 桁の 16 進文字列を 32 バイトに変える。成功したら 0。 */
int update_hex_to_bytes(const char *hex, uint8_t *out32);

/* 32 バイトを 64 桁の 16 進文字列 (NUL 終端) にする。
 * hex_out は 65 バイト以上あること。 */
void update_bytes_to_hex(const uint8_t *bytes, char *hex_out);

/* data の SHA-256 を 64 桁の 16 進文字列 (NUL 終端) にする。
 * hex_out は 65 バイト以上あること。 */
void update_sha256_hex(const uint8_t *data, size_t len, char *hex_out);

/* マニフェストの ML-DSA-44 署名を検証する。署名は 2420 バイト。
 * 0 = 署名が正しい。0 以外 = 署名がない・長さが違う・正しくない。
 * 署名のないマニフェストは受け付けない。 */
int update_verify_manifest_sig(const uint8_t *manifest, size_t manifest_len,
                               const uint8_t *sig, size_t sig_len);

#endif /* PRISM_UPDATE_H */
