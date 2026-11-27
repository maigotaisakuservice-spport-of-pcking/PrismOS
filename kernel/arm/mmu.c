/* PrismOS — ARM 用ページ表 (MMU、恒等写像)
 *
 * 4KB 粒度、48 ビット仮想空間。L0 の下の L1 に 1GB ブロックを置く。
 *   * 0x00000000-0x40000000: デバイス (GIC・UART を含む)
 *   * 0x40000000-RAM 末尾:    通常メモリ (カーネルと RAM)
 * RAM 側のブロックは DTB が示す末尾まで必要な分だけ並べる。
 * カーネルは 0x40080000 に載るので、恒等写像のまま動く。
 */
#include <stdint.h>

#include "mmu.h"

/* MAIR の属性番号: 0 = デバイス、1 = 通常メモリ。 */
#define MAIR_DEVICE 0x00u /* デバイス (キャッシュなし、順序厳密) */
#define MAIR_NORMAL 0xffu /* 通常、内側・外側とも書き戻しキャッシュ */

#define BLOCK_TABLE 0x3u /* 表への入口 (bit 1-0 = 11) */
#define BLOCK_BLOCK 0x1u /* 1GB ブロックへの入口 (bit 1-0 = 01) */
#define BLOCK_AF    (1u << 10) /* アクセス済みフラグ */
#define BLOCK_SH_INNER (3u << 8) /* 内側共有 */

__attribute__((aligned(4096))) static uint64_t l0_table[512];
__attribute__((aligned(4096))) static uint64_t l1_table[512];

void arm_mmu_init(uint64_t ram_end) {
    uint64_t mair;
    uint64_t tcr;
    uint64_t sctlr;
    uint64_t addr;
    int i;

    for (i = 0; i < 512; i++) {
        l0_table[i] = 0;
        l1_table[i] = 0;
    }

    /* L0[0] -> L1 表。 */
    l0_table[0] = ((uint64_t)l1_table & 0x0000fffffffff000ULL) | BLOCK_TABLE;
    /* L1[0]: 0x00000000 からの 1GB をデバイスとして写像する。 */
    l1_table[0] = 0x00000000ULL | (0u << 2) | BLOCK_SH_INNER | BLOCK_AF |
                  BLOCK_BLOCK;
    /* RAM: 0x40000000 から ram_end まで、1GB ブロックを並べて
     * 通常メモリとして写像する。ram_end が 1GB 境界でなくても
     * そのブロックごと写像する (末尾の端数は PMM が管理しない)。 */
    for (addr = 0x40000000ULL; addr < ram_end; addr += 0x40000000ULL) {
        l1_table[addr >> 30] = addr | (1u << 2) | BLOCK_SH_INNER |
                               BLOCK_AF | BLOCK_BLOCK;
    }

    /* MAIR: 属性 0 = デバイス、属性 1 = 通常メモリ。 */
    mair = ((uint64_t)MAIR_NORMAL << 8) | (uint64_t)MAIR_DEVICE;
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair));

    /* TCR_EL1: T0SZ=16 (48 ビット)、TG0=4KB、SH0=内側共有、
     * ORGN0/IRGN0=書き戻しキャッシュ、IPS=36 ビット。 */
    tcr = (16ULL << 0) | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) |
          (1ULL << 32);
    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr));

    __asm__ volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)l0_table));
    __asm__ volatile("isb" ::: "memory");

    /* 表を書き終えたので、TLB と命令キャッシュを捨てる。 */
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
    __asm__ volatile("dsb nsh" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("ic ialluis" ::: "memory");
    __asm__ volatile("dsb nsh" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* MMU とキャッシュを有効化する。 */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");
}
