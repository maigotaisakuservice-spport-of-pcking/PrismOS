/* PrismOS — 互換レイヤーの実証ゲスト 3: ファイルを読む。
 *
 * 使い方: cat <ファイル名>
 * argv[1] のファイルを開き (3 = open)、中身を読んで (2 = read)
 * 画面に出す (1 = write)。open に失敗したら fd 2 に文句を
 * 出して終了コード 1 で終わる。引数が無ければ使い方を出す。
 * 呼び出しは int $0x80。浮動小数点も SSE も使わない。
 * 位置に依存しない書き方だけ (lea ...(%rip)) をする。
 */
    .text
    .globl _start
_start:
    mov (%rsp), %rax            /* argc */
    cmp $2, %rax
    jl .usage
    mov 16(%rsp), %rdi          /* argv[1] */
    mov $3, %eax                /* open(path, O_RDONLY=0) */
    xor %esi, %esi
    int $0x80
    cmp $0, %rax
    jl .openfail
    mov %rax, %rbx              /* fd */
.Lread:
    mov $2, %eax                /* read(fd, buf, 256) */
    mov %rbx, %rdi
    lea buf(%rip), %rsi
    mov $256, %edx
    int $0x80
    cmp $0, %rax
    jle .done                   /* 0 = 終わり、負 = 失敗 */
    mov %rax, %rdx
    mov $1, %eax                /* write(1, buf, count) */
    mov $1, %edi
    lea buf(%rip), %rsi
    int $0x80
    jmp .Lread
.done:
    mov $4, %eax                /* close(fd) */
    mov %rbx, %rdi
    int $0x80
    mov $0, %eax                /* exit(0) */
    xor %edi, %edi
    int $0x80
.usage:
    mov $1, %eax
    mov $2, %edi                /* fd 2 */
    lea usage_msg(%rip), %rsi
    mov $usage_len, %edx
    int $0x80
    mov $0, %eax
    mov $1, %edi                /* exit(1) */
    int $0x80
.openfail:
    mov $1, %eax
    mov $2, %edi
    lea fail_msg(%rip), %rsi
    mov $fail_len, %edx
    int $0x80
    mov $0, %eax
    mov $1, %edi                /* exit(1) */
    int $0x80
1:  hlt
    jmp 1b
    .section .rodata
usage_msg:
    .ascii "usage: cat <file>\n"
    .set usage_len, . - usage_msg
fail_msg:
    .ascii "cat: cannot open file\n"
    .set fail_len, . - fail_msg
    .section .bss
    .align 16
buf:
    .skip 256
