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
/* カーネルスタックは 32KB。16KB だったとき、回復・更新からの
 * 同期のように呼び出しが深い場所で溢れ、スタック直下の VFS の
 * ファイル表を潰した (2026-11-21 に回復モードの選択肢 4 で発覚)。
 * 大きな作業バッファは静的に置くようにも直したが、余裕のため
 * 32KB に広げてある。 */
stack_bottom:
    .skip 32768
stack_top:
