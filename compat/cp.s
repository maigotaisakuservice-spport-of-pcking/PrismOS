/* PrismOS — 互換レイヤーの実証ゲスト 9: ファイルコピー。
 *
 * 使い方: cp <コピー元> <コピー先>
 * 元を開いて (3 = open) 読み (2 = read)、先を作って
 * (3 = open, WRONLY|CREAT) 書き (1 = write)、両方閉じる
 * (4 = close)。最後に先を読み直して lseek (8) の
 * SEEK_END で大きさを確かめ、写したバイト数と一致したら
 * "cp: ok <n> bytes" を出す。複数ファイル処理・引数・
 * lseek の実証。呼び出しは int $0x80、位置非依存のみ。
 */
    .text
    .globl _start
_start:
    mov (%rsp), %rax            /* argc */
    cmp $3, %rax
    jl .usage
    mov 16(%rsp), %r12          /* argv[1] = 元 */
    mov 24(%rsp), %r13          /* argv[2] = 先 */
    mov $3, %eax                /* open(src, O_RDONLY) */
    mov %r12, %rdi
    xor %esi, %esi
    int $0x80
    cmp $0, %rax
    jl .openfail
    mov %rax, %rbx              /* sfd */
    mov $3, %eax                /* open(dst, WRONLY|CREAT) */
    mov %r13, %rdi
    mov $0x41, %esi
    int $0x80
    cmp $0, %rax
    jl .openfail
    mov %rax, %rbp              /* dfd */
    xor %r14d, %r14d            /* total = 0 */
.Lread:
    mov $2, %eax                /* read(sfd, buf, 256) */
    mov %rbx, %rdi
    lea buf(%rip), %rsi
    mov $256, %edx
    int $0x80
    cmp $0, %rax
    jl .iofail
    je .copied                  /* 0 = 終わり */
    mov %rax, %r15              /* n */
    mov $1, %eax                /* write(dfd, buf, n) */
    mov %rbp, %rdi
    lea buf(%rip), %rsi
    mov %r15, %rdx
    int $0x80
    cmp %r15, %rax
    jne .iofail
    add %r15, %r14
    jmp .Lread
.copied:
    mov $4, %eax                /* close(dfd) */
    mov %rbp, %rdi
    int $0x80
    mov $4, %eax                /* close(sfd) */
    mov %rbx, %rdi
    int $0x80
    mov $3, %eax                /* open(dst, O_RDONLY) */
    mov %r13, %rdi
    xor %esi, %esi
    int $0x80
    cmp $0, %rax
    jl .iofail
    mov %rax, %rbx
    mov $8, %eax                /* lseek(fd, 0, SEEK_END) */
    mov %rbx, %rdi
    xor %esi, %esi
    mov $2, %edx
    int $0x80
    cmp %r14, %rax
    jne .mismatch
    mov $4, %eax                /* close(fd) */
    mov %rbx, %rdi
    int $0x80
    /* "cp: ok " + total + " bytes\n" */
    mov $1, %eax
    mov $1, %edi
    lea ok_msg(%rip), %rsi
    mov $ok_len, %edx
    int $0x80
    mov %r14, %rax
    call print_dec
    mov $1, %eax
    mov $1, %edi
    lea bytes_msg(%rip), %rsi
    mov $bytes_len, %edx
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
    mov $2, %edi                /* exit(2) */
    int $0x80
.mismatch:
    mov $1, %eax
    mov $2, %edi
    lea mm_msg(%rip), %rsi
    mov $mm_len, %edx
    int $0x80
    mov $0, %eax
    mov $2, %edi
    int $0x80
/* print_dec: rax の符号なし10進を fd 1 へ出す。 */
print_dec:
    push %rbx
    push %r12
    lea num_buf+24(%rip), %r12  /* 末尾から前へ詰める */
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
    mov $1, %eax                /* write(1, rbx, r12-rbx) */
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
    .ascii "usage: cp <src> <dst>\n"
    .set usage_len, . - usage_msg
fail_msg:
    .ascii "cp: cannot open file\n"
    .set fail_len, . - fail_msg
io_msg:
    .ascii "cp: i/o error\n"
    .set io_len, . - io_msg
mm_msg:
    .ascii "cp: size mismatch\n"
    .set mm_len, . - mm_msg
ok_msg:
    .ascii "cp: ok "
    .set ok_len, . - ok_msg
bytes_msg:
    .ascii " bytes\n"
    .set bytes_len, . - bytes_msg
    .section .bss
    .align 16
buf:
    .skip 256
num_buf:
    .skip 32
