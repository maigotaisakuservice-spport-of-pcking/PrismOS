/* PrismOS — FAT16 の読み書き (起動パーティション用)
 *
 * カーネル更新で使う、最小限の FAT16 ライタ。起動パーティション
 * (P1) にある /boot/prismos.elf を、新しいカーネルと置き換える
 * ためのもので、汎用のファイルシステムではない。
 *
 * できること:
 *   * MBR の先頭パーティション (種別 0x06 / 0x0E) を探し、
 *     その BPB を読む。MBR が無ければ先頭セクタ自体を BPB
 *     として扱う (区切りの無い FAT イメージ用)。
 *   * FAT16 だけを扱う。クラスタ数が 4085 未満 (FAT12) や
 *     65525 以上 (FAT32) のものは -2 で断る。古い 8MB の
 *     起動パーティションは mtools が FAT12 にするため、
 *     そこで更新を試みると「FAT16 ではない」と正直に失敗する。
 *   * ファイルの書き込み (クラスタの割り当て、鎖の書き込み、
 *     2 つの FAT コピー両方の更新、ディレクトリエントリの
 *     作成・更新)。書き込みはチャンク単位で、途中で止めても
 *     古いファイルは残る (呼び出し側が abort で片付ける)。
 *     FAT セクタへの書き出しはまとめて行う: 割り当て中は
 *     キャッシュだけを汚し、FAT セクタが切り替わる時と
 *     書き込みの完了・中止・削除・入れ替えの時にだけ両コピー
 *     へ書き出す (1 セクタごとの書き出しだと遅いため)。
 *   * 2 つのエントリのクラスタ番号とサイズを、同じセクタへの
 *     1 回の書き込みで入れ替える (カーネルの原子的な置き換え用)。
 *     入れ替えの前後どちらで止まっても、/boot/prismos.elf は
 *     新旧どちらかの完全なカーネルを指す。
 *
 * ハードウェアには触らない。セクタの読み書きは呼び出し側が
 * 渡す fat16_dev のコールバック経由で、ホストテストでは
 * メモリ上のイメージ相手に同じソースを動かせる。
 * 浮動小数点は使わない。
 */
#ifndef PRISM_FAT16_H
#define PRISM_FAT16_H

#include <stddef.h>
#include <stdint.h>

/* セクタの読み書き (呼び出し側が用意する)。
 * lba は絶対 LBA。buf は 512 バイト以上あること。
 * 成功したら 0、失敗したら -1 を返す。 */
typedef struct {
    void *ctx;
    int (*read_sector)(void *ctx, uint32_t lba, uint8_t *buf);
    int (*write_sector)(void *ctx, uint32_t lba, const uint8_t *buf);
    uint32_t total_sectors; /* デバイスの総セクタ数 (0 = 不明) */
} fat16_dev;

/* 開いた FAT16 パーティションの配置。fat_cache は FAT の
 * 1 セクタ分の読み取りキャッシュ (呼び出し側は中身を触らない)。 */
typedef struct {
    fat16_dev dev;
    uint32_t part_lba; /* パーティションの開始 LBA (絶対) */
    uint32_t total_sectors; /* パーティションのセクタ数 */
    uint16_t bytes_per_sector; /* 常に 512 */
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;
    uint32_t fat_sectors; /* FAT 1 個あたりのセクタ数 */
    uint32_t fat_start; /* 最初の FAT の開始 LBA (絶対) */
    uint32_t root_start; /* ルートディレクトリの開始 LBA (絶対) */
    uint32_t data_start; /* クラスタ 2 の開始 LBA (絶対) */
    uint32_t cluster_count;
    uint32_t alloc_hint; /* 次に空きを探すクラスタ番号 */
    uint8_t fat_cache[512];
    uint32_t fat_cache_sector; /* fat_cache の中身の FAT 内オフセット (セクタ単位) */
    int fat_cache_valid;
    int fat_cache_dirty; /* fat_cache がデバイスより新しい */
} fat16_fs;

/* 書き込み中のファイル。fat16_write_begin() が作り、
 * fat16_write_chunk() / finish() / abort() が使う。 */
typedef struct {
    fat16_fs *fs;
    uint32_t dir_sector; /* エントリのあるセクタ (絶対 LBA) */
    uint16_t dir_index; /* そのセクタ内のエントリ番号 (0..15) */
    uint16_t first_cluster;
    uint32_t size; /* 書き込んだバイト数 */
    uint32_t cur_cluster; /* 今埋めているクラスタ (0 = まだ無い) */
    uint32_t sector_lba; /* 今埋めているセクタ (0 = まだ無い) */
    uint16_t sector_in_cluster; /* クラスタ内のセクタ番号 */
    uint8_t sector_buf[512];
    uint16_t sector_used; /* sector_buf に溜めたバイト数 */
    int active;
} fat16_writer;

/* パーティションを開く。0 = 成功、-1 = 起動パーティションが
 * 無い・BPB が壊れている・読み書きに失敗した、
 * -2 = FAT16 ではない (FAT12 / FAT32)。 */
int fat16_open(const fat16_dev *dev, fat16_fs *fs);

/* 空き容量 (バイト) を数える。成功したら 0。 */
int fat16_free_bytes(const fat16_fs *fs, uint64_t *out);

/* ファイルの大きさ (バイト) を調べる。無ければ -1。 */
int fat16_file_size(const fat16_fs *fs, const char *path,
                    uint32_t *size_out);

/* ファイルを消す (鎖を解放し、エントリを 0xE5 にする)。
 * 無ければ -1。 */
int fat16_delete(fat16_fs *fs, const char *path);

/* ファイルの読み込み (ホストテストと書き込み後の確認用)。
 * 大きさが cap を超えると -2、無ければ -1。
 * *len_out に読んだバイト数を入れる。 */
int fat16_read_file(const fat16_fs *fs, const char *path, uint8_t *buf,
                    size_t cap, size_t *len_out);

/* 新しいファイルの書き込みを始める。同名のファイルがあれば
 * 先に消す (鎖を解放する)。エントリを作り、w を初期化する。
 * 成功したら 0。 */
int fat16_write_begin(fat16_fs *fs, const char *path, fat16_writer *w);

/* チャンクを書き込む。途中で空きが無くなったら -1
 * (その場合は呼び出し側が fat16_write_abort() で片付ける)。 */
int fat16_write_chunk(fat16_writer *w, const uint8_t *data, size_t len);

/* 書き込みを終える。半端なセクタを書き出し、エントリに
 * クラスタ番号と大きさを入れる。成功したら 0。 */
int fat16_write_finish(fat16_writer *w);

/* 書き込みをやめる。割り当てた鎖を解放し、エントリを消す。
 * 失敗しても旧ファイルには触れない。 */
int fat16_write_abort(fat16_writer *w);

/* 2 つのファイルのエントリのクラスタ番号と大きさを入れ替える。
 * 両方のエントリが同じセクタにある場合だけ、1 回のセクタ書き込み
 * で行う。0 = 成功、-1 = ファイルが無い・読み書きに失敗した、
 * -2 = 2 つのエントリが違うセクタにある (入れ替えをしない)。 */
int fat16_swap_entries(fat16_fs *fs, const char *path_a,
                       const char *path_b);

#endif /* PRISM_FAT16_H */
