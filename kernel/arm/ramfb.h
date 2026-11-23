/* PrismOS — ARM 用 ramfb フレームバッファ (QEMU virt の fw_cfg 経由)
 *
 * QEMU virt には表示用の実機 GPU が無い代わりに、fw_cfg の
 * ファイル "etc/ramfb" に 28 バイトの設定を DMA で書くと、
 * RAM の一部がフレームバッファになる (ramfb)。設定は
 * すべてビッグエンディアン。
 *   addr (8B) / fourcc (4B) / flags (4B) / width (4B) /
 *   height (4B) / stride (4B)
 * fourcc は XR24 (XRGB8888) を使う。fb.c の fb_rgb と同じ並び。
 * fw_cfg の MMIO は 0x09020000: データ (0x00、8 バイト)・
 * 選択子 (0x08、2 バイト)・DMA (0x10)。データは 64 ビット
 * 単位で読む。ビッグエンディアン。
 * 書き込みは DMA だけ (データレジスタへの書き込みは QEMU 2.4
 * 以降は捨てられる。hw/nvram/fw_cfg.c の fw_cfg_write は空関数)。
 */
#ifndef PRISM_ARM_RAMFB_H
#define PRISM_ARM_RAMFB_H

#include <stdint.h>

/* フレームバッファを確保して ramfb に登録する。
 * 成功したらフレームバッファの物理アドレス、失敗したら 0。 */
uint64_t arm_ramfb_init(uint32_t width, uint32_t height);

#endif /* PRISM_ARM_RAMFB_H */
