/* PrismOS — 32bit カーネルエントリ (Multiboot1)
 *
 * Limine の multiboot1 プロトコルで起動する。ブートローダは
 * 32bit プロテクトモード (ページングなし) でここへ入る:
 *   EAX = 0x2BADB002 (マジック)、EBX = Multiboot 情報への
 *   ポインタ、セグメントはブートローダ任せ、スタックは不定。
 * ここで自分の GDT とスタックを用意して C の kmain32() を呼ぶ。
 * long mode には入らないので、32bit 専用 CPU でも動く。
 *
 * Multiboot ヘッダはファイル先頭 8192 バイト以内に要る。
 * リンカスクリプト (linker32.ld) が .multiboot を最初に置く。
 */
    .section .multiboot,"a",@progbits
    .align 4
    .long 0x1BADB002          /* マジック */
    .long 0x3                 /* flags: モジュールをページ境界に + メモリ情報を渡す */
    .long -(0x1BADB002 + 0x3) /* チェックサム */

    .section .text
    .global _start32
_start32:
    /* 自分の GDT に載せ替える (ブートローダのものを信用しない)。
     * EAX (マジック) と EBX (情報ポインタ) は壊さないこと。
     * セレクタの読み込みには CX を使う。 */
    lgdt gdt_ptr
    mov $0x10, %cx
    mov %cx, %ds
    mov %cx, %es
    mov %cx, %fs
    mov %cx, %gs
    mov %cx, %ss
    ljmp $0x08, $1f
1:
    mov $stack_top, %esp
    and $-16, %esp
    /* kmain32(magic, info) — cdecl で右から積む。 */
    push %ebx
    push %eax
    call kmain32
    cli
2:  hlt
    jmp 2b

    .section .rodata
    .align 8
gdt:
    .quad 0x0000000000000000  /* NULL */
    .quad 0x00CF9A000000FFFF  /* コード: 4GB フラット、実行/読み取り */
    .quad 0x00CF92000000FFFF  /* データ: 4GB フラット、読み書き */
gdt_ptr:
    .word gdt_ptr - gdt - 1
    .long gdt

    .section .bss
    .align 16
stack_bottom:
    .skip 32768
stack_top:

    .section .note.GNU-stack,"",@progbits
