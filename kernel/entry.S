/* PrismOS — カーネルエントリ (Limine から呼ばれる _start)
 *
 * スタックを張り替えて C の kmain() を呼ぶだけの薄い層。
 * 割り込みは v0.1 では使わない。
 */
    .section .text
    .global _start
_start:
    mov $stack_top, %rsp
    /* 16 バイトアラインを保ったまま呼ぶ */
    and $-16, %rsp
    call kmain
    cli
1:  hlt
    jmp 1b

    .section .bss
    .align 16
stack_bottom:
    .skip 16384
stack_top:
