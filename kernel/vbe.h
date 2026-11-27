/* PrismOS — Bochs VBE (dispi) による解像度変更。
 *
 * 標準 VGA (PCI 1234:1111) の dispi レジスタ (I/O
 * 0x1CE/0x1CF) で画面モードを実行時に切り替える。
 * リニアフレームバッファは VGA の BAR0 で、モード変更
 * 後は自分でマップしたアドレスを fb 層へ渡す。
 */
#ifndef PRISM_VBE_H
#define PRISM_VBE_H

#include <stdint.h>

/* HHDM のずれを覚える (main が起動時に呼ぶ)。 */
void vbe_set_hhdm(uint64_t hhdm_offset);

/* Bochs VGA があるか。 */
int vbe_present(void);

/* 解像度を変更する。成功したら 0 (fb 層は切替済み)、
 * 失敗したら -1。bpp は 32 固定。 */
int vbe_set_mode(uint32_t width, uint32_t height);

#endif
