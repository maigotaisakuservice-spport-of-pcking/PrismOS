/* PrismOS — ARM 用 Flattened Device Tree (FDT) の最小パーサ
 *
 * QEMU の -kernel 起動では x0 に DTB のアドレスが入る。
 * ここでは /memory ノードの reg プロパティだけを読む。
 * 構造ブロックを先頭から辿り、セルの数はルートの
 * #address-cells / #size-cells (無ければ 2 / 1) に従う。
 */
#ifndef PRISM_ARM_FDT_H
#define PRISM_ARM_FDT_H

#include <stdint.h>

/* DTB からメモリ範囲を読む。成功したら 0 を返し、
 * *out_base と *out_size に入れる。失敗したら -1。 */
int arm_fdt_memory(uint64_t dtb_addr, uint64_t *out_base,
                   uint64_t *out_size);

/* DTB の totalsize (ヘッダの 2 番目のフィールド) を返す。
 * DTB が無効なら 0。PMM で DTB の領域を予約するために使う。 */
uint32_t arm_fdt_total_size(uint64_t dtb_addr);

#endif /* PRISM_ARM_FDT_H */
