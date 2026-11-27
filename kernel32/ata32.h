/* PrismOS — 32bit 版 ATA PIO ドライバ。
 *
 * プライマリ IDE (0x1F0 系) の LBA28 PIO 読み書き。
 * 永続化は shell/diskfs.c (64bit 版・ARM 版と共通の
 * 形式) をこのドライバの上に載せて実現する
 * (main32.c の fs32_* を参照)。
 */
#ifndef PRISM_ATA32_H
#define PRISM_ATA32_H

#include <stdint.h>

/* ドライブがあれば 1。sectors に総セクタ数が入る。 */
int ata32_init(uint32_t *sectors);
int ata32_read(uint32_t lba, void *buf);
int ata32_write(uint32_t lba, const void *buf);

#endif
