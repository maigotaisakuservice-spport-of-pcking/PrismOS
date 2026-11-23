/* PrismOS — virtio-blk ドライバ (従来型 virtio PCI)
 *
 * QEMU の virtio ブロックデバイス (PCI 1AF4:1001) から
 * ディスクを読み書きする。割り込みは使わず、使ったリングの
 * 進みをポーリングする。1 回の要求は 1 セクタ (512 バイト)。
 *
 * 従来型 (legacy) の並び: I/O ポートの BAR0 で機能・
 * キューの設定をし、記述子・利用可能リング・使用済みリング
 * を PMM から確保した物理ページに置く。デバイスには
 * 物理アドレスを渡し、カーネル側は HHDM 経由で触る。
 *
 * 純粋な組み立て関数 (virtio_blk_build_header /
 * virtio_queue_layout / virtio_desc) はハードウェアに
 * 触らないので、ホスト側のユニットテストでも同じソースを
 * 動かせる。
 */
#ifndef PRISM_VIRTIO_BLK_H
#define PRISM_VIRTIO_BLK_H

#include <stddef.h>
#include <stdint.h>

#define VIRTIO_BLK_SECTOR_SIZE 512

/* virtio-blk デバイスを探して初期化する。HHDM のずれを渡す。
 * 見つかれば 1、無ければ 0 を返す。何度呼んでもよい。 */
int virtio_blk_init(uint64_t hhdm_offset);

/* デバイスが見つかったかどうか。virtio_blk_init() の後で有効。 */
int virtio_blk_present(void);

/* デバイスの総セクタ数。無い場合は 0。 */
uint64_t virtio_blk_total_sectors(void);

/* 1 セクタ読む。成功したら 0、失敗したら -1。
 * buf は VIRTIO_BLK_SECTOR_SIZE バイト以上あること。 */
int virtio_blk_read_sector(uint64_t lba, uint8_t *buf);

/* 1 セクタ書く。成功したら 0、失敗したら -1。
 * buf は VIRTIO_BLK_SECTOR_SIZE バイト以上あること。 */
int virtio_blk_write_sector(uint64_t lba, const uint8_t *buf);

/* ---- 純粋関数 (ホストテスト用) ---- */

/* virtio-blk 要求ヘッダ (16 バイト) を作る。type は 0=読み、
 * 1=書き。sector は読み書きするセクタ番号。hdr は 16 バイト
 * 以上あること。 */
void virtio_blk_build_header(uint8_t *hdr, uint32_t type, uint64_t sector);

/* 従来型キューの配置を出す。qsize はキューの項目数。
 * desc_bytes は記述子表の大きさ、avail_off は利用可能リング
 * の先頭、used_off は使用済みリングの先頭 (ページ境界に
 * そろえた値)、total はキュー全体の大きさ (バイト)。 */
void virtio_queue_layout(uint16_t qsize, uint32_t *desc_bytes,
                         uint32_t *avail_off, uint32_t *used_off,
                         uint32_t *total);

/* 記述子 (16 バイト) を作る。addr はバッファの物理アドレス、
 * len は長さ、flags は VIRTIO_DESC_F_NEXT (1) / WRITE (2) の
 * 組み合わせ、next は次の記述子の番号。desc は 16 バイト以上
 * あること。 */
void virtio_desc(uint8_t *desc, uint64_t addr, uint32_t len,
                 uint16_t flags, uint16_t next);

#endif /* PRISM_VIRTIO_BLK_H */
