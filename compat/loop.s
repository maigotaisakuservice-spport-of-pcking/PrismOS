/* PrismOS — 互換レイヤーの検証ゲスト (暴走): あいさつを
 * 出してから無限ループに入る。呼び出しもメモリアクセスも
 * しないので、フォルトでは止まらない。サンドボックスの
 * ウォッチドッグ (時間制限) が止める役目で、止められれば
 * ゲストだけが終わりシェルが応答し続ける。
 */
    .text
    .globl _start
_start:
    mov $1, %eax                /* write */
    mov $1, %edi
    lea msg(%rip), %rsi
    mov $msg_len, %edx
    int $0x80
    /* 暴走する。 */
1:  jmp 1b
    .section .rodata
msg:
    .ascii "loop guest: spinning forever\n"
    .set msg_len, . - msg
