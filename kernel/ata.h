/* PrismOS — ATA ディスクドライバ (PIO モード)
 *
 * QEMU の -drive で付けるハードディスク (プライマリ・マスタ)
 * を、PIO (programmed I/O) で 1 セクタ (512 バイト) ずつ
 * 読み書きする。割り込みは使わず、状態をポーリングする。
 * 対応するのは LBA28 の ATA ディスクだけ。ATAPI (CD) や
 * 存在しないディスクは ata_present() が 0 を返す。
 * 書き込みの後は FLUSH CACHE を送る。
 */
#ifndef PRISM_ATA_H
#define PRISM_ATA_H

#include <stddef.h>
#include <stdint.h>

#define ATA_SECTOR_SIZE 512

/* ディスクを探して初期化する。見つかれば 1、無ければ 0 を返す。
 * 何度呼んでもよい。 */
int ata_init(void);

/* ディスクが見つかったかどうか。ata_init() の後で有効。 */
int ata_present(void);

/* ディスクの総セクタ数。無い場合は 0。 */
uint32_t ata_total_sectors(void);

/* IDENTIFY で読んだモデル名 (NUL 終端、最大 40 文字)。
 * 無い場合は空文字列。 */
const char *ata_model(void);

/* 1 セクタ読む。成功したら 0、失敗したら -1。
 * buf は ATA_SECTOR_SIZE バイト以上あること。 */
int ata_read_sector(uint32_t lba, uint8_t *buf);

/* 1 セクタ書く。成功したら 0、失敗したら -1。
 * buf は ATA_SECTOR_SIZE バイト以上あること。 */
int ata_write_sector(uint32_t lba, const uint8_t *buf);
int ata_flush_disk(void);

#endif /* PRISM_ATA_H */
