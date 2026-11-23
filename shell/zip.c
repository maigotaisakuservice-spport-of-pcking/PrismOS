/* PrismOS — 最小の ZIP 読み書き。 */
#include "zip.h"

#include <string.h>

#include "deflate.h"
#include "inflate.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

uint32_t zip_crc32(const uint8_t *data, size_t len) {
    static uint32_t table[256];
    static int table_ready = 0;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_ready = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void zip_write_begin(zip_writer *w, uint8_t *out, size_t cap) {
    w->out = out;
    w->cap = cap;
    w->len = 0;
    w->count = 0;
}

int zip_write_add(zip_writer *w, const char *name, const uint8_t *data,
                  size_t len) {
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > ZIP_MAX_NAME) {
        return -1;
    }
    if (w->count >= ZIP_MAX_ENTRIES) {
        return -1;
    }
    /* ローカルヘッダ 30 バイト + 名前 + 中身。 */
    if (w->len + 30 + name_len + len > w->cap) {
        return -1;
    }
    uint32_t crc = zip_crc32(data, len);
    uint8_t *p = w->out + w->len;
    uint32_t offset = (uint32_t)w->len;
    wr32(p + 0, 0x04034b50u); /* ローカルヘッダの署名 */
    wr16(p + 4, 20);          /* 作った版 (2.0) */
    wr16(p + 6, 0x0800);      /* フラグ: UTF-8 のファイル名 */
    wr16(p + 8, 0);           /* 手法: 格納 */
    wr16(p + 10, 0);          /* 時刻 (0 でよい) */
    wr16(p + 12, 0);          /* 日付 */
    wr32(p + 14, crc);
    wr32(p + 18, (uint32_t)len);
    wr32(p + 22, (uint32_t)len);
    wr16(p + 26, (uint16_t)name_len);
    wr16(p + 28, 0); /* 拡張フィールド無し */
    memcpy(p + 30, name, name_len);
    memcpy(p + 30 + name_len, data, len);
    w->len += 30 + name_len + len;
    zip_entry *e = &w->entries[w->count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, name_len);
    e->name[name_len] = '\0';
    e->crc = crc;
    e->comp_size = (uint32_t)len;
    e->uncomp_size = (uint32_t)len;
    e->method = 0;
    e->local_offset = offset;
    return 0;
}

int zip_write_add_deflated(zip_writer *w, const char *name,
                           const uint8_t *data, size_t len) {
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > ZIP_MAX_NAME) {
        return -1;
    }
    if (w->count >= ZIP_MAX_ENTRIES) {
        return -1;
    }
    /* 圧縮は静的な作業バッファで行う (カーネルのスタックを
     * 圧迫しないため)。 */
    static uint8_t comp[16384];
    int cn = -1;
    if (len <= sizeof(comp)) {
        cn = deflate_compress_fixed(data, len, comp, sizeof(comp));
    }
    if (cn < 0 || (size_t)cn >= len) {
        /* 縮まなければ格納で追加する。 */
        return zip_write_add(w, name, data, len);
    }
    if (w->len + 30 + name_len + (size_t)cn > w->cap) {
        return -1;
    }
    uint32_t crc = zip_crc32(data, len);
    uint8_t *p = w->out + w->len;
    uint32_t offset = (uint32_t)w->len;
    wr32(p + 0, 0x04034b50u);
    wr16(p + 4, 20);
    wr16(p + 6, 0x0800);
    wr16(p + 8, 8); /* 手法: DEFLATE */
    wr16(p + 10, 0);
    wr16(p + 12, 0);
    wr32(p + 14, crc);
    wr32(p + 18, (uint32_t)cn);
    wr32(p + 22, (uint32_t)len);
    wr16(p + 26, (uint16_t)name_len);
    wr16(p + 28, 0);
    memcpy(p + 30, name, name_len);
    memcpy(p + 30 + name_len, comp, (size_t)cn);
    w->len += 30 + name_len + (size_t)cn;
    zip_entry *e = &w->entries[w->count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, name_len);
    e->name[name_len] = '\0';
    e->crc = crc;
    e->comp_size = (uint32_t)cn;
    e->uncomp_size = (uint32_t)len;
    e->method = 8;
    e->local_offset = offset;
    return 0;
}

int zip_write_finish(zip_writer *w) {
    /* 中央ディレクトリ (46 バイト + 名前) × 件数 + 終了レコード
     * 22 バイト。 */
    size_t need = 22;
    for (int i = 0; i < w->count; i++) {
        need += 46 + strlen(w->entries[i].name);
    }
    if (w->len + need > w->cap) {
        return -1;
    }
    uint32_t cd_offset = (uint32_t)w->len;
    for (int i = 0; i < w->count; i++) {
        zip_entry *e = &w->entries[i];
        size_t name_len = strlen(e->name);
        uint8_t *p = w->out + w->len;
        wr32(p + 0, 0x02014b50u); /* 中央ディレクトリの署名 */
        wr16(p + 4, 20);          /* 作った版 */
        wr16(p + 6, 20);          /* 展開に要る版 */
        wr16(p + 8, 0x0800);      /* フラグ: UTF-8 */
        wr16(p + 10, e->method);
        wr16(p + 12, 0); /* 時刻 */
        wr16(p + 14, 0); /* 日付 */
        wr32(p + 16, e->crc);
        wr32(p + 20, e->comp_size);
        wr32(p + 24, e->uncomp_size);
        wr16(p + 28, (uint16_t)name_len);
        wr16(p + 30, 0); /* 拡張フィールド */
        wr16(p + 32, 0); /* コメント */
        wr16(p + 34, 0); /* ディスク番号 */
        wr16(p + 36, 0); /* 内部属性 */
        wr32(p + 38, 0); /* 外部属性 */
        wr32(p + 42, e->local_offset);
        memcpy(p + 46, e->name, name_len);
        w->len += 46 + name_len;
    }
    uint8_t *p = w->out + w->len;
    wr32(p + 0, 0x06054b50u); /* 終了レコードの署名 */
    wr16(p + 4, 0);           /* このディスク */
    wr16(p + 6, 0);           /* 中央ディレクトリのあるディスク */
    wr16(p + 8, (uint16_t)w->count);
    wr16(p + 10, (uint16_t)w->count);
    wr32(p + 12, (uint32_t)(w->len - cd_offset));
    wr32(p + 16, cd_offset);
    wr16(p + 20, 0); /* コメント無し */
    w->len += 22;
    return (int)w->len;
}

int zip_read_entries(const uint8_t *data, size_t len, zip_entry *entries,
                     int max) {
    if (data == NULL || len < 22 || entries == NULL || max <= 0) {
        return -1;
    }
    /* 終了レコードを後ろから探す (コメントは最大 64KB だが、
     * 本物の .docx/.pptx はコメント無し。1024 バイトだけ
     * さかのぼって探す)。 */
    size_t start = len > 1024 + 22 ? len - 1024 - 22 : 0;
    size_t eocd = 0;
    int found = 0;
    size_t i = len - 22;
    for (;;) {
        if (rd32(data + i) == 0x06054b50u) {
            eocd = i;
            found = 1;
            break;
        }
        if (i == start) {
            break;
        }
        i--;
    }
    if (!found) {
        return -1;
    }
    uint16_t count = rd16(data + eocd + 10);
    uint32_t cd_size = rd32(data + eocd + 12);
    uint32_t cd_offset = rd32(data + eocd + 16);
    /* size_t で計算する: 32 ビット環境で uint32 のまま足すと
     * 巻き戻って境界チェックをすり抜ける。 */
    if ((size_t)cd_offset + (size_t)cd_size > len) {
        return -1;
    }
    size_t pos = cd_offset;
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        if (pos + 46 > len || rd32(data + pos) != 0x02014b50u) {
            return -1;
        }
        uint16_t name_len = rd16(data + pos + 28);
        uint16_t extra_len = rd16(data + pos + 30);
        uint16_t comment_len = rd16(data + pos + 32);
        if (name_len > ZIP_MAX_NAME ||
            pos + 46 + name_len + extra_len + comment_len > len) {
            return -1;
        }
        zip_entry *e = &entries[n++];
        memset(e, 0, sizeof(*e));
        memcpy(e->name, data + pos + 46, name_len);
        e->name[name_len] = '\0';
        e->method = rd16(data + pos + 10);
        e->crc = rd32(data + pos + 16);
        e->comp_size = rd32(data + pos + 20);
        e->uncomp_size = rd32(data + pos + 24);
        e->local_offset = rd32(data + pos + 42);
        pos += 46 + name_len + extra_len + comment_len;
    }
    return n;
}

const zip_entry *zip_find_entry(const zip_entry *entries, int count,
                                const char *name) {
    if (entries == NULL || name == NULL) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

int zip_read_entry(const uint8_t *data, size_t len,
                   const zip_entry *entry, uint8_t *out, size_t cap) {
    if (data == NULL || entry == NULL || out == NULL) {
        return -1;
    }
    uint32_t off = entry->local_offset;
    /* (size_t)off + 30: 32 ビット環境で off + 30 が巻き戻ると
     * 大きな local_offset がチェックを通ってしまう。 */
    if ((size_t)off + 30 > len || rd32(data + off) != 0x04034b50u) {
        return -1;
    }
    uint16_t name_len = rd16(data + off + 26);
    uint16_t extra_len = rd16(data + off + 28);
    size_t data_off = (size_t)off + 30 + name_len + extra_len;
    if (data_off + entry->comp_size > len) {
        return -1;
    }
    if (entry->uncomp_size > cap) {
        return -1;
    }
    if (entry->method == 0) {
        memcpy(out, data + data_off, entry->comp_size);
        if (zip_crc32(out, entry->comp_size) != entry->crc) {
            return -1;
        }
        return (int)entry->comp_size;
    }
    if (entry->method == 8) {
        int n = inflate_decompress(data + data_off, entry->comp_size, out,
                                   cap);
        if (n < 0 || (uint32_t)n != entry->uncomp_size) {
            return -1;
        }
        if (zip_crc32(out, (size_t)n) != entry->crc) {
            return -1;
        }
        return n;
    }
    return -1; /* 対応していない圧縮手法 */
}
