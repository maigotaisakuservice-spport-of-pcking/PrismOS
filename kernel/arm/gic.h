/* PrismOS — ARM 用 GICv2 ドライバ (QEMU virt 構成)
 *
 * 分配器 (GICD) 0x08000000、CPU 側 (GICC) 0x08010000。
 * 汎用タイマの割り込み (PPI 27) を CPU0 に届けるのが目的。
 */
#ifndef PRISM_ARM_GIC_H
#define PRISM_ARM_GIC_H

void arm_gic_init(void);
/* IRQ ベクタから呼ばれる。IAR を読み、タイマなら刻みを進め、
 * EOIR で終わらせる。 */
void arm_irq_handler(void);

#endif /* PRISM_ARM_GIC_H */
