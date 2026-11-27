/* PrismOS — FAT16 の読み書き (起動パーティション用)。fat16.h を見る。 */
#include "fat16.h"

#include "string.h"

#define FAT16_EOC 0xFFFFu
#define FAT16_SECTOR 512
#define FAT16_ENTRY_SIZE 32
#define FAT16_ENTRIES_PER_SECTOR 16
#define FAT16_ATTR_LFN 0x0F
#define FAT16_ATTR_DIR 0x10

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int read_sec(const fat16_fs *fs, uint32_t lba, uint8_t *buf) {
    return fs->dev.read_sector(fs->dev.ctx, lba, buf);
}

static int write_sec(const fat16_fs *fs, uint32_t lba, const uint8_t *buf) {
    return fs->dev.write_sector(fs->dev.ctx, lba, buf);
}

static uint32_t cluster_lba(const fat16_fs *fs, uint32_t cluster) {
    return fs->data_start +
           (cluster - 2) * (uint32_t)fs->sectors_per_cluster;
}

/* ---- FAT の読み書き (1 セクタ分のキャッシュ付き) ---- */

/* 汚れた FAT キャッシュを全コピーに書き出す。汚れていなければ
 * 何もしない。失敗したらキャッシュを捨てる (デバイスと食い違う
 * 内容を残さないため)。 */
static int fat_flush(fat16_fs *fs) {
    if (!fs->fat_cache_valid || !fs->fat_cache_dirty) {
        return 0;
    }
    for (uint8_t i = 0; i < fs->num_fats; i++) {
        uint32_t lba = fs->fat_start + (uint32_t)i * fs->fat_sectors +
                       fs->fat_cache_sector;
        if (write_sec(fs, lba, fs->fat_cache) != 0) {
            fs->fat_cache_valid = 0;
            fs->fat_cache_dirty = 0;
            return -1;
        }
    }
    fs->fat_cache_dirty = 0;
    return 0;
}

/* FAT の sector 番目のセクタをキャッシュに載せる。汚れた古い
 * セクタがあれば先に書き出す。 */
static int fat_load(fat16_fs *fs, uint32_t sector) {
    if (fs->fat_cache_valid && fs->fat_cache_sector == sector) {
        return 0;
    }
    if (fat_flush(fs) != 0) {
        return -1;
    }
    if (read_sec(fs, fs->fat_start + sector, fs->fat_cache) != 0) {
        fs->fat_cache_valid = 0;
        return -1;
    }
    fs->fat_cache_sector = sector;
    fs->fat_cache_valid = 1;
    fs->fat_cache_dirty = 0;
    return 0;
}

static int fat_get(fat16_fs *fs, uint32_t cluster, uint16_t *out) {
    if (cluster < 2 || cluster >= fs->cluster_count + 2) {
        return -1;
    }
    uint32_t off = cluster * 2;
    uint32_t sector = off / FAT16_SECTOR;
    if (fat_load(fs, sector) != 0) {
        return -1;
    }
    *out = rd16(fs->fat_cache + (off % FAT16_SECTOR));
    return 0;
}

static int fat_set(fat16_fs *fs, uint32_t cluster, uint16_t value) {
    if (cluster < 2 || cluster >= fs->cluster_count + 2) {
        return -1;
    }
    uint32_t off = cluster * 2;
    uint32_t sector = off / FAT16_SECTOR;
    if (fat_load(fs, sector) != 0) {
        return -1;
    }
    wr16(fs->fat_cache + (off % FAT16_SECTOR), value);
    /* デバイスへの書き出しは fat_flush() に任せる。連続した
     * 割り当てでは同じ FAT セクタを何度も触るため、毎回書くと
     * 遅い (1 セクタのファイルでも 2 回の FAT 書き込みになる)。 */
    fs->fat_cache_dirty = 1;
    return 0;
}

/* 空きクラスタを 1 個取り、EOC (0xFFFF) にする。 */
static int fat_alloc(fat16_fs *fs, uint32_t *out) {
    uint32_t start = fs->alloc_hint;
    if (start < 2) {
        start = 2;
    }
    for (uint32_t tried = 0; tried < fs->cluster_count; tried++) {
        uint32_t c =
            2 + ((start - 2 + tried) % fs->cluster_count);
        uint16_t v = 0;
        if (fat_get(fs, c, &v) != 0) {
            return -1;
        }
        if (v == 0) {
            if (fat_set(fs, c, FAT16_EOC) != 0) {
                return -1;
            }
            fs->alloc_hint = c + 1;
            *out = c;
            return 0;
        }
    }
    return -1; /* 空き無し */
}

/* 鎖をたどって全部解放する。壊れた鎖でも止まる。 */
static void fat_free_chain(fat16_fs *fs, uint32_t first) {
    uint32_t c = first;
    uint32_t guard = 0;
    while (c >= 2 && c < fs->cluster_count + 2 &&
           guard++ <= fs->cluster_count) {
        uint16_t next = FAT16_EOC;
        if (fat_get(fs, c, &next) != 0) {
            return;
        }
        (void)fat_set(fs, c, 0);
        if (next >= 0xFFF8u) {
            return;
        }
        c = next;
    }
}

/* ---- 8.3 名前 ---- */

static int to_83(const char *comp, size_t len, uint8_t out11[11]) {
    if (len == 0 || len > 12) {
        return -1;
    }
    /* 最初の '.' で本体と拡張子に分ける。 */
    size_t dot = len;
    for (size_t i = 0; i < len; i++) {
        if (comp[i] == '.') {
            dot = i;
            break;
        }
    }
    size_t base_len = dot;
    size_t ext_len = (dot < len) ? (len - dot - 1) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3) {
        return -1;
    }
    for (int i = 0; i < 11; i++) {
        out11[i] = ' ';
    }
    for (size_t i = 0; i < base_len; i++) {
        char ch = comp[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '$' || ch == '%' || ch == '\'' || ch == '-' ||
              ch == '_' || ch == '@' || ch == '~' || ch == '`' ||
              ch == '!' || ch == '(' || ch == ')' || ch == '{' ||
              ch == '}' || ch == '^' || ch == '#' || ch == '&')) {
            return -1;
        }
        out11[i] = (uint8_t)ch;
    }
    for (size_t i = 0; i < ext_len; i++) {
        char ch = comp[dot + 1 + i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '$' || ch == '%' || ch == '\'' || ch == '-' ||
              ch == '_' || ch == '@' || ch == '~' || ch == '`' ||
              ch == '!' || ch == '(' || ch == ')' || ch == '{' ||
              ch == '}' || ch == '^' || ch == '#' || ch == '&')) {
            return -1;
        }
        out11[8 + i] = (uint8_t)ch;
    }
    return 0;
}

/* ---- ディレクトリ ---- */

typedef struct {
    uint32_t dir_sector;
    uint16_t dir_index;
    uint16_t first_cluster;
    uint32_t size;
    uint8_t attr;
} fat16_loc;

typedef struct {
    int is_root;
    uint32_t first_cluster; /* ルートのときは使わない */
} fat16_dir;

/* 1 セクタ分を走査する。1 = 見つかった (loc を埋める)、
 * -1 = 0x00 で終わり (この先は無い)、0 = このセクタには無い。 */
static int scan_sector(const uint8_t *sec, uint32_t sector,
                       const uint8_t name11[11], fat16_loc *loc) {
    for (int i = 0; i < FAT16_ENTRIES_PER_SECTOR; i++) {
        const uint8_t *e = sec + i * FAT16_ENTRY_SIZE;
        if (e[0] == 0x00) {
            return -1;
        }
        if (e[0] == 0xE5) {
            continue;
        }
        if (e[11] == FAT16_ATTR_LFN) {
            continue; /* 長い名前の入口は飛ばす */
        }
        if (memcmp(e, name11, 11) == 0) {
            loc->dir_sector = sector;
            loc->dir_index = (uint16_t)i;
            loc->first_cluster = rd16(e + 26);
            loc->size = rd32(e + 28);
            loc->attr = e[11];
            return 1;
        }
    }
    return 0;
}

static int dir_find(fat16_fs *fs, const fat16_dir *dir,
                    const uint8_t name11[11], fat16_loc *loc) {
    uint8_t sec[FAT16_SECTOR];
    if (dir->is_root) {
        uint32_t root_sectors =
            ((uint32_t)fs->root_entries * FAT16_ENTRY_SIZE +
             FAT16_SECTOR - 1) /
            FAT16_SECTOR;
        for (uint32_t s = 0; s < root_sectors; s++) {
            uint32_t lba = fs->root_start + s;
            if (read_sec(fs, lba, sec) != 0) {
                return -2;
            }
            int r = scan_sector(sec, lba, name11, loc);
            if (r != 0) {
                return r == 1 ? 0 : -1;
            }
        }
        return -1;
    }
    uint32_t c = dir->first_cluster;
    uint32_t guard = 0;
    while (c >= 2 && c < fs->cluster_count + 2 &&
           guard++ <= fs->cluster_count) {
        uint32_t base = cluster_lba(fs, c);
        for (uint8_t k = 0; k < fs->sectors_per_cluster; k++) {
            uint32_t lba = base + k;
            if (read_sec(fs, lba, sec) != 0) {
                return -2;
            }
            int r = scan_sector(sec, lba, name11, loc);
            if (r != 0) {
                return r == 1 ? 0 : -1;
            }
        }
        uint16_t next = FAT16_EOC;
        if (fat_get(fs, c, &next) != 0) {
            return -2;
        }
        if (next >= 0xFFF8u) {
            break;
        }
        c = next;
    }
    return -1;
}

/* 空きスロットを探す。無ければ、子ディレクトリならクラスタを
 * 1 個足して広げる (ルートは広げられない)。 */
static int dir_find_free(fat16_fs *fs, const fat16_dir *dir,
                         fat16_loc *loc) {
    uint8_t sec[FAT16_SECTOR];
    if (dir->is_root) {
        uint32_t root_sectors =
            ((uint32_t)fs->root_entries * FAT16_ENTRY_SIZE +
             FAT16_SECTOR - 1) /
            FAT16_SECTOR;
        for (uint32_t s = 0; s < root_sectors; s++) {
            uint32_t lba = fs->root_start + s;
            if (read_sec(fs, lba, sec) != 0) {
                return -1;
            }
            for (int i = 0; i < FAT16_ENTRIES_PER_SECTOR; i++) {
                const uint8_t *e = sec + i * FAT16_ENTRY_SIZE;
                if (e[0] == 0x00 || e[0] == 0xE5) {
                    loc->dir_sector = lba;
                    loc->dir_index = (uint16_t)i;
                    loc->first_cluster = 0;
                    loc->size = 0;
                    loc->attr = 0;
                    return 0;
                }
            }
        }
        return -1; /* ルートがいっぱい */
    }
    uint32_t c = dir->first_cluster;
    uint32_t last = c;
    uint32_t guard = 0;
    while (c >= 2 && c < fs->cluster_count + 2 &&
           guard++ <= fs->cluster_count) {
        last = c;
        uint32_t base = cluster_lba(fs, c);
        for (uint8_t k = 0; k < fs->sectors_per_cluster; k++) {
            uint32_t lba = base + k;
            if (read_sec(fs, lba, sec) != 0) {
                return -1;
            }
            for (int i = 0; i < FAT16_ENTRIES_PER_SECTOR; i++) {
                const uint8_t *e = sec + i * FAT16_ENTRY_SIZE;
                if (e[0] == 0x00 || e[0] == 0xE5) {
                    loc->dir_sector = lba;
                    loc->dir_index = (uint16_t)i;
                    loc->first_cluster = 0;
                    loc->size = 0;
                    loc->attr = 0;
                    return 0;
                }
            }
        }
        uint16_t next = FAT16_EOC;
        if (fat_get(fs, c, &next) != 0) {
            return -1;
        }
        if (next >= 0xFFF8u) {
            break;
        }
        c = next;
    }
    /* 空きが無いのでクラスタを 1 個足す。 */
    uint32_t nc = 0;
    if (fat_alloc(fs, &nc) != 0) {
        return -1;
    }
    if (fat_set(fs, last, (uint16_t)nc) != 0) {
        (void)fat_set(fs, nc, 0);
        return -1;
    }
    /* 新しいディレクトリクラスタを指す前に FAT を書き出しておく。
     * 書き出さずにこのクラスタへエントリを書くと、途中で止まった
     * 時にディスク上の FAT が「空き」のままになり、後で別の
     * ファイルに使い回されてしまう。 */
    if (fat_flush(fs) != 0) {
        return -1;
    }
    uint8_t zero[FAT16_SECTOR];
    memset(zero, 0, sizeof(zero));
    uint32_t base = cluster_lba(fs, nc);
    for (uint8_t k = 0; k < fs->sectors_per_cluster; k++) {
        if (write_sec(fs, base + k, zero) != 0) {
            return -1;
        }
    }
    loc->dir_sector = base;
    loc->dir_index = 0;
    loc->first_cluster = 0;
    loc->size = 0;
    loc->attr = 0;
    return 0;
}

/* パスを親ディレクトリと末尾の 8.3 名前に分ける。
 * "/boot/prismos.new" -> 親は /boot、名前は PRISMOS NEW。
 * 深さは 8 段まで。 */
static int split_path(fat16_fs *fs, const char *path, fat16_dir *dir_out,
                      uint8_t name11[11]) {
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    /* 要素に分ける。先頭の '/' は飛ばす。 */
    const char *parts[8];
    size_t lens[8];
    int nparts = 0;
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    while (*p != '\0') {
        if (nparts >= 8) {
            return -1;
        }
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        parts[nparts] = start;
        lens[nparts] = (size_t)(p - start);
        nparts++;
        while (*p == '/') {
            p++;
        }
    }
    if (nparts == 0) {
        return -1;
    }
    fat16_dir dir;
    dir.is_root = 1;
    dir.first_cluster = 0;
    for (int i = 0; i + 1 < nparts; i++) {
        uint8_t n11[11];
        if (to_83(parts[i], lens[i], n11) != 0) {
            return -1;
        }
        fat16_loc loc;
        if (dir_find(fs, &dir, n11, &loc) != 0) {
            return -1;
        }
        if ((loc.attr & FAT16_ATTR_DIR) == 0) {
            return -1; /* 途中の要素がディレクトリではない */
        }
        dir.is_root = 0;
        dir.first_cluster = loc.first_cluster;
    }
    if (to_83(parts[nparts - 1], lens[nparts - 1], name11) != 0) {
        return -1;
    }
    *dir_out = dir;
    return 0;
}

static int write_entry(fat16_fs *fs, const fat16_loc *loc,
                       const uint8_t name11[11], uint8_t attr,
                       uint16_t first_cluster, uint32_t size) {
    uint8_t sec[FAT16_SECTOR];
    if (read_sec(fs, loc->dir_sector, sec) != 0) {
        return -1;
    }
    uint8_t *e = sec + loc->dir_index * FAT16_ENTRY_SIZE;
    if (name11 != NULL) {
        memcpy(e, name11, 11);
    }
    e[11] = attr;
    e[12] = 0;
    for (int i = 13; i < 26; i++) {
        e[i] = 0; /* 時刻・日付・予約領域は 0 のまま */
    }
    wr16(e + 26, first_cluster);
    wr32(e + 28, size);
    return write_sec(fs, loc->dir_sector, sec);
}

static int delete_loc(fat16_fs *fs, const fat16_loc *loc) {
    if (loc->first_cluster != 0) {
        fat_free_chain(fs, loc->first_cluster);
        /* 解放した鎖を先に書き出す。失敗したらエントリは残す
         * (ディスク上の FAT は鎖を持ったままで、ファイルは
         * 壊れずに残る)。 */
        if (fat_flush(fs) != 0) {
            return -1;
        }
    }
    uint8_t sec[FAT16_SECTOR];
    if (read_sec(fs, loc->dir_sector, sec) != 0) {
        return -1;
    }
    sec[loc->dir_index * FAT16_ENTRY_SIZE] = 0xE5;
    return write_sec(fs, loc->dir_sector, sec);
}

/* ---- 公開 API ---- */

int fat16_open(const fat16_dev *dev, fat16_fs *fs) {
    if (dev == NULL || fs == NULL || dev->read_sector == NULL ||
        dev->write_sector == NULL) {
        return -1;
    }
    memset(fs, 0, sizeof(*fs));
    fs->dev = *dev;
    uint8_t sec[FAT16_SECTOR];
    if (dev->read_sector(dev->ctx, 0, sec) != 0) {
        return -1;
    }
    uint32_t part_lba = 0;
    uint32_t part_sectors = dev->total_sectors;
    if (sec[510] == 0x55 && sec[511] == 0xAA) {
        int found = 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = sec + 446 + i * 16;
            if (e[4] == 0x06 || e[4] == 0x0E) {
                part_lba = rd32(e + 8);
                part_sectors = rd32(e + 12);
                found = 1;
                break;
            }
        }
        if (!found) {
            return -1; /* FAT16 のパーティションが無い */
        }
        if (dev->read_sector(dev->ctx, part_lba, sec) != 0) {
            return -1;
        }
    }
    /* BPB の検査。 */
    if (sec[0] != 0xEB && sec[0] != 0xE9) {
        return -1;
    }
    uint16_t bps = rd16(sec + 11);
    uint8_t spc = sec[13];
    uint16_t reserved = rd16(sec + 14);
    uint8_t nfats = sec[16];
    uint16_t root_entries = rd16(sec + 17);
    uint16_t tot16 = rd16(sec + 19);
    uint16_t fat_sectors = rd16(sec + 22);
    uint32_t tot32 = rd32(sec + 32);
    if (bps != FAT16_SECTOR || spc == 0 || (spc & (spc - 1)) != 0 ||
        reserved == 0 || nfats == 0 || root_entries == 0 ||
        fat_sectors == 0) {
        return -1;
    }
    uint32_t total = tot16 != 0 ? (uint32_t)tot16 : tot32;
    if (total == 0) {
        return -1;
    }
    uint32_t root_sectors =
        ((uint32_t)root_entries * FAT16_ENTRY_SIZE + FAT16_SECTOR - 1) /
        FAT16_SECTOR;
    uint32_t data_sectors =
        total - (uint32_t)reserved - (uint32_t)nfats * fat_sectors -
        root_sectors;
    uint32_t clusters = data_sectors / (uint32_t)spc;
    if (clusters < 4085) {
        return -2; /* FAT12 */
    }
    if (clusters >= 65525) {
        return -2; /* FAT32 */
    }
    fs->part_lba = part_lba;
    fs->total_sectors = part_sectors != 0 ? part_sectors : total;
    fs->bytes_per_sector = bps;
    fs->sectors_per_cluster = spc;
    fs->reserved_sectors = reserved;
    fs->num_fats = nfats;
    fs->root_entries = root_entries;
    fs->fat_sectors = fat_sectors;
    fs->fat_start = part_lba + (uint32_t)reserved;
    fs->root_start =
        fs->fat_start + (uint32_t)nfats * (uint32_t)fat_sectors;
    fs->data_start = fs->root_start + root_sectors;
    fs->cluster_count = clusters;
    fs->alloc_hint = 2;
    fs->fat_cache_valid = 0;
    return 0;
}

int fat16_free_bytes(const fat16_fs *fs, uint64_t *out) {
    if (fs == NULL || out == NULL) {
        return -1;
    }
    fat16_fs *m = (fat16_fs *)fs;
    uint64_t free_clusters = 0;
    for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
        uint16_t v = 0;
        if (fat_get(m, c, &v) != 0) {
            return -1;
        }
        if (v == 0) {
            free_clusters++;
        }
    }
    *out = free_clusters * (uint64_t)fs->sectors_per_cluster *
           FAT16_SECTOR;
    return 0;
}

int fat16_file_size(const fat16_fs *fs, const char *path,
                    uint32_t *size_out) {
    if (fs == NULL || path == NULL || size_out == NULL) {
        return -1;
    }
    fat16_fs *m = (fat16_fs *)fs;
    fat16_dir dir;
    uint8_t name11[11];
    if (split_path(m, path, &dir, name11) != 0) {
        return -1;
    }
    fat16_loc loc;
    if (dir_find(m, &dir, name11, &loc) != 0) {
        return -1;
    }
    *size_out = loc.size;
    return 0;
}

int fat16_delete(fat16_fs *fs, const char *path) {
    if (fs == NULL || path == NULL) {
        return -1;
    }
    fat16_dir dir;
    uint8_t name11[11];
    if (split_path(fs, path, &dir, name11) != 0) {
        return -1;
    }
    fat16_loc loc;
    if (dir_find(fs, &dir, name11, &loc) != 0) {
        return -1;
    }
    if ((loc.attr & FAT16_ATTR_DIR) != 0) {
        return -1; /* ディレクトリは消さない */
    }
    return delete_loc(fs, &loc);
}

int fat16_read_file(const fat16_fs *fs, const char *path, uint8_t *buf,
                    size_t cap, size_t *len_out) {
    if (fs == NULL || path == NULL || buf == NULL || len_out == NULL) {
        return -1;
    }
    *len_out = 0;
    fat16_fs *m = (fat16_fs *)fs;
    fat16_dir dir;
    uint8_t name11[11];
    if (split_path(m, path, &dir, name11) != 0) {
        return -1;
    }
    fat16_loc loc;
    if (dir_find(m, &dir, name11, &loc) != 0) {
        return -1;
    }
    if (loc.size > cap) {
        return -2;
    }
    uint8_t sec[FAT16_SECTOR];
    size_t done = 0;
    uint32_t c = loc.first_cluster;
    uint32_t guard = 0;
    while (done < loc.size) {
        if (c < 2 || c >= fs->cluster_count + 2 ||
            guard++ > fs->cluster_count) {
            return -1; /* 鎖が壊れている */
        }
        uint32_t base = cluster_lba(fs, c);
        for (uint8_t k = 0; k < fs->sectors_per_cluster &&
                           done < loc.size;
             k++) {
            if (read_sec(fs, base + k, sec) != 0) {
                return -1;
            }
            size_t n = loc.size - done;
            if (n > FAT16_SECTOR) {
                n = FAT16_SECTOR;
            }
            memcpy(buf + done, sec, n);
            done += n;
        }
        uint16_t next = FAT16_EOC;
        if (fat_get(m, c, &next) != 0) {
            return -1;
        }
        if (next >= 0xFFF8u) {
            break;
        }
        c = next;
    }
    if (done != loc.size) {
        return -1;
    }
    *len_out = done;
    return 0;
}

int fat16_write_begin(fat16_fs *fs, const char *path, fat16_writer *w) {
    if (fs == NULL || path == NULL || w == NULL) {
        return -1;
    }
    memset(w, 0, sizeof(*w));
    fat16_dir dir;
    uint8_t name11[11];
    if (split_path(fs, path, &dir, name11) != 0) {
        return -1;
    }
    /* 同名があれば先に消す。 */
    fat16_loc old;
    if (dir_find(fs, &dir, name11, &old) == 0) {
        if ((old.attr & FAT16_ATTR_DIR) != 0) {
            return -1;
        }
        if (delete_loc(fs, &old) != 0) {
            return -1;
        }
    }
    fat16_loc loc;
    if (dir_find_free(fs, &dir, &loc) != 0) {
        return -1;
    }
    if (write_entry(fs, &loc, name11, 0x20, 0, 0) != 0) {
        return -1;
    }
    w->fs = fs;
    w->dir_sector = loc.dir_sector;
    w->dir_index = loc.dir_index;
    w->first_cluster = 0;
    w->size = 0;
    w->cur_cluster = 0;
    w->sector_lba = 0;
    w->sector_in_cluster = 0;
    w->sector_used = 0;
    w->active = 1;
    return 0;
}

static int writer_next_sector(fat16_writer *w) {
    fat16_fs *fs = w->fs;
    if (w->cur_cluster == 0) {
        uint32_t c = 0;
        if (fat_alloc(fs, &c) != 0) {
            return -1;
        }
        w->cur_cluster = c;
        w->first_cluster = (uint16_t)c;
        w->sector_in_cluster = 0;
        w->sector_lba = cluster_lba(fs, c);
        return 0;
    }
    if (w->sector_in_cluster + 1 < fs->sectors_per_cluster) {
        w->sector_in_cluster++;
        w->sector_lba = cluster_lba(fs, w->cur_cluster) +
                        w->sector_in_cluster;
        return 0;
    }
    uint32_t c = 0;
    if (fat_alloc(fs, &c) != 0) {
        return -1;
    }
    if (fat_set(fs, w->cur_cluster, (uint16_t)c) != 0) {
        (void)fat_set(fs, c, 0);
        return -1;
    }
    w->cur_cluster = c;
    w->sector_in_cluster = 0;
    w->sector_lba = cluster_lba(fs, c);
    return 0;
}

int fat16_write_chunk(fat16_writer *w, const uint8_t *data, size_t len) {
    if (w == NULL || !w->active || data == NULL) {
        return -1;
    }
    while (len > 0) {
        if (w->sector_used == 0 && w->sector_lba == 0) {
            if (writer_next_sector(w) != 0) {
                return -1; /* 空きが無い */
            }
        }
        size_t room = FAT16_SECTOR - w->sector_used;
        size_t n = len < room ? len : room;
        memcpy(w->sector_buf + w->sector_used, data, n);
        w->sector_used = (uint16_t)(w->sector_used + n);
        data += n;
        len -= n;
        w->size += (uint32_t)n;
        if (w->sector_used == FAT16_SECTOR) {
            if (write_sec(w->fs, w->sector_lba, w->sector_buf) != 0) {
                return -1;
            }
            w->sector_used = 0;
            w->sector_lba = 0;
        }
    }
    return 0;
}

int fat16_write_finish(fat16_writer *w) {
    if (w == NULL || !w->active) {
        return -1;
    }
    if (w->sector_used > 0) {
        memset(w->sector_buf + w->sector_used, 0,
               FAT16_SECTOR - w->sector_used);
        if (write_sec(w->fs, w->sector_lba, w->sector_buf) != 0) {
            return -1;
        }
        w->sector_used = 0;
        w->sector_lba = 0;
    }
    /* 鎖を先に書き出す。エントリにクラスタ番号と大きさを入れる
     * 時には、指す先の鎖がディスク上に揃っていること。 */
    if (fat_flush(w->fs) != 0) {
        return -1;
    }
    fat16_loc loc;
    loc.dir_sector = w->dir_sector;
    loc.dir_index = w->dir_index;
    loc.first_cluster = w->first_cluster;
    loc.size = w->size;
    loc.attr = 0x20;
    if (write_entry(w->fs, &loc, NULL, 0x20, w->first_cluster,
                    w->size) != 0) {
        return -1;
    }
    w->active = 0;
    return 0;
}

int fat16_write_abort(fat16_writer *w) {
    if (w == NULL || !w->active) {
        return -1;
    }
    if (w->first_cluster != 0) {
        fat_free_chain(w->fs, w->first_cluster);
    }
    /* 解放した分を書き出す (失敗しても best-effort)。 */
    (void)fat_flush(w->fs);
    fat16_loc loc;
    loc.dir_sector = w->dir_sector;
    loc.dir_index = w->dir_index;
    loc.first_cluster = 0;
    loc.size = 0;
    loc.attr = 0x20;
    uint8_t sec[FAT16_SECTOR];
    if (read_sec(w->fs, loc.dir_sector, sec) == 0) {
        sec[loc.dir_index * FAT16_ENTRY_SIZE] = 0xE5;
        (void)write_sec(w->fs, loc.dir_sector, sec);
    }
    w->active = 0;
    return 0;
}

int fat16_swap_entries(fat16_fs *fs, const char *path_a,
                       const char *path_b) {
    if (fs == NULL || path_a == NULL || path_b == NULL) {
        return -1;
    }
    fat16_dir dir_a;
    uint8_t name_a[11];
    if (split_path(fs, path_a, &dir_a, name_a) != 0) {
        return -1;
    }
    fat16_loc loc_a;
    if (dir_find(fs, &dir_a, name_a, &loc_a) != 0) {
        return -1;
    }
    fat16_dir dir_b;
    uint8_t name_b[11];
    if (split_path(fs, path_b, &dir_b, name_b) != 0) {
        return -1;
    }
    fat16_loc loc_b;
    if (dir_find(fs, &dir_b, name_b, &loc_b) != 0) {
        return -1;
    }
    if (loc_a.dir_sector != loc_b.dir_sector) {
        return -2; /* 違うセクタ: 1 回の書き込みでは入れ替えられない */
    }
    /* 両方の鎖がディスク上に揃ってから入れ替える。 */
    if (fat_flush(fs) != 0) {
        return -1;
    }
    uint8_t sec[FAT16_SECTOR];
    if (read_sec(fs, loc_a.dir_sector, sec) != 0) {
        return -1;
    }
    uint8_t *ea = sec + loc_a.dir_index * FAT16_ENTRY_SIZE;
    uint8_t *eb = sec + loc_b.dir_index * FAT16_ENTRY_SIZE;
    /* クラスタ番号 (2 バイト) と大きさ (4 バイト) の 6 バイトを
     * 入れ替える。名前と属性はそのまま。 */
    uint8_t tmp[6];
    memcpy(tmp, ea + 26, 6);
    memcpy(ea + 26, eb + 26, 6);
    memcpy(eb + 26, tmp, 6);
    return write_sec(fs, loc_a.dir_sector, sec);
}
