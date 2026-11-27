/* PrismOS — 互換レイヤーの実証ゲスト 4: 未対応の呼び出し。
 *
 * 存在しない呼び出し番号 999 を呼ぶ。互換レイヤーは -38
 * (ENOSYS: 未対応) を返さなければならない。返り値が -38
 * なら「ok: unsupported syscall returned -38」と出して
 * 終了コード 0、そうでなければ文句を出して終了コード 1。
 * 呼び出しは int $0x80。浮動小数点も SSE も使わない。
 */
    .text
    .globl _start
_start:
    mov $999, %eax              /* 存在しない呼び出し */
    int $0x80
    cmp $-38, %rax
    jne .unexpected
    mov $1, %eax                /* write(1, ok_msg, ok_len) */
    mov $1, %edi
    lea ok_msg(%rip), %rsi
    mov $ok_len, %edx
    int $0x80
    mov $0, %eax                /* exit(0) */
    xor %edi, %edi
    int $0x80
.unexpected:
    mov $1, %eax
    mov $2, %edi
    lea bad_msg(%rip), %rsi
    mov $bad_len, %edx
    int $0x80
    mov $0, %eax
    mov $1, %edi                /* exit(1) */
    int $0x80
1:  hlt
    jmp 1b
    .section .rodata
ok_msg:
    .ascii "ok: unsupported syscall returned -38\n"
    .set ok_len, . - ok_msg
bad_msg:
    .ascii "unexpected result for unsupported syscall\n"
    .set bad_len, . - bad_msg
