/* PrismOS — 互換レイヤーの検証ゲスト (悪意): サンドボックスの
 * 外側 (カーネルのメモリ) を読もうとする。
 *
 * あいさつを出してから、カーネルの高い側のアドレス
 * (0xffffffff80000000) を読む。サンドボックスが効いていれば
 * ここでページフォルトになり、このゲストだけが終わる
 * (カーネルとシェルは生き続ける)。隔離が無ければカーネル
 * ごと止まるはずのコード。
 */
    .text
    .globl _start
_start:
    mov $1, %eax                /* write */
    mov $1, %edi
    lea msg(%rip), %rsi
    mov $msg_len, %edx
    int $0x80
    /* カーネルのメモリを読む (サンドボックス外)。 */
    movabs $0xffffffff80000000, %rax
    mov (%rax), %rbx
    /* ここに来てはいけない。到達したら exit 99 で知らせる。 */
    mov $0, %eax                /* exit */
    mov $99, %edi
    int $0x80
1:  hlt
    jmp 1b
    .section .rodata
msg:
    .ascii "evil guest: touching kernel memory now\n"
    .set msg_len, . - msg
