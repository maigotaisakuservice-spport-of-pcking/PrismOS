/* PrismOS — 8259 PIC (割り込みコントローラ)
 *
 * マスタをベクタ 32〜39、スレーブを 40〜47 にリマップする。
 * 許可するのはキーボードの IRQ1 と、マウスの IRQ12 と、
 * カスケードの IRQ2 だけ。
 * それ以外 (タイマ IRQ0 を含む) はマスクしたままにする。
 */
#ifndef PRISM_PIC_H
#define PRISM_PIC_H

#include <stdint.h>

void pic_init(void);
/* IRQ の処理が終わったことを PIC に知らせる (EOI)。 */
void pic_send_eoi(uint8_t irq);

#endif /* PRISM_PIC_H */
