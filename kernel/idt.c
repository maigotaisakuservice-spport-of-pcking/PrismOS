/* PrismOS — IDT と例外ハンドラ (v0.2 フェーズ1)
 *
 * Limine が作る GDT では 64 ビットコードセグメントのセレクタは
 * 0x28 (run #10 のレジスタでも CS=0x28)。IDT のセレクタは 0x28 を
 * 使う。0x08 だと #GP になるので注意。
 *
 * 例外ハンドラは GCC の __attribute__((interrupt)) で書く。
 * エラーコードを積む例外 (8, 10, 11, 12, 13, 14, 17, 21, 29, 30)
 * だけ 2 引数版、それ以外は 1 引数版にする。
 * ハンドラはベクタ・エラーコード・RIP をシリアルに出して止まる。
 * (IDT が無い v0.1 では、ここでトリプルフォルトになっていた)
 */
#include "idt.h"
#include "appguard.h"
#include "compat_run.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "serial.h"

#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];

static void print_u64_dec(uint64_t value) {
    char buf[21];
    int i = 0;
    if (value == 0) {
        serial_write_char('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        serial_write_char(buf[--i]);
    }
}

static void print_u64_hex(uint64_t value) {
    const char *hex = "0123456789abcdef";
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        int digit = (int)((value >> shift) & 0xF);
        if (digit != 0 || started || shift == 0) {
            serial_write_char(hex[digit]);
            started = 1;
        }
    }
}

static void exception_handler(int vec, struct interrupt_frame *frame,
                              uint64_t error_code) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    /* サンドボックス: ゲスト (リング 3) の例外、または
     * ディスパッチャがゲストの範囲内のページで起こした
     * ページフォルトは、ゲストだけを終わらせる。カーネルは
     * 生き続け、シェルが応答する。 */
    if (compat_guest_active &&
        ((frame->cs & 3) == 3 ||
         (vec == 14 && compat_guest_addr_in_range(cr2)))) {
        serial_write("\n[sandbox] guest fault: vector=");
        print_u64_dec((uint64_t)vec);
        serial_write(" rip=0x");
        print_u64_hex(frame->ip);
        serial_write(" cr2=0x");
        print_u64_hex(cr2);
        serial_write_line(" -- guest killed, kernel alive.");
        compat_guest_bail(COMPAT_KILL_FAULT);
    }
    /* 内蔵アプリの封じ込め: アプリの描画・入力ディスパッチ
     * 中 (appguard 武装中) のカーネル側例外は、ディスパッチ
     * 地点へ戻してアプリだけを閉じる。カーネルは生き続ける。 */
    if (appguard_armed() && (frame->cs & 3) == 0) {
        serial_write("\n[appguard] fault in a built-in app: vector=");
        print_u64_dec((uint64_t)vec);
        serial_write(" rip=0x");
        print_u64_hex(frame->ip);
        serial_write_line(" -- contained, kernel alive.");
        /* 割り込みゲートで入って IF が落ちている。武装地点は
         * IF=1 で動いていたので、戻る前に立て直す (iret を
         * 経由しない復帰のため)。 */
        prism_sti();
        appguard_bail();
    }
    serial_write("\n[exception] vector=");
    print_u64_dec((uint64_t)vec);
    serial_write(" error=0x");
    print_u64_hex(error_code);
    serial_write(" rip=0x");
    print_u64_hex(frame->ip);
    serial_write(" cr2=0x");
    print_u64_hex(cr2);
    serial_write_line(" -- halting (caught by IDT, no triple fault).");
    prism_cli();
    prism_halt();
}

/* エラーコード無しの例外用 */
#define EXC_NOERR(n)                                                     \
    __attribute__((interrupt)) static void exc_##n(                      \
        struct interrupt_frame *frame) {                                 \
        exception_handler(n, frame, 0);                                  \
    }

/* エラーコード付きの例外用 */
#define EXC_ERR(n)                                                       \
    __attribute__((interrupt)) static void exc_##n(                      \
        struct interrupt_frame *frame, uint64_t error_code) {            \
        exception_handler(n, frame, error_code);                         \
    }

EXC_NOERR(0)
EXC_NOERR(1)
EXC_NOERR(2)
EXC_NOERR(3)
EXC_NOERR(4)
EXC_NOERR(5)
EXC_NOERR(6)
EXC_NOERR(7)
EXC_ERR(8)
EXC_NOERR(9)
EXC_ERR(10)
EXC_ERR(11)
EXC_ERR(12)
EXC_ERR(13)
EXC_ERR(14)
EXC_NOERR(15)
EXC_NOERR(16)
EXC_ERR(17)
EXC_NOERR(18)
EXC_NOERR(19)
EXC_NOERR(20)
EXC_ERR(21)
EXC_NOERR(22)
EXC_NOERR(23)
EXC_NOERR(24)
EXC_NOERR(25)
EXC_NOERR(26)
EXC_NOERR(27)
EXC_NOERR(28)
EXC_ERR(29)
EXC_ERR(30)
EXC_NOERR(31)

/* IRQ1 (ベクタ 33): キーボード */
__attribute__((interrupt)) static void irq1_handler(
    struct interrupt_frame *frame) {
    (void)frame;
    keyboard_irq_handler();
}

/* IRQ12 (ベクタ 44): マウス */
__attribute__((interrupt)) static void irq12_handler(
    struct interrupt_frame *frame) {
    (void)frame;
    mouse_irq_handler();
}

/* 互換レイヤーの呼び出し入口 (kernel/compat_gate.S)。 */
extern void compat_syscall_gate(void);

void idt_set_gate(int vec, void *handler, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[vec].offset_low = (uint16_t)(addr & 0xFFFF);
    idt[vec].selector = 0x28; /* Limine の 64 ビットコードセグメント */
    idt[vec].ist = 0;
    idt[vec].type_attr = type_attr;
    idt[vec].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].zero = 0;
}

/* AP 用: BSP が作った IDT をそのまま読み直す。 */
void idt_load_existing(void) {
    struct idtr idtr = {
        .limit = (uint16_t)(sizeof(idt) - 1),
        .base = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

/* SMP の IPI (ベクタ 0x40): 相手 CPU が生きている証拠に
 * カウンタを進める。実装は smp.c。 */
extern void smp_ipi_handler(void);
__attribute__((interrupt)) static void ipi40_handler(
    struct interrupt_frame *frame) {
    (void)frame;
    smp_ipi_handler();
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].type_attr = 0;
        idt[i].offset_mid = 0;
        idt[i].offset_high = 0;
        idt[i].zero = 0;
    }

    /* 割り込みゲート (P=1, DPL=0, type=0xE) = 0x8E */
    idt_set_gate(0, exc_0, 0x8E);
    idt_set_gate(1, exc_1, 0x8E);
    idt_set_gate(2, exc_2, 0x8E);
    idt_set_gate(3, exc_3, 0x8E);
    idt_set_gate(4, exc_4, 0x8E);
    idt_set_gate(5, exc_5, 0x8E);
    idt_set_gate(6, exc_6, 0x8E);
    idt_set_gate(7, exc_7, 0x8E);
    idt_set_gate(8, exc_8, 0x8E);
    idt_set_gate(9, exc_9, 0x8E);
    idt_set_gate(10, exc_10, 0x8E);
    idt_set_gate(11, exc_11, 0x8E);
    idt_set_gate(12, exc_12, 0x8E);
    idt_set_gate(13, exc_13, 0x8E);
    idt_set_gate(14, exc_14, 0x8E);
    idt_set_gate(15, exc_15, 0x8E);
    idt_set_gate(16, exc_16, 0x8E);
    idt_set_gate(17, exc_17, 0x8E);
    idt_set_gate(18, exc_18, 0x8E);
    idt_set_gate(19, exc_19, 0x8E);
    idt_set_gate(20, exc_20, 0x8E);
    idt_set_gate(21, exc_21, 0x8E);
    idt_set_gate(22, exc_22, 0x8E);
    idt_set_gate(23, exc_23, 0x8E);
    idt_set_gate(24, exc_24, 0x8E);
    idt_set_gate(25, exc_25, 0x8E);
    idt_set_gate(26, exc_26, 0x8E);
    idt_set_gate(27, exc_27, 0x8E);
    idt_set_gate(28, exc_28, 0x8E);
    idt_set_gate(29, exc_29, 0x8E);
    idt_set_gate(30, exc_30, 0x8E);
    idt_set_gate(31, exc_31, 0x8E);
    idt_set_gate(33, irq1_handler, 0x8E); /* IRQ1: キーボード */
    idt_set_gate(44, irq12_handler, 0x8E); /* IRQ12: マウス */
    /* int 0x80 はリング 3 からも呼べるよう DPL=3 (0xEE)。
     * ゲストのサンドボックス実行に使う。それ以外の例外・
     * IRQ のゲートは DPL=0 のまま。 */
    idt_set_gate(0x80, compat_syscall_gate, 0xEE); /* int 0x80: 互換レイヤーの呼び出し */
    idt_set_gate(0x40, ipi40_handler, 0x8E); /* IPI: SMP 生存確認 */

    struct idtr idtr = {
        .limit = (uint16_t)(sizeof(idt) - 1),
        .base = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
