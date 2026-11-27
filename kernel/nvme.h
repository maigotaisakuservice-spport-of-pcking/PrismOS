/* PrismOS — NVMe ドライバ (PCIe、ポーリング)
 *
 * PCI クラス 01/08/02 (Mass Storage / NVM / NVMe) を探し、
 * BAR0 のレジスタでコントローラを初期化する。管理キューで
 * Identify と IO キュー作成を行い、IO キューで読み書きする。
 * データ転送は PRP1 の 1 ページ (512 バイト × 最大 8) まで。
 * 割り込みは使わず、完了キューのフェーズビットをポーリング
 * する。LBA サイズ 512 バイトの名前空間だけを扱う。
 */
#ifndef PRISM_NVME_H
#define PRISM_NVME_H

#include <stdint.h>

/* 見つけて初期化できたら 1、無ければ 0 (AHCI と同じ約束)。 */
int nvme_init(uint64_t hhdm_offset);
int nvme_present(void);
uint64_t nvme_total_sectors(void);
const char *nvme_model(void);
int nvme_read_sector(uint64_t lba, uint8_t *buf);
int nvme_write_sector(uint64_t lba, const uint8_t *buf);

#endif /* PRISM_NVME_H */
