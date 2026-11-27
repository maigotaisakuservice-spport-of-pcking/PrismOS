/* PrismOS — ARM 用 GICv2 ドライバ (QEMU virt 構成)
 *
 * 分配器 (GICD) 0x08000000、CPU 側 (GICC) 0x08010000。
 * 汎用タイマの割り込み (PPI 27) を CPU0 に届けるのが目的。
 * 割り込みはグループ 0 だけで扱う。
 */
#include <stdint.h>

#include "gic.h"
#include "timer.h"

#define GICD_BASE ((volatile uint32_t *)0x08000000u)
#define GICC_BASE ((volatile uint32_t *)0x08010000u)

/* 分配器のレジスタ (32 ビット単位の番号)。 */
#define GICD_CTLR      0x000 /* 制御 */
#define GICD_IGROUPR   0x020 /* グループ (32 ビット単位) */
#define GICD_ISENABLER 0x040 /* 有効化 (32 ビット単位) */
#define GICD_IPRIORITYR 0x100 /* 優先度 (32 ビット単位) */
#define GICD_ITARGETSR 0x200 /* 届け先 CPU (32 ビット単位) */

/* CPU 側のレジスタ (32 ビット単位の番号)。 */
#define GICC_CTLR 0x000 /* 制御 */
#define GICC_PMR  0x001 /* 優先度マスク */
#define GICC_IAR  0x003 /* 応答 */
#define GICC_EOIR 0x004 /* 終了 */

#define TIMER_IRQ_ID 30 /* 非セキュア物理タイマ (PPI)。DTB の
                          * タイマ欄が示す番号 (PPI 14 = ID 30)。
                          * 27 は EL2 用タイマなので届かない。 */

void arm_gic_init(void) {
    uint32_t i;

    /* 一旦止める。 */
    GICD_BASE[GICD_CTLR] = 0;

    /* 全部グループ 0 にする。 */
    for (i = 0; i < 32; i++) {
        GICD_BASE[GICD_IGROUPR + i] = 0;
    }
    /* 全部の優先度を 0xA0 にする。 */
    for (i = 0; i < 255; i++) {
        GICD_BASE[GICD_IPRIORITYR + i] = 0xa0a0a0a0u;
    }
    /* 全部 CPU0 に届ける。 */
    for (i = 0; i < 255; i++) {
        GICD_BASE[GICD_ITARGETSR + i] = 0x01010101u;
    }

    /* タイマ (PPI 27) だけ有効化する。 */
    GICD_BASE[GICD_ISENABLER] = (1u << TIMER_IRQ_ID);

    /* 分配器を動かす (グループ 0 を転送)。 */
    GICD_BASE[GICD_CTLR] = 1;

    /* CPU 側: 優先度マスクは全部通し、制御を有効化する。 */
    GICC_BASE[GICC_PMR] = 0xff;
    GICC_BASE[GICC_CTLR] = 1;
}

void arm_irq_handler(void) {
    uint32_t iar = GICC_BASE[GICC_IAR];
    uint32_t id = iar & 0x3ffu;

    if (id == TIMER_IRQ_ID) {
        /* タイマの刻みを進め、次の割り込みを仕掛ける。 */
        arm_timer_tick();
    }
    /* 1023 は「割り込みなし」なので EOIR は要らない。 */
    if (id != 1023) {
        GICC_BASE[GICC_EOIR] = iar;
    }
}
