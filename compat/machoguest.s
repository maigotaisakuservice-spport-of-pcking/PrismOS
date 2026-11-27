/* PrismOS — Mach-O 実証ゲスト: あいさつを出すだけの
 * 小さな実行ファイル (アセンブリで手書き)。
 *
 * 互換レイヤーの呼び出し (int $0x80) を使う:
 *   eax = 番号, rdi/rsi/rdx = 引数、戻り値は rax。
 *   1 = write(fd, buf, len), 0 = exit(code)。
 * 浮動小数点も SSE も使わない (起動時の CPU は SSE が
 * 有効化されていないため)。位置に依存しない書き方だけを
 * する (lea ...(%rip)) ので、ローダーがどこに載せても動く。
 * Mach-O の中身は scripts/build-macho-guest.py が作る。
 */
    .text
    .globl _start
_start:
    mov $1, %eax                /* write */
    mov $1, %edi                /* fd 1 */
    lea msg(%rip), %rsi
    mov $msg_len, %edx
    int $0x80
    mov $0, %eax                /* exit */
    mov $42, %edi               /* 終了コード 42 */
    int $0x80
1:  hlt
    jmp 1b
    .section .rodata
msg:
    .ascii "Hello from Mach-O guest!\n"
    .set msg_len, . - msg
