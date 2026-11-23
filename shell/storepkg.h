/* PrismOS — Prism Store のパッケージ形式
 *
 * バイナリ形式 (すべてリトルエンディアン):
 *   0..7   マジック "PRISMPK1"
 *   8..11  ヘッダ長 H
 *   12..15 ペイロード長 P
 *   16..   ヘッダ (テキスト、H バイト):
 *            name=<アプリ名>
 *            file=<導入先 VFS 名>
 *            version=<版>
 *   16+H   署名 (ML-DSA-44、2420 バイト、ペイロード
 *          全体への署名)
 *   16+H+2420  ペイロード (互換 ELF など、P バイト)
 *
 * 署名が必須。検証は呼び出し側が store.pub (信頼の根)
 * で行う。ここは形式の解析と生成だけの純粋な層。
 */
#ifndef PRISM_STOREPKG_H
#define PRISM_STOREPKG_H

#include <stddef.h>
#include <stdint.h>

#define STOREPKG_MAGIC "PRISMPK1"
#define STOREPKG_SIG_LEN 2420
#define STOREPKG_NAME_MAX 24
#define STOREPKG_FILE_MAX 31

typedef struct {
    char name[STOREPKG_NAME_MAX + 1];
    char file[STOREPKG_FILE_MAX + 1];
    char version[16];
    size_t header_len;
    size_t payload_len;
} storepkg_info;

/* アプリ名・ファイル名が VFS に安全か (英数字と
 * ._- のみ、'/' と先頭 '.' は不可)。 */
int storepkg_name_ok(const char *name);
int storepkg_file_ok(const char *file);

/* パッケージを解析する。成功したら 0 を返し、info と
 * 署名・ペイロードへのポインタを埋める。 */
int storepkg_parse(const uint8_t *data, size_t len,
                   storepkg_info *info, const uint8_t **sig_out,
                   const uint8_t **payload_out);

/* パッケージを組み立てる。sig はペイロードへの
 * ML-DSA-44 署名。成功したら 0。 */
int storepkg_build(const char *name, const char *file,
                   const char *version, const uint8_t *payload,
                   size_t payload_len, const uint8_t *sig,
                   uint8_t *out, size_t cap, size_t *out_len);

/* store-installed.txt のテキスト処理:
 * 書式は 1 行 "name version file"。 */
int store_installed_contains(const char *text, size_t len,
                             const char *name);
int store_installed_add(const char *text, size_t len,
                        const char *name, const char *version,
                        const char *file, char *out, size_t cap);
int store_installed_remove(const char *text, size_t len,
                           const char *name, char *out, size_t cap,
                           char *file_out, size_t file_cap);

#endif /* PRISM_STOREPKG_H */
