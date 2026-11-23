/* PrismOS — ARM 用 virtio ブロックデバイス (virtio-mmio)
 *
 * QEMU virt 構成の virtio-blk-device を 512 バイトセクタで
 * 読み書きする。要求は ヘッダ (16 バイト、デバイスが読む) →
 * データ (512 バイト) → ステータス (1 バイト、デバイスが
 * 書く) の 3 記述子チェーン。ポーリングで完了を待つ。
 */
#ifndef PRISM_ARM_VIRTIO_BLK_H
#define PRISM_ARM_VIRTIO_BLK_H

#include <stdint.h>

/* デバイスを探して初期化する。成功したら 0。 */
int arm_blk_init(void);
/* 付いていれば 1。 */
int arm_blk_present(void);
/* 総セクタ数 (512 バイト単位)。 */
uint64_t arm_blk_sectors(void);
/* 1 セクタ読む / 書く。成功したら 0。 */
int arm_blk_read(uint64_t lba, uint8_t *buf);
int arm_blk_write(uint64_t lba, const uint8_t *buf);

#endif /* PRISM_ARM_VIRTIO_BLK_H */
