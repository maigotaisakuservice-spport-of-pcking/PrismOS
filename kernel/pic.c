/* PrismOS — 8259 PIC のリマップとマスク */
#include "pic.h"
#include "io.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

void pic_init(void) {
    /* ICW1: 初期化開始 (ICW4 あり) */
    prism_outb(PIC1_CMD, 0x11);
    prism_outb(PIC2_CMD, 0x11);
    /* ICW2: ベクタの基点。マスタ 32、スレーブ 40 */
    prism_outb(PIC1_DATA, 0x20);
    prism_outb(PIC2_DATA, 0x28);
    /* ICW3: カスケード接続。マスタは IRQ2 に、スレーブは 2 番に */
    prism_outb(PIC1_DATA, 0x04);
    prism_outb(PIC2_DATA, 0x02);
    /* ICW4: 8086 モード */
    prism_outb(PIC1_DATA, 0x01);
    prism_outb(PIC2_DATA, 0x01);
    /* マスク: IRQ1 (キーボード) と IRQ2 (カスケード) だけ許可。
     * 0xF9 = 1111 1001。スレーブは IRQ12 (マウス) だけ許可。
     * 0xEF = 1110 1111。 */
    prism_outb(PIC1_DATA, 0xF9);
    prism_outb(PIC2_DATA, 0xEF);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        prism_outb(PIC2_CMD, 0x20);
    }
    prism_outb(PIC1_CMD, 0x20);
}
