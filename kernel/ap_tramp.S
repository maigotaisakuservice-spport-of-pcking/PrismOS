/* PrismOS — AP (Application Processor) 起動トランポリン。
 *
 * このバイト列を BSP が物理 0x8000 にコピーし、SIPI の
 * ベクタ 0x08 で各 AP がここから目覚める。実行位置は
 * 固定 (0x8000) 前提で、内部参照はすべて
 * 「0x8000 + ラベル差分」で解決する。
 *
 * 遷移: 16bit リアルモード → 32bit 保護モード (PAE) →
 * CR3 をスロットから読み、EFER.LME + ページングで
 * 互換モード → 64bit コードへ far jump → スロットの
 * スタックと入口 (ap_main) へ跳ぶ。ページ 0x8000 は
 * BSP が恒等マップしておく。
 *
 * データスロットはページ末尾寄り (オフセットは
 * smp.c が extern シンボルで参照する)。
 */
    .section .rodata.ap_tramp, "a"
    .globl ap_trampoline
    .globl ap_trampoline_end
    .globl ap_tramp_cr3_slot
    .globl ap_tramp_stack_slot
    .globl ap_tramp_entry_slot

    .code16
ap_trampoline:
    cli
    cld
    movw $0x0800, %ax
    movw %ax, %ds
    movw %ax, %ss
    movw $0x7F0, %sp
    lgdt tramp_gdt32_desc - ap_trampoline
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    /* 16bit モードで 32bit オフセットの far jump をするには
     * オペランドサイズ接頭辞 0x66 が要る (無いと ptr16:16
     * として解釈され、セレクタ 0 を読んで #GP になる)。 */
    .byte 0x66, 0xEA
    .long 0x8000 + (tramp_prot32 - ap_trampoline)
    .word 0x08

    .code32
tramp_prot32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    /* PAE を有効化 */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4
    /* CR3 は BSP がスロットに書いたカーネルの値 */
    movl 0x8000 + (tramp_cr3_slot - ap_trampoline), %eax
    movl %eax, %cr3
    /* EFER.LME */
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr
    /* ページング有効化 (互換モードに入る) */
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0
    lgdt 0x8000 + (tramp_gdt64_desc - ap_trampoline)
    .byte 0xEA
    .long 0x8000 + (tramp_long64 - ap_trampoline)
    .word 0x08

    .code64
tramp_long64:
    movq 0x8000 + (tramp_stack_slot - ap_trampoline), %rsp
    movq 0x8000 + (tramp_entry_slot - ap_trampoline), %rax
    jmp *%rax

    .align 8
tramp_gdt32:
    .quad 0
    .quad 0x00CF9A000000FFFF /* 32bit コード (flat 4GB) */
    .quad 0x00CF92000000FFFF /* 32bit データ (flat 4GB) */
tramp_gdt32_desc:
    .word tramp_gdt32_desc - tramp_gdt32 - 1
    .long 0x8000 + (tramp_gdt32 - ap_trampoline)
tramp_gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF /* 64bit コード (L=1) */
    .quad 0x00CF92000000FFFF /* データ */
tramp_gdt64_desc:
    .word tramp_gdt64_desc - tramp_gdt64 - 1
    .long 0x8000 + (tramp_gdt64 - ap_trampoline)

    .align 8
tramp_cr3_slot:
ap_tramp_cr3_slot:
    .quad 0
tramp_stack_slot:
ap_tramp_stack_slot:
    .quad 0
tramp_entry_slot:
ap_tramp_entry_slot:
    .quad 0
ap_trampoline_end:
