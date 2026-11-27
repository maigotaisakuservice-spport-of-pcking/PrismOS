/* PrismOS — PE ゲストの実証プログラム (コンソール)。
 *
 * KERNEL32.dll の 6 関数だけを使う:
 *   GetStdHandle(-11) で標準出力ハンドルを取り、
 *   WriteFile で "Hello from PE guest!" を出し、
 *   CreateFileA で about.txt を開き、ReadFile で先頭を
 *   読んで標準出力へ書き、CloseHandle で閉じ、
 *   ExitProcess(7) で終わる。
 * インポートは .idata に自前で並べ、IAT スロット経由で
 * 呼ぶ (ローダーがスタブのアドレスに書き換える)。
 * 配置は build-pe-guest.py のリンカスクリプトと一致させる:
 *   .text  = RVA 0x1000、.idata = RVA 0x2000、
 *   イメージ ベース 0x140000000。 */
    .text
    .globl _start
_start:
    sub $56, %rsp
    mov $-11, %ecx              /* STD_OUTPUT_HANDLE */
    call *__imp_GetStdHandle(%rip)
    mov %rax, %rbx              /* 標準出力ハンドル */
    mov %rbx, %rcx
    lea msg(%rip), %rdx
    mov $msg_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)           /* 第 5 引数 lpOverlapped */
    call *__imp_WriteFile(%rip)
    /* about.txt を読み用に開く */
    lea fname(%rip), %rcx
    mov $0x80000000, %edx       /* GENERIC_READ */
    xor %r8d, %r8d              /* 共有モード */
    xor %r9d, %r9d              /* セキュリティ属性 */
    movq $3, 32(%rsp)           /* OPEN_EXISTING */
    movq $0, 40(%rsp)           /* フラグ */
    movq $0, 48(%rsp)           /* テンプレート */
    call *__imp_CreateFileA(%rip)
    cmp $-1, %rax
    je nofile
    mov %rax, %r12              /* ファイルハンドル */
    mov %r12, %rcx
    lea rbuf(%rip), %rdx
    mov $16, %r8d
    lea nread(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_ReadFile(%rip)
    mov %rbx, %rcx
    lea rbuf(%rip), %rdx
    mov nread(%rip), %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)
    mov %r12, %rcx
    call *__imp_CloseHandle(%rip)
nofile:
    mov $7, %ecx
    call *__imp_ExitProcess(%rip)
1:  hlt
    jmp 1b
msg:
    .ascii "Hello from PE guest!\n"
    .set msg_len, . - msg
fname:
    .ascii "about.txt"
    .byte 0

    /* ---- インポート (RVA 0x2000 から) ---- */
    .section .idata,"aw"
    /* ディスクリプタ (1 DLL + 終端) */
    .org 0x00
    .long 0x2040                /* OriginalFirstThunk (ILT) */
    .long 0                     /* TimeDateStamp */
    .long 0                     /* ForwarderChain */
    .long 0x21A0                /* Name */
    .long 0x2080                /* FirstThunk (IAT) */
    .org 0x14
    .long 0, 0, 0, 0, 0         /* 終端ディスクリプタ */
    /* ILT (0x2040) */
    .org 0x40
    .quad 0x20C0
    .quad 0x20E0
    .quad 0x2100
    .quad 0x2120
    .quad 0x2140
    .quad 0x2160
    .quad 0
    /* IAT (0x2080): ローダーがスタブに書き換える */
    .org 0x80
    .globl __imp_GetStdHandle
    .set __imp_GetStdHandle, 0x140002080
    .quad 0x20C0
    .globl __imp_WriteFile
    .set __imp_WriteFile, 0x140002088
    .quad 0x20E0
    .globl __imp_ExitProcess
    .set __imp_ExitProcess, 0x140002090
    .quad 0x2100
    .globl __imp_CreateFileA
    .set __imp_CreateFileA, 0x140002098
    .quad 0x2120
    .globl __imp_ReadFile
    .set __imp_ReadFile, 0x1400020A0
    .quad 0x2140
    .globl __imp_CloseHandle
    .set __imp_CloseHandle, 0x1400020A8
    .quad 0x2160
    .quad 0
    /* hint/name */
    .org 0xC0
    .short 0
    .ascii "GetStdHandle"
    .byte 0
    .org 0xE0
    .short 0
    .ascii "WriteFile"
    .byte 0
    .org 0x100
    .short 0
    .ascii "ExitProcess"
    .byte 0
    .org 0x120
    .short 0
    .ascii "CreateFileA"
    .byte 0
    .org 0x140
    .short 0
    .ascii "ReadFile"
    .byte 0
    .org 0x160
    .short 0
    .ascii "CloseHandle"
    .byte 0
    /* DLL 名 */
    .org 0x1A0
    .ascii "KERNEL32.dll"
    .byte 0
    /* 書き込み/読み取りバイト数と読み取りバッファ */
    .org 0x1C0
    .globl written
written:
    .quad 0
    .globl nread
nread:
    .quad 0
    .globl rbuf
rbuf:
    .space 64
