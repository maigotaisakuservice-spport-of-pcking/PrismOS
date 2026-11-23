/* PrismOS — 文字コード変換 (深層チェック E-1)
 *
 * Shift_JIS / EUC-JP / ISO-2022-JP を UTF-8 へ変換
 * する。変換表は Python の codecs (JIS X 0208) から
 * 生成した shell/charset_tables.h。メール表示と
 * ファイル表示で使う。
 */
#ifndef PRISM_CHARSET_H
#define PRISM_CHARSET_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CHARSET_UTF8 = 0,
    CHARSET_SJIS = 1,
    CHARSET_EUCJP = 2,
    CHARSET_ISO2022JP = 3,
    CHARSET_ASCII = 4
} charset_kind;

/* バイト列の文字コードを推定する (ESC $ があれば
 * 2022、UTF-8 として妥当なら UTF-8、高バイトの
 * 並びで SJIS / EUC を見分ける)。 */
charset_kind charset_detect(const uint8_t *data, size_t len);

/* 名前 ("shift_jis", "euc-jp", "iso-2022-jp",
 * "utf-8" 等) から。未知なら CHARSET_UTF8。 */
charset_kind charset_from_name(const char *name);

/* UTF-8 へ変換する。out に cap バイトまで書き、
 * 書いたバイト数を返す (変換結果が収まらない場合は
 * 文字の途中で切らない)。変換元が UTF-8 / ASCII の
 * ときはそのままコピーする。 */
size_t charset_to_utf8(charset_kind kind, const uint8_t *in,
                       size_t in_len, char *out, size_t cap);

#endif /* PRISM_CHARSET_H */
