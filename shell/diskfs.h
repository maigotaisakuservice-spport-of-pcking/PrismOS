/* PrismOS — ディスク上のファイル形式 (diskfs)
 *
 * ディスクの配置 (1 セクタ = 512 バイト):
 *   セクタ 0: MBR。0x55AA の署名と、1 個のパーティション
 *             (種別 0x83、開始 LBA 1、残り全部)。
 *   セクタ 1: スーパーブロック。魔法 "PRISMFS2"、版 2/3/4/5、
 *             ファイル数、データ開始セクタ、1 ファイルの
 *             セクタ数、総セクタ数。
 *   形式は版で管理する。版4 (従来): ファイル表は
 *   セクタ 2-17 (128 件 x 64 バイト)、メタ領域 (作成/
 *   更新時刻) がセクタ 18-19、データはセクタ 20〜。
 *   版3: 表はセクタ 2-17、データは 18〜 (メタ無し)。
 *   版2 (従来): 表はセクタ 2-9 (64 件)、データは 10〜。
 *   全版で各項目の +60 に平文の FNV-1a チェックサムを
 *   保存する (旧版で書かれた項目は 0 = 照合しない)。
 *   版2/3/4 ではファイル i は 32 セクタ (16384 バイト) の
 *   固定枠を持ち、断片化は無い。版はスーパーブロックに
 *   記録され、版2/3/4 のディスクもそのまま読み書きできる。
 *   下の DISKFS_MAX_FILES 等は版3 (最大) の値で、
 *   バッファ確保用。実際の配置は版から決まる。
 *
 *   版5 (大きなファイル): 表・メタ・データ・復元ポイントの
 *   配置は版4 と同じ。16KB 以下のファイルは従来通り固定枠
 *   に置く。16KB を超えるファイルは、復元ポイント領域の
 *   直後から始まる「大きなファイル領域」(セクタ 20576〜、
 *   パーティションの末尾まで) に可変長の連続領域として
 *   置く (first-fit)。表の +42 バイトが 1 の項目が大きな
 *   ファイル。大きなファイルは復元ポイントに写さない
 *   (表の項目だけ写し、データは大きなファイル領域に残る。
 *   復元しても大きなファイルは消えない)。
 *   版4 のディスクは diskfs_migrate_to_v5() で版5 へ移行
 *   できる (スーパーブロックの版番号だけを変える。データの
 *   移動は無く、版4 の復元ポイントもそのまま読める)。
 *
 * このファイルはハードウェアに触らない。セクタの読み書きは
 * 呼び出し側が渡す関数に任せるので、カーネル (ATA) でも
 * ホストテスト (メモリ上の仮想ディスク) でも同じソースを
 * 使える。ファイル名の規則は VFS と同じ。
 */
#ifndef PRISM_DISKFS_H
#define PRISM_DISKFS_H

#include <stddef.h>
#include <stdint.h>

#define DISKFS_SECTOR_SIZE 512
#define DISKFS_MAX_FILES 128
/* 1 ファイルの上限。版5 で 4MB まで (1280x800x3 の BMP
 * 原寸 3072054 バイトを含む)。版2/3/4 のディスクでは
 * 従来通り 16KB まで (下の SMALL 参照)。 */
#define DISKFS_MAX_FILE_SIZE 4194304
/* 小さなファイルの上限 (固定枠 32 セクタ)。アプリの作業
 * バッファや更新ファイルなど、大きなファイルを扱わない
 * 場所の大きさ。 */
#define DISKFS_SMALL_FILE_SIZE 16384
#define DISKFS_MAX_NAME 31
#define DISKFS_MBR_SECTOR 0
#define DISKFS_SB_SECTOR 1
#define DISKFS_TABLE_SECTOR 2
#define DISKFS_TABLE_SECTORS 16
/* 現行形式 (版4/5) の値。旧版の配置は diskfs.c の
 * LAYOUT_V2/V3 が保持する。 */
#define DISKFS_DATA_SECTOR 20
#define DISKFS_SECTORS_PER_FILE 32
#define DISKFS_PART_TYPE 0x83
/* 版5 の大きなファイル領域の開始 (相対セクタ)。
 * 版4 の復元ポイント領域の直後 (4116 + 4*4115 = 20576)。 */
#define DISKFS_BIG_SECTOR 20576

/* 暗号化のフック (NULL なら暗号化しない)。
 * encrypt: 平文 in (len バイト) を暗号文 out にし、使った
 * 初期ベクトル (16 バイト) を iv_out に入れる。成功したら 0。
 * decrypt: 暗号文 in を iv で平文 out にする。成功したら 0。
 * どちらも長さは変わらない (CTR モードを想定)。 */
typedef struct {
    void *ctx;
    int (*encrypt)(void *ctx, uint8_t *iv_out, const uint8_t *in,
                   uint8_t *out, size_t len);
    int (*decrypt)(void *ctx, const uint8_t *iv, const uint8_t *in,
                   uint8_t *out, size_t len);
} diskfs_crypto;

typedef struct {
    void *ctx;
    /* 1 セクタ読む。成功したら 0。buf は 512 バイト以上。 */
    int (*read_sector)(void *ctx, uint32_t lba, uint8_t *buf);
    /* 1 セクタ書く。成功したら 0。buf は 512 バイト以上。 */
    int (*write_sector)(void *ctx, uint32_t lba, const uint8_t *buf);
    uint32_t total_sectors;
    /* ファイルシステムの開始 LBA。0 = ディスク全体 (従来の配置)。
     * 0 より大きい場合は、MBR のパーティションの中に置く:
     * スーパーブロックは base_lba + 1、表は base_lba + 2〜5、
     * データは base_lba + 6 から。total_sectors はその
     * パーティションの大きさ (セクタ数) にすること。 */
    uint32_t base_lba;
    /* 暗号化のフック。NULL なら平文で読み書きする。 */
    const diskfs_crypto *crypto;
} diskfs_dev;

typedef struct {
    char name[DISKFS_MAX_NAME + 1];
    uint32_t size;
    uint32_t start_sector;
    int used;
    /* 大きなファイルかどうか (版5 のみ。表の +42 バイト)。
     * 1 のとき start_sector は大きなファイル領域を指す。 */
    int big;
    /* 暗号化されているかどうか。暗号化されている場合、iv に
     * そのファイルの初期ベクトル (16 バイト) が入る。 */
    int encrypted;
    uint8_t iv[16];
    /* 平文の FNV-1a チェックサム (表の +60 に保存)。
     * 0 = 無し (旧形式で書かれた項目)。読み取り時に
     * 照合し、不一致は -6 を返す。 */
    uint32_t checksum;
    /* 作成/更新時刻 (Unix 秒、版4 のメタ領域から)。
     * 版2/3 や不明な場合は 0。 */
    uint32_t ctime;
    uint32_t mtime;
} diskfs_entry;

/* 時刻の源を設定する (Unix 秒を返す関数)。カーネルは
 * RTC から設定する。未設定の間は時刻 0 で記録される。 */
void diskfs_set_time_source(uint32_t (*fn)(void));

/* ディスクを初期化する (MBR + スーパーブロック + 空の表)。
 * 既存のファイルは消える。成功したら 0。
 * base_lba が 0 より大きい場合は MBR を書かない
 * (パーティションの中だけを初期化する)。 */
int diskfs_format(diskfs_dev *dev);

/* MBR のパーティション表からデータ用パーティションを探す。
 * 見つかればその開始 LBA を返し、*out_size に大きさを入れる。
 * 従来の配置 (開始 LBA 1) や、見つからない・署名が無い場合は
 * 0 を返す (out_size は書かない)。 */
uint32_t diskfs_find_data_partition(const uint8_t *mbr,
                                    uint32_t *out_size);

/* 形式が正しいディスクかどうか。1 = 正しい、0 = 違う、-1 = 読めない。 */
int diskfs_is_formatted(diskfs_dev *dev);

/* ファイル表を読む。entries は DISKFS_MAX_FILES 件分あること。
 * 成功したら 0、*out_count に件数を入れる。 */
int diskfs_list(diskfs_dev *dev, diskfs_entry *entries, int *out_count);

/* ファイルを読む。buf は cap バイト以上あること。
 * 成功したら 0、*out_len に大きさを入れる。無い場合は -1。
 * 暗号化されたファイルは、crypto が設定されていれば復号して
 * 返し、設定されていなければ -1 を返す。 */
int diskfs_read_file(diskfs_dev *dev, const char *name, uint8_t *buf,
                     size_t cap, size_t *out_len);

/* ファイルを復号せずに読む (暗号文のまま)。ログイン前に
 * ディスクから読み込んで、ログイン後に復号するときに使う。
 * 成功したら 0、*out_len に大きさを入れる。無い場合は -1。 */
int diskfs_read_file_raw(diskfs_dev *dev, const char *name, uint8_t *buf,
                         size_t cap, size_t *out_len);

/* ファイルを書く (新規作成または上書き)。
 * 成功したら 0。-1: 大きすぎる (版5 の上限 4MB を超える、
 * 版2/3/4 では 16KB を超える、または大きなファイル領域に
 * 空きが無い)、-2: 件数上限、-3: 名前が不正、
 * -4: 書き込み失敗。 */
int diskfs_write_file(diskfs_dev *dev, const char *name,
                      const uint8_t *data, size_t len);

/* ファイルを消す。成功したら 0、無ければ -1。 */
int diskfs_delete_file(diskfs_dev *dev, const char *name);

/* 版4 のディスクを版5 へ移行する。スーパーブロックの版番号
 * だけを変える (データの移動は無く、版4 の復元ポイントも
 * そのまま読める)。成功したら 0。-1: 読めない、-2: 版4 では
 * 無い、-3: 表が壊れている。版2/3 は移行しない (読み書き
 * だけの互換を保つ)。 */
int diskfs_migrate_to_v5(diskfs_dev *dev);

/* ---- 復元ポイント (スナップショット) ----
 *
 * ディスク上の配置: データ領域の直後から 4 スロット。
 * 版3 では相対セクタ 4114 から 4113 セクタずつ (ヘッダ 1 +
 * 表のコピー 16 + データのコピー 4096 = 128 ファイル x 32
 * セクタ)、版2 では 2058 から 2057 セクタずつ。中身は
 * 暗号文のまま丸ごと写すため、暗号化されたファイルも
 * 鍵なしで保存・復元できる。スナップショットを作るには、
 * パーティションがその版の配置で全スロット分あること
 * (版3: 4114 + 4*4113 = 20566 セクタ以上)。
 *
 * 版4/5 では相対セクタ 4116 から 4115 セクタずつ (ヘッダ 1 +
 * 表のコピー 16 + メタ 2 + データのコピー 4096)。
 * 版5 の大きなファイルは復元ポイントに写さない (表の項目
 * だけ写す。復元しても大きなファイルのデータは残る)。
 *
 * ヘッダ (512 バイト): 魔法 "PRISMSNAP" (0-8)、版 u32 (12)、
 * 連番 u32 (16)、名前 32 バイト (20-51)、時刻文字列 20 バイト
 * (52-71、"YYYY-MM-DD HH:MM:SS")、ファイル数 u32 (72)、
 * チェックサム u32 (76、版2のみ: 表とデータの FNV-1a)。
 * 魔法と版が合わなければ、そのスロットは空き。
 *
 * 作成は「旧ヘッダを無効化 → 表とデータをコピー →
 * チェックサム付きヘッダを最後に書く」の順に行うため、
 * 途中で止まっても中身の混ざったスナップショットが
 * 有効として残ることはない。復元は版2のチェックサムを
 * 照合し、不一致なら書き戻さない。版1 (チェックサム
 * 無し) も読み取り・復元は可能。
 */
#define DISKFS_SNAP_BASE 4116
#define DISKFS_SNAP_SLOTS 4
#define DISKFS_SNAP_SLOT_SECTORS 4115
#define DISKFS_SNAP_NAME_MAX 31

typedef struct {
    char name[DISKFS_SNAP_NAME_MAX + 1];
    char timestamp[20]; /* "YYYY-MM-DD HH:MM:SS" */
    uint32_t seq;
    uint32_t file_count;
} diskfs_snapshot_info;

/* 復元ポイントを作る。空きスロットが無ければ、いちばん古い
 * ものを上書きする。成功したら 0。-1: 名前や時刻が不正、
 * -2: 領域が足りない、-3: 読み書きに失敗した。 */
int diskfs_snapshot_create(diskfs_dev *dev, const char *name,
                           const char *timestamp);

/* 有効な復元ポイントの一覧を infos (cap 件分) に入れる。
 * 成功したら 0、*out_count に件数を入れる。 */
int diskfs_snapshot_list(diskfs_dev *dev, diskfs_snapshot_info *infos,
                         int cap, int *out_count);

/* 復元ポイントをディスクのファイル領域に書き戻す。
 * 成功したら 0。-1: その名前が無い、-2: 内容が壊れている。 */
int diskfs_snapshot_restore(diskfs_dev *dev, const char *name);

/* 復元ポイントを消す。成功したら 0、無ければ -1。 */
int diskfs_snapshot_delete(diskfs_dev *dev, const char *name);

/* ---- 回復モードの要求フラグ ----
 *
 * スーパーブロックの予備バイト (オフセット 28) に置く。
 * 1 = 次の起動で回復モードに入る。diskfs_format() は
 * スーパーブロックを 0 で書き直すため、初期化で消える。 */
int diskfs_get_recovery_flag(diskfs_dev *dev);
int diskfs_set_recovery_flag(diskfs_dev *dev, int value);

#endif /* PRISM_DISKFS_H */
