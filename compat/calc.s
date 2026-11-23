/* PrismOS — 互換レイヤーの実証ゲスト 2: 計算して結果を出す。
 *
 * 1 から 100 までの和 (5050) と 7 の階乗 (5040) を計算し、
 * 10 進に直して書き出す。10 進への変換も自前 (div 命令)。
 * 呼び出しは int $0x80 (1 = write, 0 = exit)。
 * 浮動小数点も SSE も使わない。位置に依存しない書き方だけ
 * (lea ...(%rip)) をする。
 */
    .text
    .globl _start
_start:
    /* sum 1..100 -> rbx */
    xor %ecx, %ecx
    xor %ebx, %ebx
1:
    inc %ecx
    add %ecx, %ebx
    cmp $100, %ecx
    jne 1b
    /* 7! -> r12 */
    mov $1, %r12d
    mov $2, %rcx
2:
    imul %rcx, %r12
    inc %rcx
    cmp $8, %rcx
    jne 2b
    /* "sum 1..100 = " */
    mov $1, %eax
    mov $1, %edi
    lea prefix1(%rip), %rsi
    mov $prefix1_len, %edx
    int $0x80
    mov %rbx, %rax
    call print_u64
    /* "7! = " */
    mov $1, %eax
    mov $1, %edi
    lea prefix2(%rip), %rsi
    mov $prefix2_len, %edx
    int $0x80
    mov %r12, %rax
    call print_u64
    mov $0, %eax                /* exit(0) */
    xor %edi, %edi
    int $0x80
1:  hlt
    jmp 1b

/* print_u64: rax の値を 10 進 + 改行で書き出す。
 * rbx / r12 は壊さない。 */
print_u64:
    sub $32, %rsp
    lea 31(%rsp), %r8           /* r8 = 末尾の改行の位置 */
    movb $10, (%r8)
    mov %r8, %r9                /* r9 = 終端 */
    mov $10, %rcx
.Lconv:
    xor %edx, %edx
    div %rcx                    /* rax = rax / 10, rdx = 余り */
    add $'0', %dl
    dec %r8
    mov %dl, (%r8)
    test %rax, %rax
    jne .Lconv
    mov %r9, %rdx
    sub %r8, %rdx
    inc %rdx                    /* 改行の分 */
    mov %r8, %rsi
    mov $1, %eax                /* write(1, rsi, rdx) */
    mov $1, %edi
    int $0x80
    add $32, %rsp
    ret
    .section .rodata
prefix1:
    .ascii "sum 1..100 = "
    .set prefix1_len, . - prefix1
prefix2:
    .ascii "7! = "
    .set prefix2_len, . - prefix2
