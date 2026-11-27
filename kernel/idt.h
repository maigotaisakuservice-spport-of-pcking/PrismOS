/* PrismOS — IDT (割り込み記述子テーブル)
 *
 * v0.1 には IDT が無く、どんな例外もトリプルフォルトで QEMU が
 * 終わっていた。v0.2 フェーズ1で IDT を入れ、例外はベクタ番号と
 * RIP をシリアルに表示して止まるようにする。キーボード (IRQ1)
 * の割り込みもここで受ける。
 */
#ifndef PRISM_IDT_H
#define PRISM_IDT_H

#include <stdint.h>

/* GCC の interrupt 属性が期待するフレームの形。割り込み
 * ハンドラを書くモジュール (sandbox.c のウォッチドッグ等)
 * も使うので、ここに置く。 */
struct interrupt_frame {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

/* IDT を構築して lidt で読み込む。割り込みの許可 (sti) はしない。
 * kmain() が PIC とキーボードの初期化のあと sti する。 */
void idt_init(void);
void idt_load_existing(void);

/* 1 個のゲートを設定する (ウォッチドッグが IRQ0 を出し入れ
 * するために公開する)。handler が NULL ならゲートを消す。 */
void idt_set_gate(int vec, void *handler, uint8_t type_attr);

#endif /* PRISM_IDT_H */
