/* PrismOS — 互換レイヤーの実証ゲスト: ネットワーク。
 *
 * TCP クライアントの呼び出しを使う:
 *   15 = net_connect(host, port) -> handle
 *   16 = net_send(handle, buf, len)
 *   17 = net_recv(handle, buf, cap)
 *   18 = net_close(handle)
 * 10.0.2.2:8730 (エミュレータのホスト側サーバー) へ
 * "ping from guest" を送り、返事を stdout へ出す。
 * 接続できなければ終了コード 3。 */
    .text
    .globl _start
_start:
    mov $15, %eax
    lea host(%rip), %rdi
    mov $8730, %esi
    int $0x80
    test %rax, %rax
    js fail
    mov %rax, %rbx          /* handle */
    mov $16, %eax
    mov %rbx, %rdi
    lea line(%rip), %rsi
    mov $line_len, %edx
    int $0x80
    mov $17, %eax
    mov %rbx, %rdi
    lea buf(%rip), %rsi
    mov $128, %edx
    int $0x80
    test %rax, %rax
    jle done                /* 返事なしでも送れたことは確か */
    mov %rax, %rdx          /* n バイトを stdout へ */
    mov $1, %eax
    mov $1, %edi
    lea buf(%rip), %rsi
    int $0x80
done:
    mov $18, %eax
    mov %rbx, %rdi
    int $0x80
    mov $1, %eax
    mov $1, %edi
    lea msg(%rip), %rsi
    mov $msg_len, %edx
    int $0x80
    mov $0, %eax
    xor %edi, %edi
    int $0x80
fail:
    mov $1, %eax
    mov $1, %edi
    lea fmsg(%rip), %rsi
    mov $fmsg_len, %edx
    int $0x80
    mov $0, %eax
    mov $3, %edi
    int $0x80
1:  hlt
    jmp 1b
    .section .rodata
host:
    .ascii "10.0.2.2"
    .byte 0
line:
    .ascii "ping from guest\n"
    .set line_len, . - line
msg:
    .ascii "netdemo: done\n"
    .set msg_len, . - msg
fmsg:
    .ascii "netdemo: connect failed\n"
    .set fmsg_len, . - fmsg
    .bss
buf:
    .space 128
