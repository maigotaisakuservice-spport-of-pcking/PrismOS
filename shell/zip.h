/* PrismOS — 最小の ZIP 読み書き (OOXML/.docx/.pptx 用)。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。
 *
 * 書き込みは格納と DEFLATE (shell/deflate.h) の両方に対応
 * する。読み込みは格納と DEFLATE (shell/inflate.h) の両方に
 * 対応する。ZIP64 と暗号化とマルチディスクには対応しない
 * (本物の .docx/.pptx は普通どれも使わない)。
 */
#ifndef PRISM_ZIP_H
#define PRISM_ZIP_H

#include <stddef.h>
#include <stdint.h>

/* 書き込み・読み込み共通の上限。本物の .pptx は 30 件を
 * 超えることがある (テーマやレイアウトが多いため)。
 * zip_writer は大きいので、カーネルでは静的に置くこと。 */
#define ZIP_MAX_ENTRIES 64
#define ZIP_MAX_NAME 63

typedef struct {
    char name[ZIP_MAX_NAME + 1];
    uint32_t crc;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t method; /* 0 = 格納、8 = DEFLATE */
    uint32_t local_offset;
} zip_entry;

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t len;
    zip_entry entries[ZIP_MAX_ENTRIES];
    int count;
} zip_writer;

/* 書き出しの準備。out は呼び出し側のバッファ。 */
void zip_write_begin(zip_writer *w, uint8_t *out, size_t cap);

/* 1 ファイルを格納で追加する。成功したら 0、失敗したら -1
 * (名前が長い・件数が上限・出力先が足りない)。 */
int zip_write_add(zip_writer *w, const char *name, const uint8_t *data,
                  size_t len);

/* 1 ファイルを DEFLATE で圧縮して追加する。圧縮しても
 * 縮まなければ格納で追加する。成功したら 0、失敗したら -1。 */
int zip_write_add_deflated(zip_writer *w, const char *name,
                           const uint8_t *data, size_t len);

/* 中央ディレクトリと終了レコードを書く。成功したら全体の
 * バイト数、失敗したら -1。 */
int zip_write_finish(zip_writer *w);

/* data から ZIP を読む。entries に最大 max 件まで入れる。
 * 戻り値は見つかった件数、失敗したら -1。 */
int zip_read_entries(const uint8_t *data, size_t len, zip_entry *entries,
                     int max);

/* 名前で項目を探す。見つかればその項目、無ければ NULL。 */
const zip_entry *zip_find_entry(const zip_entry *entries, int count,
                                const char *name);

/* 1 項目の中身を out (cap バイト) に読む。格納はコピー、
 * DEFLATE は展開する。戻り値は読んだバイト数、失敗は -1。 */
int zip_read_entry(const uint8_t *data, size_t len,
                   const zip_entry *entry, uint8_t *out, size_t cap);

/* CRC-32 (IEEE 802.3、ZIP と同じ多項式)。 */
uint32_t zip_crc32(const uint8_t *data, size_t len);

#endif /* PRISM_ZIP_H */
