/* PrismOS — 互換レイヤーの実証ゲスト 10: 行数・語数・
 * バイト数を数える。
 *
 * 使い方: wc <ファイル名>
 * 開いて (3 = open) fstat (5) で大きさを取り、読み
 * (2 = read) ながら行数 ('\n') と語数 (空白区切りの
 * 塊) を数え、"wc: <行> <語> <バイト>" を出す。
 * 呼び出しは int $0x80、位置非依存のみ。
 */
    .text
    .globl _start
_start:
    mov (%rsp), %rax            /* argc */
    cmp $2, %rax
    jl .usage
    mov 16(%rsp), %rdi          /* argv[1] */
    mov $3, %eax                /* open(path, O_RDONLY) */
    xor %esi, %esi
    int $0x80
    cmp $0, %rax
    jl .openfail
    mov %rax, %rbx              /* fd */
    mov $5, %eax                /* fstat(fd, &statbuf) */
    mov %rbx, %rdi
    lea statbuf(%rip), %rsi
    int $0x80
    cmp $0, %rax
    jl .iofail
    mov statbuf(%rip), %r12     /* size */
    xor %r13d, %r13d            /* lines = 0 */
    xor %r14d, %r14d            /* words = 0 */
    xor %r15d, %r15d            /* in_word = 0 */
.Lread:
    mov $2, %eax                /* read(fd, buf, 256) */
    mov %rbx, %rdi
    lea buf(%rip), %rsi
    mov $256, %edx
    int $0x80
    cmp $0, %rax
    jl .iofail
    je .done
    mov %rax, %rcx              /* n */
    lea buf(%rip), %rsi
.Lscan:
    test %rcx, %rcx
    je .Lread
    movzbq (%rsi), %rax
    inc %rsi
    dec %rcx
    cmp $'\n', %al
    je .is_nl
    cmp $' ', %al
    je .is_sp
    cmp $'\t', %al
    je .is_sp
    /* 通常文字: 語の始まりなら words++ */
    test %r15, %r15
    jne .Lscan
    inc %r14
    mov $1, %r15
    jmp .Lscan
.is_nl:
    inc %r13
.is_sp:
    xor %r15d, %r15d
    jmp .Lscan
.done:
    mov $4, %eax                /* close(fd) */
    mov %rbx, %rdi
    int $0x80
    mov $1, %eax                /* "wc: " */
    mov $1, %edi
    lea wc_msg(%rip), %rsi
    mov $wc_len, %edx
    int $0x80
    mov %r13, %rax
    call print_dec
    call print_space
    mov %r14, %rax
    call print_dec
    call print_space
    mov %r12, %rax
    call print_dec
    mov $1, %eax
    mov $1, %edi
    lea nl_msg(%rip), %rsi
    mov $1, %edx
    int $0x80
    mov $0, %eax                /* exit(0) */
    xor %edi, %edi
    int $0x80
.usage:
    mov $1, %eax
    mov $2, %edi
    lea usage_msg(%rip), %rsi
    mov $usage_len, %edx
    int $0x80
    mov $0, %eax
    mov $1, %edi
    int $0x80
.openfail:
    mov $1, %eax
    mov $2, %edi
    lea fail_msg(%rip), %rsi
    mov $fail_len, %edx
    int $0x80
    mov $0, %eax
    mov $1, %edi
    int $0x80
.iofail:
    mov $1, %eax
    mov $2, %edi
    lea io_msg(%rip), %rsi
    mov $io_len, %edx
    int $0x80
    mov $0, %eax
    mov $2, %edi
    int $0x80
print_space:
    push %rax
    mov $1, %eax
    mov $1, %edi
    lea sp_msg(%rip), %rsi
    mov $1, %edx
    int $0x80
    pop %rax
    ret
/* print_dec: rax の符号なし10進を fd 1 へ出す。 */
print_dec:
    push %rbx
    push %r12
    lea num_buf+24(%rip), %r12
    mov %r12, %rbx
    mov $10, %ecx
    test %rax, %rax
    jne .pd_loop
    dec %rbx
    movb $'0', (%rbx)
    jmp .pd_out
.pd_loop:
    test %rax, %rax
    je .pd_out
    xor %edx, %edx
    div %rcx
    add $'0', %dl
    dec %rbx
    mov %dl, (%rbx)
    jmp .pd_loop
.pd_out:
    mov $1, %eax
    mov $1, %edi
    mov %rbx, %rsi
    mov %r12, %rdx
    sub %rbx, %rdx
    int $0x80
    pop %r12
    pop %rbx
    ret
    .section .rodata
usage_msg:
    .ascii "usage: wc <file>\n"
    .set usage_len, . - usage_msg
fail_msg:
    .ascii "wc: cannot open file\n"
    .set fail_len, . - fail_msg
io_msg:
    .ascii "wc: i/o error\n"
    .set io_len, . - io_msg
wc_msg:
    .ascii "wc: "
    .set wc_len, . - wc_msg
sp_msg:
    .ascii " "
nl_msg:
    .ascii "\n"
    .section .bss
    .align 16
buf:
    .skip 256
statbuf:
    .skip 8
num_buf:
    .skip 32
