/* PrismOS — ディスク上のファイル形式: MBR + 表 + 固定枠データ。 */
#include "diskfs.h"

static const char DISKFS_MAGIC[8] = { 'P', 'R', 'I', 'S', 'M', 'F', 'S', '2' };

/* 大きな作業バッファは静的に置く。理由: カーネルのスタックは
 * 32KB しかなく、回復・更新からの同期のように呼び出しが深い
 * 場所で 2-4KB の配列をスタックに置くと溢れる。2026-11-21 に
 * 回復モードの選択肢 4 で発覚した: diskfs_write_file (約 8.9KB)
 * の中の diskfs_list 呼び出しで合計約 19.6KB になり、スタック
 * 直下の VFS のファイル表がゼロで潰れ、名前が消えたファイル
 * を同期が全て消すという連鎖になった。カーネルもホストテスト
 * もシングルスレッドで、割り込みから diskfs は呼ばれないため、
 * 静的バッファで問題ない。diskfs_list は diskfs_write_file の
 * 中から呼ばれるので、表だけは別々のバッファにする。 */
static uint8_t table_scratch[DISKFS_TABLE_SECTORS * DISKFS_SECTOR_SIZE];
static uint8_t list_table_scratch[DISKFS_TABLE_SECTORS *
                                   DISKFS_SECTOR_SIZE];
static uint8_t enc_scratch[DISKFS_MAX_FILE_SIZE];
static diskfs_entry count_scratch[DISKFS_MAX_FILES];

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

static int name_ok(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    size_t n = 0;
    while (name[n] != '\0') {
        n++;
    }
    if (n > DISKFS_MAX_NAME) {
        return 0;
    }
    /* '/' はディレクトリ区切り (VFS と同じ規則)。 */
    size_t comp = 0;
    for (size_t i = 0; ; i++) {
        char c = name[i];
        if (c == '/' || c == '\0') {
            if (comp == 0) {
                return 0;
            }
            if (comp == 1 && name[i - 1] == '.') {
                return 0;
            }
            if (comp == 2 && name[i - 2] == '.' &&
                name[i - 1] == '.') {
                return 0;
            }
            comp = 0;
            if (c == '\0') {
                break;
            }
            continue;
        }
        if (c == '\\' || (unsigned char)c < 0x20) {
            return 0;
        }
        comp++;
    }
    if ((name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
        return 0;
    }
    return 1;
}

static int name_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* base_lba を足した絶対セクタ番号。 */
static uint32_t sec(diskfs_dev *dev, uint32_t rel) {
    return dev->base_lba + rel;
}

/* ---- 形式の版ごとの配置 ----
 * 版2: 64 件、表 8 セクタ、データ開始 10、スナップショット
 *      開始 2058、スロット 2057 セクタ。
 * 版3: 128 件、表 16 セクタ、データ開始 18、スナップショット
 *      開始 4114、スロット 4113 セクタ。
 * 版4: 128 件、表 16 セクタ、メタ 2 セクタ、データ開始 20、
 *      スナップショット開始 4116、スロット 4115 セクタ。
 * 版5: 版4 と同じ配置に、大きなファイル領域 (開始 20576)
 *      を足したもの。1 ファイルのセクタ数 (32 = 16384 バイト)
 *      は版2/3/4/5 の固定枠で共通。配置はスーパーブロックの
 *      版とデータ開始セクタから決める。版2/3/4 のディスクは
 *      そのまま読み書きできる。 */
typedef struct {
    int max_files;
    uint32_t table_sectors;
    uint32_t data_sector;
    uint32_t snap_base;
    uint32_t snap_slot_sectors;
    /* 版4のみ: メタ領域 (作成/更新時刻) の開始セクタ。
     * 0 = 無し (版2/3)。 */
    uint32_t meta_sector;
    /* 版5のみ: 大きなファイル領域の開始セクタ (相対)。
     * 0 = 無し (版2/3/4)。大きさは total_sectors から決まる。 */
    uint32_t big_sector;
} diskfs_layout;

static const diskfs_layout LAYOUT_V2 = { 64, 8, 10, 2058, 2057, 0, 0 };
static const diskfs_layout LAYOUT_V3 = { 128, 16, 18, 4114, 4113, 0, 0 };
/* 版4: 表 16 セクタ (2-17)、メタ 2 セクタ (18-19、
 * 128 件 x 8 バイト)、データは 20 から。 */
static const diskfs_layout LAYOUT_V4 = { 128, 16, 20, 4116, 4115, 18, 0 };
/* 版5: 版4 と同じ配置 + 大きなファイル領域 (20576〜)。 */
static const diskfs_layout LAYOUT_V5 = {
    128, 16, 20, 4116, 4115, 18, DISKFS_BIG_SECTOR
};

/* 大きなファイル領域の大きさ (セクタ数)。領域が無ければ 0。 */
static uint32_t big_area_sectors(diskfs_dev *dev,
                                 const diskfs_layout *lay) {
    if (lay->big_sector == 0 ||
        dev->total_sectors <= lay->big_sector) {
        return 0;
    }
    return dev->total_sectors - lay->big_sector;
}

static int layout_of(diskfs_dev *dev, diskfs_layout *out) {
    uint8_t sector[DISKFS_SECTOR_SIZE];
    if (dev->read_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR),
                         sector) != 0) {
        return -1;
    }
    for (int i = 0; i < 8; i++) {
        if (sector[i] != (uint8_t)DISKFS_MAGIC[i]) {
            return -1;
        }
    }
    if (rd32(sector + 20) != DISKFS_SECTORS_PER_FILE) {
        return -1;
    }
    if (rd32(sector + 8) == 2 &&
        rd32(sector + 16) == LAYOUT_V2.data_sector) {
        *out = LAYOUT_V2;
        return 0;
    }
    if (rd32(sector + 8) == 3 &&
        rd32(sector + 16) == LAYOUT_V3.data_sector) {
        *out = LAYOUT_V3;
        return 0;
    }
    if (rd32(sector + 8) == 4 &&
        rd32(sector + 16) == LAYOUT_V4.data_sector) {
        *out = LAYOUT_V4;
        return 0;
    }
    if (rd32(sector + 8) == 5 &&
        rd32(sector + 16) == LAYOUT_V5.data_sector) {
        *out = LAYOUT_V5;
        return 0;
    }
    return -1;
}

/* FNV-1a (ファイルのチェックサム用)。 */
static uint32_t fnv1a(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ data[i]) * 16777619u;
    }
    return h;
}

/* 時刻の源 (Unix 秒)。カーネルが RTC から設定する。
 * 未設定なら 0 (時刻不明として記録)。 */
static uint32_t (*diskfs_time_fn)(void) = NULL;

void diskfs_set_time_source(uint32_t (*fn)(void)) {
    diskfs_time_fn = fn;
}

static uint32_t diskfs_now(void) {
    return diskfs_time_fn != NULL ? diskfs_time_fn() : 0;
}

/* メタ領域 (版4) の読み書き。1 件 8 バイト:
 * ctime (u32), mtime (u32) をスロット番号順に並べる。 */
static uint8_t meta_scratch[2 * DISKFS_SECTOR_SIZE];

static int meta_load(diskfs_dev *dev, const diskfs_layout *lay) {
    if (lay->meta_sector == 0) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (dev->read_sector(dev->ctx,
                             sec(dev, lay->meta_sector + (uint32_t)i),
                             meta_scratch + i * DISKFS_SECTOR_SIZE) !=
            0) {
            return -1;
        }
    }
    return 0;
}

static int meta_flush(diskfs_dev *dev, const diskfs_layout *lay) {
    for (int i = 0; i < 2; i++) {
        if (dev->write_sector(dev->ctx,
                              sec(dev, lay->meta_sector + (uint32_t)i),
                              meta_scratch + i * DISKFS_SECTOR_SIZE) !=
            0) {
            return -1;
        }
    }
    return 0;
}

static void meta_get(int slot, uint32_t *ctime, uint32_t *mtime) {
    *ctime = rd32(meta_scratch + slot * 8);
    *mtime = rd32(meta_scratch + slot * 8 + 4);
}

static void meta_set(int slot, uint32_t ctime, uint32_t mtime) {
    wr32(meta_scratch + slot * 8, ctime);
    wr32(meta_scratch + slot * 8 + 4, mtime);
}

/* Defined with the snapshot code below; the plain read
 * path validates table entries with it too. */
static int snap_entry_valid(diskfs_dev *dev, const diskfs_entry *e,
                            const diskfs_layout *lay);

/* Defined with the snapshot code below; diskfs_write_file
 * uses it to place big files. */
static uint32_t big_alloc(diskfs_dev *dev, const diskfs_layout *lay,
                          const uint8_t *table, uint32_t sectors,
                          int exclude_slot);


/* ファイル表 (4096 バイト) を読む。 */
static int read_table(diskfs_dev *dev, uint8_t *table,
                      const diskfs_layout *lay) {
    for (uint32_t i = 0; i < lay->table_sectors; i++) {
        if (dev->read_sector(dev->ctx, sec(dev, DISKFS_TABLE_SECTOR + i),
                             table + i * DISKFS_SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

static int write_table(diskfs_dev *dev, const uint8_t *table,
                       const diskfs_layout *lay) {
    for (uint32_t i = 0; i < lay->table_sectors; i++) {
        if (dev->write_sector(dev->ctx, sec(dev, DISKFS_TABLE_SECTOR + i),
                              table + i * DISKFS_SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

uint32_t diskfs_find_data_partition(const uint8_t *mbr, uint32_t *out_size) {
    if (mbr == NULL) {
        return 0;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t *part = mbr + 446 + i * 16;
        if (part[4] != DISKFS_PART_TYPE) {
            continue;
        }
        uint32_t start = rd32(part + 8);
        uint32_t size = rd32(part + 12);
        if (start == DISKFS_SB_SECTOR) {
            return 0; /* 従来の配置: パーティション自体が全体 */
        }
        if (out_size != NULL) {
            *out_size = size;
        }
        return start;
    }
    return 0;
}

/* 表の i 番目の項目を読む。used が 0 なら空き。
 * 項目の配置 (64 バイト): 名前 0-31、大きさ 32-35、
 * 開始セクタ 36-39、使用中 40、暗号化 41、大きなファイル
 * 42、IV 44-59。+42 は版5 で使う (旧版は読まない)。 */
static void entry_from(const uint8_t *table, int i, diskfs_entry *e) {
    const uint8_t *p = table + i * 64;
    size_t n = 0;
    while (n < DISKFS_MAX_NAME && p[n] != '\0') {
        e->name[n] = (char)p[n];
        n++;
    }
    e->name[n] = '\0';
    e->size = rd32(p + 32);
    e->start_sector = rd32(p + 36);
    e->used = p[40] != 0;
    e->encrypted = p[41] != 0;
    e->big = p[42] != 0;
    for (int j = 0; j < 16; j++) {
        e->iv[j] = p[44 + j];
    }
    e->checksum = rd32(p + 60);
    e->ctime = 0;
    e->mtime = 0;
}

static void entry_to(uint8_t *table, int i, const diskfs_entry *e) {
    uint8_t *p = table + i * 64;
    for (int j = 0; j < 64; j++) {
        p[j] = 0;
    }
    size_t n = 0;
    while (e->name[n] != '\0' && n < DISKFS_MAX_NAME) {
        p[n] = (uint8_t)e->name[n];
        n++;
    }
    wr32(p + 32, e->size);
    wr32(p + 36, e->start_sector);
    p[40] = e->used ? 1 : 0;
    p[41] = e->encrypted ? 1 : 0;
    p[42] = e->big ? 1 : 0;
    for (int j = 0; j < 16; j++) {
        p[44 + j] = e->iv[j];
    }
    wr32(p + 60, e->checksum);
}

int diskfs_format(diskfs_dev *dev) {
    if (dev == NULL || dev->read_sector == NULL ||
        dev->write_sector == NULL) {
        return -1;
    }
    if (dev->total_sectors <
        LAYOUT_V5.data_sector +
            (uint32_t)LAYOUT_V5.max_files * DISKFS_SECTORS_PER_FILE) {
        return -1;
    }
    uint8_t sector[DISKFS_SECTOR_SIZE];
    if (dev->base_lba == 0) {
        /* MBR: パーティション 1 個 (開始 LBA 1、残り全部)。
         * base_lba > 0 のときは MBR を書かない (ブートローダの
         * MBR や起動用パーティションを壊さないため)。 */
        for (int i = 0; i < DISKFS_SECTOR_SIZE; i++) {
            sector[i] = 0;
        }
        uint8_t *part = sector + 446;
        part[0] = 0x00; /* 起動しない */
        part[1] = 0x00;
        part[2] = 0x02;
        part[3] = 0x00;
        part[4] = DISKFS_PART_TYPE;
        part[5] = 0xFF;
        part[6] = 0xFF;
        part[7] = 0xFF;
        wr32(part + 8, DISKFS_SB_SECTOR);
        wr32(part + 12, dev->total_sectors - DISKFS_SB_SECTOR);
        sector[510] = 0x55;
        sector[511] = 0xAA;
        if (dev->write_sector(dev->ctx, sec(dev, DISKFS_MBR_SECTOR),
                              sector) != 0) {
            return -1;
        }
    }
    /* スーパーブロック。 */
    for (int i = 0; i < DISKFS_SECTOR_SIZE; i++) {
        sector[i] = 0;
    }
    for (int i = 0; i < 8; i++) {
        sector[i] = (uint8_t)DISKFS_MAGIC[i];
    }
    wr32(sector + 8, 5); /* 版 (新規は版5で書く) */
    wr32(sector + 12, 0); /* ファイル数 */
    wr32(sector + 16, LAYOUT_V5.data_sector);
    wr32(sector + 20, DISKFS_SECTORS_PER_FILE);
    wr32(sector + 24, dev->total_sectors);
    if (dev->write_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -1;
    }
    /* 空のファイル表。 */
    uint8_t *table = table_scratch;
    for (size_t i = 0; i < LAYOUT_V5.table_sectors * DISKFS_SECTOR_SIZE;
         i++) {
        table[i] = 0;
    }
    if (write_table(dev, table, &LAYOUT_V5) != 0) {
        return -1;
    }
    /* 空のメタ領域 (版4/5)。 */
    for (size_t i = 0; i < sizeof(meta_scratch); i++) {
        meta_scratch[i] = 0;
    }
    if (meta_flush(dev, &LAYOUT_V5) != 0) {
        return -1;
    }
    return 0;
}

/* 版4 のディスクを版5 へ移行する。版番号だけを変える。
 * 版4 と版5 は表・メタ・データ・復元ポイントの配置が同じ
 * なので、データの移動は要らない。 */
int diskfs_migrate_to_v5(diskfs_dev *dev) {
    if (dev == NULL || dev->read_sector == NULL ||
        dev->write_sector == NULL) {
        return -1;
    }
    uint8_t sector[DISKFS_SECTOR_SIZE];
    if (dev->read_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -1;
    }
    for (int i = 0; i < 8; i++) {
        if (sector[i] != (uint8_t)DISKFS_MAGIC[i]) {
            return -1;
        }
    }
    if (rd32(sector + 8) != 4 ||
        rd32(sector + 16) != LAYOUT_V4.data_sector) {
        return -2;
    }
    /* 表の整合性を確認してから版番号だけを変える。 */
    diskfs_layout lay = LAYOUT_V4;
    uint8_t *table = table_scratch;
    if (read_table(dev, table, &lay) != 0) {
        return -1;
    }
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (!snap_entry_valid(dev, &e, &lay)) {
            return -3;
        }
    }
    wr32(sector + 8, 5);
    if (dev->write_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR),
                          sector) != 0) {
        return -1;
    }
    return 0;
}

int diskfs_is_formatted(diskfs_dev *dev) {
    if (dev == NULL || dev->read_sector == NULL) {
        return -1;
    }
    uint8_t sector[DISKFS_SECTOR_SIZE];
    if (dev->base_lba == 0) {
        /* 従来の配置: MBR の署名とパーティションを確認する。 */
        if (dev->read_sector(dev->ctx, sec(dev, DISKFS_MBR_SECTOR),
                             sector) != 0) {
            return -1;
        }
        if (sector[510] != 0x55 || sector[511] != 0xAA) {
            return 0;
        }
        const uint8_t *part = sector + 446;
        if (part[4] != DISKFS_PART_TYPE) {
            return 0;
        }
        if (rd32(part + 8) != DISKFS_SB_SECTOR) {
            return 0;
        }
    }
    {
        diskfs_layout lay;
        if (layout_of(dev, &lay) != 0) {
            return 0;
        }
    }
    return 1;
}

int diskfs_list(diskfs_dev *dev, diskfs_entry *entries, int *out_count) {
    if (dev == NULL || entries == NULL || out_count == NULL) {
        return -1;
    }
    diskfs_layout lay;
    if (layout_of(dev, &lay) != 0) {
        return -1;
    }
    uint8_t *table = list_table_scratch;
    if (read_table(dev, table, &lay) != 0) {
        return -1;
    }
    int has_meta = lay.meta_sector != 0 &&
                   meta_load(dev, &lay) == 0;
    int n = 0;
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (e.used) {
            if (has_meta) {
                meta_get(i, &e.ctime, &e.mtime);
            }
            entries[n++] = e;
        }
    }
    *out_count = n;
    return 0;
}

static int read_file_impl(diskfs_dev *dev, const char *name, uint8_t *buf,
                          size_t cap, size_t *out_len, int decrypt) {
    if (dev == NULL || !name_ok(name) || buf == NULL) {
        return -1;
    }
    diskfs_layout lay;
    if (layout_of(dev, &lay) != 0) {
        return -1;
    }
    uint8_t *table = table_scratch;
    if (read_table(dev, table, &lay) != 0) {
        return -1;
    }
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (!e.used || !name_eq(e.name, name)) {
            continue;
        }
        if (e.size > cap) {
            return -1;
        }
        /* 壊れた表でもデータ領域の外を読まない。 */
        if (!snap_entry_valid(dev, &e, &lay)) {
            return -1;
        }
        size_t got = 0;
        uint8_t sector[DISKFS_SECTOR_SIZE];
        uint32_t sectors = (uint32_t)((e.size + DISKFS_SECTOR_SIZE - 1) /
                                      DISKFS_SECTOR_SIZE);
        for (uint32_t s = 0; s < sectors; s++) {
            if (dev->read_sector(dev->ctx, e.start_sector + s, sector) != 0) {
                return -1;
            }
            size_t take = e.size - got;
            if (take > DISKFS_SECTOR_SIZE) {
                take = DISKFS_SECTOR_SIZE;
            }
            for (size_t j = 0; j < take; j++) {
                buf[got + j] = sector[j];
            }
            got += take;
        }
        if (e.encrypted) {
            if (!decrypt) {
                /* 復号しない読み取り: 暗号文のまま返す。 */
            } else if (dev->crypto == NULL || dev->crypto->decrypt == NULL) {
                return -1; /* 鍵が無ければ読めない */
            } else if (dev->crypto->decrypt(dev->crypto->ctx, e.iv, buf,
                                            buf, e.size) != 0) {
                return -1;
            }
        }
        /* 平文のチェックサム照合 (停電による途中書き換えや
         * セクタ破損の検出)。チェックサムが無い (0 の) 項目は
         * 旧形式で書かれたものとして照合しない。復号しない
         * 読み取りは暗号文なので照合しない。 */
        if (decrypt && e.checksum != 0 &&
            fnv1a(buf, e.size) != e.checksum) {
            return -6;
        }
        if (out_len != NULL) {
            *out_len = e.size;
        }
        return 0;
    }
    return -1;
}

int diskfs_read_file(diskfs_dev *dev, const char *name, uint8_t *buf,
                     size_t cap, size_t *out_len) {
    return read_file_impl(dev, name, buf, cap, out_len, 1);
}

int diskfs_read_file_raw(diskfs_dev *dev, const char *name, uint8_t *buf,
                         size_t cap, size_t *out_len) {
    return read_file_impl(dev, name, buf, cap, out_len, 0);
}

int diskfs_write_file(diskfs_dev *dev, const char *name,
                      const uint8_t *data, size_t len) {
    if (dev == NULL || !name_ok(name)) {
        return -3;
    }
    if (len > DISKFS_MAX_FILE_SIZE) {
        return -1;
    }
    if (len > 0 && data == NULL) {
        return -4;
    }
    diskfs_layout lay;
    if (layout_of(dev, &lay) != 0) {
        return -4;
    }
    uint8_t *table = table_scratch;
    if (read_table(dev, table, &lay) != 0) {
        return -4;
    }
    int slot = -1;
    int existed = 0;
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (e.used && name_eq(e.name, name)) {
            slot = i;
            existed = 1;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < lay.max_files; i++) {
            diskfs_entry e;
            entry_from(table, i, &e);
            if (!e.used) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            return -2;
        }
    }
    /* 16KB を超えるファイルは版5 の大きなファイル領域へ。
     * 旧版のディスク (大きなファイル領域が無い) には書けない。 */
    int is_big = len > DISKFS_SMALL_FILE_SIZE;
    uint32_t start = 0;
    if (!is_big) {
        start = sec(dev, lay.data_sector) +
                (uint32_t)slot * DISKFS_SECTORS_PER_FILE;
    } else {
        if (lay.big_sector == 0) {
            return -1;
        }
        uint32_t sectors =
            (uint32_t)((len + DISKFS_SECTOR_SIZE - 1) /
                       DISKFS_SECTOR_SIZE);
        start = big_alloc(dev, &lay, table, sectors,
                          existed ? slot : -1);
        if (start == 0) {
            return -1;
        }
    }
    /* 暗号化が有効なら、ここで暗号文に変える。IV はファイルごとに
     * 新しく作り、表に置く。 */
    uint8_t *enc_buf = enc_scratch;
    uint8_t iv[16];
    for (int j = 0; j < 16; j++) {
        iv[j] = 0;
    }
    int encrypted = 0;
    const uint8_t *to_write = data;
    if (dev->crypto != NULL && dev->crypto->encrypt != NULL) {
        if (dev->crypto->encrypt(dev->crypto->ctx, iv, data, enc_buf,
                                 len) != 0) {
            return -4;
        }
        to_write = enc_buf;
        encrypted = 1;
    }
    /* データを書いてから表を書く。表が先だと、途中で止まった
     * ときに壊れた項目が見えてしまう。 */
    uint8_t sector[DISKFS_SECTOR_SIZE];
    uint32_t sectors = (uint32_t)((len + DISKFS_SECTOR_SIZE - 1) /
                                  DISKFS_SECTOR_SIZE);
    for (uint32_t s = 0; s < sectors; s++) {
        for (int j = 0; j < DISKFS_SECTOR_SIZE; j++) {
            sector[j] = 0;
        }
        size_t off = (size_t)s * DISKFS_SECTOR_SIZE;
        size_t take = len - off;
        if (take > DISKFS_SECTOR_SIZE) {
            take = DISKFS_SECTOR_SIZE;
        }
        for (size_t j = 0; j < take; j++) {
            sector[j] = to_write[off + j];
        }
        if (dev->write_sector(dev->ctx, start + s, sector) != 0) {
            return -4;
        }
    }
    diskfs_entry e;
    size_t n = 0;
    while (name[n] != '\0' && n < DISKFS_MAX_NAME) {
        e.name[n] = name[n];
        n++;
    }
    e.name[n] = '\0';
    e.size = (uint32_t)len;
    e.start_sector = start;
    e.used = 1;
    e.big = is_big;
    e.encrypted = encrypted;
    for (int j = 0; j < 16; j++) {
        e.iv[j] = iv[j];
    }
    /* 平文に対するチェックサム (暗号化前)。 */
    e.checksum = fnv1a(data, len);
    e.ctime = 0;
    e.mtime = 0;
    entry_to(table, slot, &e);
    if (write_table(dev, table, &lay) != 0) {
        return -4;
    }
    /* 版4: 作成/更新時刻をメタ領域へ。作成時刻は上書きでは
     * 保持する。 */
    if (lay.meta_sector != 0 && meta_load(dev, &lay) == 0) {
        uint32_t ct = 0;
        uint32_t mt = 0;
        meta_get(slot, &ct, &mt);
        uint32_t now = diskfs_now();
        meta_set(slot, existed ? ct : now, now);
        (void)meta_flush(dev, &lay);
    }
    /* スーパーブロックのファイル数を数え直す。 */
    if (dev->read_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -4;
    }
    diskfs_entry *list = count_scratch;
    int count = 0;
    if (diskfs_list(dev, list, &count) != 0) {
        return -4;
    }
    wr32(sector + 12, (uint32_t)count);
    if (dev->write_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -4;
    }
    return 0;
}

int diskfs_delete_file(diskfs_dev *dev, const char *name) {
    if (dev == NULL || !name_ok(name)) {
        return -1;
    }
    diskfs_layout lay;
    if (layout_of(dev, &lay) != 0) {
        return -1;
    }
    uint8_t *table = table_scratch;
    if (read_table(dev, table, &lay) != 0) {
        return -1;
    }
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (e.used && name_eq(e.name, name)) {
            /* 全ての項目を初期化すること。encrypted と iv を
             * 書き忘れると、スタックのゴミが表に残り、消した
             * はずの項目が暗号化済みのように見えてしまう。 */
            diskfs_entry empty;
            empty.name[0] = '\0';
            empty.size = 0;
            empty.start_sector = 0;
            empty.used = 0;
            empty.big = 0;
            empty.encrypted = 0;
            empty.checksum = 0;
            empty.ctime = 0;
            empty.mtime = 0;
            for (int j = 0; j < 16; j++) {
                empty.iv[j] = 0;
            }
            entry_to(table, i, &empty);
            if (write_table(dev, table, &lay) != 0) {
                return -1;
            }
            if (lay.meta_sector != 0 &&
                meta_load(dev, &lay) == 0) {
                meta_set(i, 0, 0);
                (void)meta_flush(dev, &lay);
            }
            return 0;
        }
    }
    return -1;
}

/* ---- 復元ポイント (スナップショット) ---- */

#define SNAP_MAGIC_LEN 9
static const char SNAP_MAGIC[SNAP_MAGIC_LEN] = { 'P', 'R', 'I', 'S', 'M',
                                                  'S', 'N', 'A', 'P' };
#define SNAP_VERSION 2 /* 2: checksum at offset 76 (see below) */
#define SNAP_VERSION_LEGACY 1 /* no checksum; still readable */
#define SNAP_DATA_SECTORS(lay) \
    ((uint32_t)(lay)->max_files * DISKFS_SECTORS_PER_FILE)

static uint32_t snap_slot_base(diskfs_dev *dev, int slot,
                               const diskfs_layout *lay) {
    return sec(dev, lay->snap_base +
                        (uint32_t)slot * lay->snap_slot_sectors);
}

/* スロットのヘッダを読む。有効なら 1、空き・壊れなら 0、
 * 読めなければ -1。info が NULL でなければ内容を入れる。 */
static int snap_read_header(diskfs_dev *dev, int slot,
                            const diskfs_layout *lay,
                            diskfs_snapshot_info *info, uint32_t *seq_out) {
    uint8_t sector[DISKFS_SECTOR_SIZE];
    if (dev->read_sector(dev->ctx, snap_slot_base(dev, slot, lay),
                         sector) != 0) {
        return -1;
    }
    for (int i = 0; i < SNAP_MAGIC_LEN; i++) {
        if (sector[i] != (uint8_t)SNAP_MAGIC[i]) {
            return 0;
        }
    }
    if (rd32(sector + 12) != SNAP_VERSION &&
        rd32(sector + 12) != SNAP_VERSION_LEGACY) {
        return 0;
    }
    uint32_t count = rd32(sector + 72);
    if (count > (uint32_t)lay->max_files) {
        return 0;
    }
    if (seq_out != NULL) {
        *seq_out = rd32(sector + 16);
    }
    if (info != NULL) {
        size_t n = 0;
        while (n < DISKFS_SNAP_NAME_MAX && sector[20 + n] != '\0') {
            info->name[n] = (char)sector[20 + n];
            n++;
        }
        info->name[n] = '\0';
        size_t t = 0;
        while (t < 19 && sector[52 + t] != '\0') {
            info->timestamp[t] = (char)sector[52 + t];
            t++;
        }
        info->timestamp[t] = '\0';
        info->seq = rd32(sector + 16);
        info->file_count = count;
    }
    return 1;
}

/* A used table entry is sane when its data sits inside
 * the file data region and its size is within the file
 * size limit. The restore path has always checked this;
 * the create path and the plain read path now check the
 * same thing, so a damaged table cannot make any of them
 * read or write outside the data region (the unsigned
 * subtraction start_sector - data_start underflowed).
 * 版5 の大きなファイルは大きなファイル領域の中にあり、
 * 固定枠の整列は要らない。 */
static int snap_entry_valid(diskfs_dev *dev, const diskfs_entry *e,
                            const diskfs_layout *lay) {
    if (!e->used) {
        return 1;
    }
    if (e->size > DISKFS_MAX_FILE_SIZE) {
        return 0;
    }
    uint32_t sectors =
        (e->size + DISKFS_SECTOR_SIZE - 1) / DISKFS_SECTOR_SIZE;
    if (e->big) {
        uint32_t big_sectors = big_area_sectors(dev, lay);
        if (big_sectors == 0) {
            return 0;
        }
        uint32_t big_start = sec(dev, lay->big_sector);
        if (e->start_sector < big_start ||
            e->start_sector >= big_start + big_sectors) {
            return 0;
        }
        if (e->start_sector - big_start + sectors > big_sectors) {
            return 0;
        }
        return 1;
    }
    if (e->size > DISKFS_SMALL_FILE_SIZE) {
        return 0;
    }
    if (e->start_sector < sec(dev, lay->data_sector) ||
        e->start_sector >=
            sec(dev, lay->data_sector + SNAP_DATA_SECTORS(lay))) {
        return 0;
    }
    /* 終端と整列: データ領域は (ファイル枠 × 32 セクタ) の
     * 並びで、割当は必ず枠の先頭から始まる。枠をはみ出す
     * 終端や、枠の途中から始まるエントリは壊れている。 */
    {
        uint32_t off = e->start_sector - sec(dev, lay->data_sector);
        if ((off % DISKFS_SECTORS_PER_FILE) != 0) {
            return 0;
        }
        if (off + sectors > SNAP_DATA_SECTORS(lay)) {
            return 0;
        }
    }
    return 1;
}

/* 大きなファイル領域の空きを探す (first-fit)。table は
 * 現在のファイル表。exclude_slot は上書き中の自分の項目
 * (その古い領域は空きとして扱う。-1 = 無し)。
 * 見つかれば書き込み先の絶対セクタ、無ければ 0 を返す。 */
static uint32_t big_alloc(diskfs_dev *dev, const diskfs_layout *lay,
                          const uint8_t *table, uint32_t sectors,
                          int exclude_slot) {
    uint32_t area = big_area_sectors(dev, lay);
    if (area < sectors) {
        return 0;
    }
    /* 使用中の大きなファイルの区間を集め、開始順に並べる。 */
    uint32_t starts[DISKFS_MAX_FILES];
    uint32_t ends[DISKFS_MAX_FILES];
    int n = 0;
    uint32_t big_start = sec(dev, lay->big_sector);
    for (int i = 0; i < lay->max_files; i++) {
        if (i == exclude_slot) {
            continue;
        }
        diskfs_entry e;
        entry_from(table, i, &e);
        if (!e.used || !e.big || e.size == 0) {
            continue;
        }
        uint32_t s = e.start_sector - big_start;
        uint32_t cnt =
            (e.size + DISKFS_SECTOR_SIZE - 1) / DISKFS_SECTOR_SIZE;
        if (s >= area || s + cnt > area) {
            continue; /* 壊れた項目は無視 (検証は別途行う) */
        }
        starts[n] = s;
        ends[n] = s + cnt;
        n++;
    }
    /* 挿入ソート (最大 128 件)。 */
    for (int i = 1; i < n; i++) {
        uint32_t s = starts[i], en = ends[i];
        int j = i - 1;
        while (j >= 0 && starts[j] > s) {
            starts[j + 1] = starts[j];
            ends[j + 1] = ends[j];
            j--;
        }
        starts[j + 1] = s;
        ends[j + 1] = en;
    }
    uint32_t cur = 0;
    for (int i = 0; i < n; i++) {
        if (starts[i] >= cur + sectors) {
            return big_start + cur;
        }
        if (ends[i] > cur) {
            cur = ends[i];
        }
    }
    if (cur + sectors <= area) {
        return big_start + cur;
    }
    return 0;
}

/* FNV-1a over the snapshot's stored table and file data,
 * in table order. Create stores it in the header (offset
 * 76) after the copy; restore recomputes and compares,
 * so a half-written or damaged snapshot is detected
 * instead of restored. Returns 0 on success. */
static int snap_checksum(diskfs_dev *dev, int slot,
                         const diskfs_layout *lay,
                         const uint8_t *table, uint32_t *out) {
    uint32_t base = snap_slot_base(dev, slot, lay);
    uint32_t slot_data = base + 1 + lay->table_sectors;
    uint32_t data_start = sec(dev, lay->data_sector);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < lay->table_sectors * DISKFS_SECTOR_SIZE;
         i++) {
        h = (h ^ table[i]) * 16777619u;
    }
    uint8_t buf[DISKFS_SECTOR_SIZE];
    for (int i = 0; i < lay->max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (!e.used || e.size == 0 || e.big) {
            continue;
        }
        uint32_t sectors =
            (e.size + DISKFS_SECTOR_SIZE - 1) / DISKFS_SECTOR_SIZE;
        uint32_t off = e.start_sector - data_start;
        for (uint32_t s = 0; s < sectors; s++) {
            if (dev->read_sector(dev->ctx, slot_data + off + s,
                                 buf) != 0) {
                return -1;
            }
            for (int j = 0; j < DISKFS_SECTOR_SIZE; j++) {
                h = (h ^ buf[j]) * 16777619u;
            }
        }
    }
    *out = h;
    return 0;
}

int diskfs_snapshot_create(diskfs_dev *dev, const char *name,
                           const char *timestamp) {
    if (dev == NULL || !name_ok(name) || timestamp == NULL ||
        timestamp[0] == '\0') {
        return -1;
    }
    diskfs_layout lay;
    if (layout_of(dev, &lay) != 0) {
        return -3;
    }
    if (dev->total_sectors <
        lay.snap_base + DISKFS_SNAP_SLOTS * lay.snap_slot_sectors) {
        return -2;
    }
    /* 同じ名前があれば、まず消す (作り直し)。 */
    diskfs_snapshot_delete(dev, name);
    /* 空きスロットを探す。無ければいちばん古いものを上書きする。
     * 連番は全スロットを見て決める (途中で止めると、後の
     * スロットより小さい連番が付いてしまうため)。 */
    int slot = -1;
    uint32_t max_seq = 0;
    uint32_t min_seq = 0xFFFFFFFFu;
    int oldest = 0;
    for (int i = 0; i < DISKFS_SNAP_SLOTS; i++) {
        uint32_t seq = 0;
        int rc = snap_read_header(dev, i, &lay, NULL, &seq);
        if (rc < 0) {
            return -3;
        }
        if (rc == 0) {
            if (slot < 0) {
                slot = i;
            }
            continue;
        }
        if (seq > max_seq) {
            max_seq = seq;
        }
        if (seq < min_seq) {
            min_seq = seq;
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
    }
    uint32_t base = snap_slot_base(dev, slot, &lay);
    /* 今のファイル表とファイル数を読む。 */
    uint8_t *table = table_scratch;
    if (read_table(dev, table, &lay) != 0) {
        return -3;
    }
    int count = 0;
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (e.used) {
            count++;
        }
        /* 作成側も復元側と同じ検証をする。壊れた表の
         * start_sector で領域外を読み書きしないため。 */
        if (!snap_entry_valid(dev, &e, &lay)) {
            return -3;
        }
    }
    /* スロットの旧ヘッダを先に無効化する。コピーの途中で
     * 止まっても、古いヘッダが「有効なのに中身は混在」を
     * 主張しないようにするため。 */
    {
        uint8_t zero[DISKFS_SECTOR_SIZE];
        for (int i = 0; i < DISKFS_SECTOR_SIZE; i++) {
            zero[i] = 0;
        }
        if (dev->write_sector(dev->ctx, base, zero) != 0) {
            return -3;
        }
    }
    /* ヘッダの内容を用意する (書くのはコピーの最後)。 */
    uint8_t sector[DISKFS_SECTOR_SIZE];
    for (int i = 0; i < DISKFS_SECTOR_SIZE; i++) {
        sector[i] = 0;
    }
    for (int i = 0; i < SNAP_MAGIC_LEN; i++) {
        sector[i] = (uint8_t)SNAP_MAGIC[i];
    }
    wr32(sector + 12, SNAP_VERSION);
    wr32(sector + 16, max_seq + 1);
    size_t n = 0;
    while (name[n] != '\0' && n < DISKFS_SNAP_NAME_MAX) {
        sector[20 + n] = (uint8_t)name[n];
        n++;
    }
    size_t t = 0;
    while (timestamp[t] != '\0' && t < 19) {
        sector[52 + t] = (uint8_t)timestamp[t];
        t++;
    }
    wr32(sector + 72, (uint32_t)count);
    /* ファイル表のコピーを書く。 */
    for (uint32_t i = 0; i < lay.table_sectors; i++) {
        if (dev->write_sector(dev->ctx, base + 1 + i,
                              table + i * DISKFS_SECTOR_SIZE) != 0) {
            return -3;
        }
    }
    /* ファイルデータのコピーを書く。使われているファイルの分
     * だけ写す (各ファイルは大きさ分のセクタだけ)。データ領域
     * 全体 (64 ファイル x 32 セクタ = 2048 セクタ) を丸ごと
     * 写すと遅いため。使っていない枠は古い内容のままでよい:
     * 復元は表にあるファイルの分だけ読み戻すので、古い内容は
     * 決して読まれない。大きなファイル (版5) は写さない:
     * データは大きなファイル領域に残り、復元しても消えない。 */
    uint8_t buf[DISKFS_SECTOR_SIZE];
    uint32_t data_start = sec(dev, lay.data_sector);
    uint32_t slot_data = base + 1 + lay.table_sectors;
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (!e.used || e.size == 0 || e.big) {
            continue;
        }
        uint32_t sectors =
            (e.size + DISKFS_SECTOR_SIZE - 1) / DISKFS_SECTOR_SIZE;
        uint32_t off = e.start_sector - data_start;
        for (uint32_t s = 0; s < sectors; s++) {
            if (dev->read_sector(dev->ctx, e.start_sector + s, buf) != 0) {
                return -3;
            }
            if (dev->write_sector(dev->ctx, slot_data + off + s, buf) !=
                0) {
                return -3;
            }
        }
    }
    /* チェックサムを計算し、ヘッダを最後に書く (コミット)。
     * ここまで来なければスロットは無効化されたままになる。 */
    {
        uint32_t sum = 0;
        if (snap_checksum(dev, slot, &lay, table, &sum) != 0) {
            return -3;
        }
        wr32(sector + 76, sum);
    }
    if (dev->write_sector(dev->ctx, base, sector) != 0) {
        return -3;
    }
    return 0;
}

int diskfs_snapshot_list(diskfs_dev *dev, diskfs_snapshot_info *infos,
                         int cap, int *out_count) {
    if (dev == NULL || infos == NULL || out_count == NULL || cap < 0) {
        return -1;
    }
    diskfs_layout lay;
    if (layout_of(dev, &lay) != 0) {
        return -1;
    }
    int n = 0;
    for (int i = 0; i < DISKFS_SNAP_SLOTS; i++) {
        diskfs_snapshot_info info;
        int rc = snap_read_header(dev, i, &lay, &info, NULL);
        if (rc < 0) {
            return -1;
        }
        if (rc == 1 && n < cap) {
            infos[n++] = info;
        }
    }
    *out_count = n;
    return 0;
}

int diskfs_snapshot_restore(diskfs_dev *dev, const char *name) {
    if (dev == NULL || !name_ok(name)) {
        return -1;
    }
    diskfs_layout lay;
    if (layout_of(dev, &lay) != 0) {
        return -2;
    }
    int slot = -1;
    diskfs_snapshot_info info;
    for (int i = 0; i < DISKFS_SNAP_SLOTS; i++) {
        int rc = snap_read_header(dev, i, &lay, &info, NULL);
        if (rc < 0) {
            return -2;
        }
        if (rc == 1 && name_eq(info.name, name)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    uint32_t base = snap_slot_base(dev, slot, &lay);
    /* 復元前に現在の表を退避する。復元ポイントは大きな
     * ファイルを含まないため、大きなファイルは今のまま残す
     * (表の項目もデータも復元しない)。スナップショット時点の
     * 表をそのまま書き戻すと、大きなファイルのチェックサム
     * だけが古くなり、現内容が読めなくなってしまう。 */
    uint8_t *live_table = list_table_scratch;
    if (read_table(dev, live_table, &lay) != 0) {
        return -2;
    }
    /* ファイル表のコピーを読む。 */
    uint8_t *table = table_scratch;
    for (uint32_t i = 0; i < lay.table_sectors; i++) {
        if (dev->read_sector(dev->ctx, base + 1 + i,
                             table + i * DISKFS_SECTOR_SIZE) != 0) {
            return -2;
        }
    }
    /* 表の整合性: 使われている項目の開始セクタがデータ領域内か。 */
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (!e.used) {
            continue;
        }
        if (!snap_entry_valid(dev, &e, &lay)) {
            return -2;
        }
    }
    /* 版2 はチェックサムを照合する。一致しなければ、
     * 書きかけ・破損したスナップショットとして復元しない。
     * 版1 (旧形式) はチェックサムが無いので検証できない
     * まま受け入れる。 */
    {
        uint8_t hdr[DISKFS_SECTOR_SIZE];
        if (dev->read_sector(dev->ctx, base, hdr) != 0) {
            return -2;
        }
        if (rd32(hdr + 12) == SNAP_VERSION) {
            uint32_t sum = 0;
            if (snap_checksum(dev, slot, &lay, table, &sum) != 0 ||
                sum != rd32(hdr + 76)) {
                return -2;
            }
        }
    }
    /* 現在の大きなファイルの項目を、復元する表へ戻す。
     * 同じ名前があればその場で上書きし、無ければ空き枠へ
     * 入れる。空き枠が無ければそのファイルは表から落ちる
     * (データは大きなファイル領域に残る)。 */
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry live;
        entry_from(live_table, i, &live);
        if (!live.used || !live.big) {
            continue;
        }
        int at = -1;
        for (int j = 0; j < lay.max_files; j++) {
            diskfs_entry cur;
            entry_from(table, j, &cur);
            if (cur.used && name_eq(cur.name, live.name)) {
                at = j;
                break;
            }
        }
        if (at < 0) {
            for (int j = 0; j < lay.max_files; j++) {
                diskfs_entry cur;
                entry_from(table, j, &cur);
                if (!cur.used) {
                    at = j;
                    break;
                }
            }
        }
        if (at < 0) {
            continue; /* 空き枠が無い: その大きなファイルは落ちる */
        }
        for (int k = 0; k < 64; k++) {
            table[at * 64 + k] = live_table[i * 64 + k];
        }
    }
    /* 合併後の表を検証し、件数を数え直す。 */
    int count = 0;
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (!e.used) {
            continue;
        }
        if (!snap_entry_valid(dev, &e, &lay)) {
            return -2;
        }
        count++;
    }
    if (write_table(dev, table, &lay) != 0) {
        return -2;
    }
    /* 版4/5: スナップショットはメタ領域を含まないので、復元後は
     * 小さなファイルの時刻を「不明 (0)」に落とす (古い時刻が
     * 残って誤解を招かないように)。大きなファイルの時刻は
     * 今のものを残す (表と一緒に復元しないため)。 */
    if (lay.meta_sector != 0 && meta_load(dev, &lay) == 0) {
        /* 大きなファイルの時刻を退避してから全て 0 にし、
         * 大きなファイルの分だけ戻す。 */
        static uint32_t keep_ct[DISKFS_MAX_FILES];
        static uint32_t keep_mt[DISKFS_MAX_FILES];
        static int keep_big[DISKFS_MAX_FILES];
        for (int j = 0; j < lay.max_files; j++) {
            keep_big[j] = 0;
            keep_ct[j] = 0;
            keep_mt[j] = 0;
            diskfs_entry cur;
            entry_from(table, j, &cur);
            if (!cur.used || !cur.big) {
                continue;
            }
            for (int i = 0; i < lay.max_files; i++) {
                diskfs_entry live;
                entry_from(live_table, i, &live);
                if (live.used && name_eq(live.name, cur.name)) {
                    uint32_t ct = 0, mt = 0;
                    meta_get(i, &ct, &mt);
                    keep_big[j] = 1;
                    keep_ct[j] = ct;
                    keep_mt[j] = mt;
                    break;
                }
            }
        }
        for (size_t i = 0; i < sizeof(meta_scratch); i++) {
            meta_scratch[i] = 0;
        }
        for (int j = 0; j < lay.max_files; j++) {
            if (keep_big[j]) {
                meta_set(j, keep_ct[j], keep_mt[j]);
            }
        }
        (void)meta_flush(dev, &lay);
    }
    /* ファイルデータのコピーを書き戻す。表にあるファイルの分
     * だけ写す (作成時と同じく、大きさ分のセクタだけ)。 */
    uint8_t buf[DISKFS_SECTOR_SIZE];
    uint32_t data_start = sec(dev, lay.data_sector);
    uint32_t slot_data = base + 1 + lay.table_sectors;
    for (int i = 0; i < lay.max_files; i++) {
        diskfs_entry e;
        entry_from(table, i, &e);
        if (!e.used || e.size == 0 || e.big) {
            continue;
        }
        uint32_t sectors =
            (e.size + DISKFS_SECTOR_SIZE - 1) / DISKFS_SECTOR_SIZE;
        uint32_t off = e.start_sector - data_start;
        for (uint32_t s = 0; s < sectors; s++) {
            if (dev->read_sector(dev->ctx, slot_data + off + s, buf) != 0) {
                return -2;
            }
            if (dev->write_sector(dev->ctx, e.start_sector + s, buf) != 0) {
                return -2;
            }
        }
    }
    /* スーパーブロックのファイル数を数え直す。 */
    uint8_t sector[DISKFS_SECTOR_SIZE];
    if (dev->read_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -2;
    }
    wr32(sector + 12, (uint32_t)count);
    if (dev->write_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -2;
    }
    return 0;
}

int diskfs_snapshot_delete(diskfs_dev *dev, const char *name) {
    if (dev == NULL || !name_ok(name)) {
        return -1;
    }
    diskfs_layout lay;
    if (layout_of(dev, &lay) != 0) {
        return -1;
    }
    for (int i = 0; i < DISKFS_SNAP_SLOTS; i++) {
        diskfs_snapshot_info info;
        int rc = snap_read_header(dev, i, &lay, &info, NULL);
        if (rc < 0) {
            return -1;
        }
        if (rc == 1 && name_eq(info.name, name)) {
            uint8_t sector[DISKFS_SECTOR_SIZE];
            for (int j = 0; j < DISKFS_SECTOR_SIZE; j++) {
                sector[j] = 0;
            }
            if (dev->write_sector(dev->ctx, snap_slot_base(dev, i, &lay),
                                  sector) != 0) {
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

/* ---- 回復モードの要求フラグ ---- */

int diskfs_get_recovery_flag(diskfs_dev *dev) {
    if (dev == NULL || dev->read_sector == NULL) {
        return -1;
    }
    uint8_t sector[DISKFS_SECTOR_SIZE];
    if (dev->read_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -1;
    }
    return sector[28] != 0 ? 1 : 0;
}

int diskfs_set_recovery_flag(diskfs_dev *dev, int value) {
    if (dev == NULL || dev->read_sector == NULL ||
        dev->write_sector == NULL) {
        return -1;
    }
    uint8_t sector[DISKFS_SECTOR_SIZE];
    if (dev->read_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -1;
    }
    sector[28] = value ? 1 : 0;
    if (dev->write_sector(dev->ctx, sec(dev, DISKFS_SB_SECTOR), sector) !=
        0) {
        return -1;
    }
    return 0;
}
