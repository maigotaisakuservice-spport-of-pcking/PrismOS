/* PrismOS — ブロックデバイス層
 *
 * システムディスク (機器 0) を 1 台選び、ATA PIO /
 * AHCI / virtio-blk の違いを隠す。探す順番は ATA PIO、
 * AHCI、virtio-blk。加えて USB マスストレージを
 * 機器 1 として登録できる (複数ディスク対応)。
 * 従来の構成 (ATA ディスク) では ATA PIO が選ばれ、
 * 既存の動きは変わらない。q35 + AHCI の構成では AHCI、
 * virtio だけの構成では virtio-blk が選ばれる。
 *
 * persist.c はこの層だけを使い、ATA を直接呼ばない。
 */
#ifndef PRISM_BLOCK_H
#define PRISM_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512

/* HHDM のずれを覚える。persist_init() より前に呼ぶこと。 */
void block_set_hhdm(uint64_t hhdm_offset);

/* ディスクを探して初期化する。見つかれば 1、無ければ 0 を返す。
 * 何度呼んでもよい。PMM が使える状態で呼ぶこと。 */
int block_init(void);

/* ディスクが見つかったかどうか。block_init() の後で有効。 */
int block_present(void);

/* 選ばれた種別: "ata" / "ahci" / "virtio" / "none"。 */
const char *block_kind(void);

/* ディスクの総セクタ数。無い場合は 0。 */
uint64_t block_total_sectors(void);

/* モデル名 (NUL 終端)。無い場合は空文字列。virtio-blk は
 * 固定の名前を返す。 */
const char *block_model(void);

/* 1 セクタ読む。成功したら 0、失敗したら -1。
 * buf は BLOCK_SECTOR_SIZE バイト以上あること。 */
int block_read_sector(uint64_t lba, uint8_t *buf);

/* 1 セクタ書く。成功したら 0、失敗したら -1。
 * buf は BLOCK_SECTOR_SIZE バイト以上あること。 */
int block_write_sector(uint64_t lba, const uint8_t *buf);

/* ---- 複数ディスク対応 ----
 * 機器 0 = システムディスク (上の関数の対象)、
 * 機器 1 = USB マスストレージ (UHCI 経由、あれば)、
 * 機器 2 = NVMe (あれば。システムディスクが NVMe の
 * 場合は機器 0 と同じ実体)。
 * block_register_usb は UHCI 列挙後に main が呼ぶ。 */
void block_register_usb(uint64_t sectors);
/* EHCI 経由の USB ストレージ (機器 4)。登録は main が
 * EHCI 列挙の後に行う。 */
void block_register_ehci(uint64_t sectors);
/* xHCI 経由の USB ストレージ (機器 5)。登録は main が
 * xHCI 列挙の後に行う。 */
void block_register_xhci(uint64_t sectors);
int block_dev_count(void);
const char *block_dev_kind(int dev);
uint64_t block_dev_sectors(int dev);
int block_read_sector_dev(int dev, uint64_t lba, uint8_t *buf);
int block_write_sector_dev(int dev, uint64_t lba,
                           const uint8_t *buf);

#endif /* PRISM_BLOCK_H */
