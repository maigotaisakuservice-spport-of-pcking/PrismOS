/* PrismOS — ARM 用ページ表 (MMU、恒等写像)
 *
 * 4KB 粒度、48 ビット仮想空間。L0 の下の L1 に 1GB ブロックを置く。
 *   * 0x00000000-0x40000000: デバイス (GIC・UART を含む)
 *   * 0x40000000-RAM 末尾:    通常メモリ (カーネルと RAM)
 * RAM の末尾は DTB が示す値で、arm_mmu_init に渡す。1GB ブロックを
 * 末尾まで並べるので、RAM が 1GB を超える構成でも写像が足りる
 * (上限は TCR の物理アドレス 36 ビット = 64GB)。
 * カーネルは 0x40080000 に載るので、恒等写像のまま動く。
 */
#ifndef PRISM_ARM_MMU_H
#define PRISM_ARM_MMU_H

#include <stdint.h>

/* ram_end: DTB から読んだ RAM の末尾アドレス (物理)。
 * そのアドレスまで通常メモリとして恒等写像する。 */
void arm_mmu_init(uint64_t ram_end);

#endif /* PRISM_ARM_MMU_H */
