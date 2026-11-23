/* PrismOS — SD ホストコントローラ (SDHCI) ドライバ。
 *
 * PCI の SDHCI (クラス 08/05) を PIO 転送で駆動する。
 * カード初期化 (CMD0/8/55+ACMD41/2/3/9/7) を行い、
 * CMD17/CMD24 の単一ブロック読み書きをブロック層の
 * 追加デバイスとして提供する。DMA は使わない。
 */
#ifndef PRISM_SDHCI_H
#define PRISM_SDHCI_H

#include <stdint.h>

/* 見つかれば初期化して 1、無ければ 0 を返す。 */
int sdhci_init(uint64_t hhdm_offset);
int sdhci_present(void);
uint64_t sdhci_sectors(void);
int sdhci_read_sector(uint64_t lba, void *buf);
int sdhci_write_sector(uint64_t lba, const void *buf);

#endif
