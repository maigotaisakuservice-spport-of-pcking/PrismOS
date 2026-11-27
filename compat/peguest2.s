/* PrismOS — PE ゲスト第 2 弾 (コンソール、拡大 API の実証)。
 *
 * KERNEL32.dll の拡大サブセットを使う:
 *   GetModuleFileNameA / GetVersion / GetSystemTime /
 *   SetEnvironmentVariableA / GetEnvironmentVariableA /
 *   GetCurrentDirectoryA / GetComputerNameA / GetSystemInfo /
 *   GetDiskFreeSpaceA / FindFirstFileA / FindClose /
 *   FormatMessageA / WriteConsoleW / lstrlenA /
 *   GetFileSize / GetFileAttributesA (CreateFileA/WriteFile/
 *   ReadFile/CloseHandle と組み合わせて)。
 * 各検査は成功時に 1 行出し、失敗時は固有の終了コードで
 * 終わる (21 以降)。全て通れば ExitProcess(11)。
 * 配置は build-pe2-guest.py のリンカスクリプトと一致させる:
 *   .text  = RVA 0x1000、.idata = RVA 0x2000、
 *   イメージ ベース 0x140000000。 */
    .text
    .globl _start
_start:
    sub $56, %rsp
    mov $-11, %ecx              /* STD_OUTPUT_HANDLE */
    call *__imp_GetStdHandle(%rip)
    mov %rax, %rbx              /* 標準出力ハンドル */

    /* GetModuleFileNameA(NULL, buf, 64) -> 13 */
    xor %ecx, %ecx
    lea buf(%rip), %rdx
    mov $64, %r8d
    call *__imp_GetModuleFileNameA(%rip)
    cmp $13, %rax
    jne fail21
    mov %rbx, %rcx
    lea buf(%rip), %rdx
    mov $13, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)
    mov %rbx, %rcx
    lea nl(%rip), %rdx
    mov $1, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* GetVersion -> 下位バイト 10 */
    call *__imp_GetVersion(%rip)
    cmp $10, %al
    jne fail22
    mov %rbx, %rcx
    lea msg_ver(%rip), %rdx
    mov $msg_ver_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* GetSystemTime(buf) -> 年 >= 2020 */
    lea buf(%rip), %rcx
    call *__imp_GetSystemTime(%rip)
    movzwl buf(%rip), %eax
    cmp $2020, %eax
    jb fail23
    mov %rbx, %rcx
    lea msg_time(%rip), %rdx
    mov $msg_time_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* 環境変数: PE2VAR=yes を入れて読む */
    lea env_name(%rip), %rcx
    lea env_val(%rip), %rdx
    call *__imp_SetEnvironmentVariableA(%rip)
    cmp $1, %rax
    jne fail24
    lea env_name(%rip), %rcx
    lea buf(%rip), %rdx
    mov $64, %r8d
    call *__imp_GetEnvironmentVariableA(%rip)
    cmp $3, %rax
    jne fail24
    cmpb $'y', buf(%rip)
    jne fail24
    cmpb $'e', buf+1(%rip)
    jne fail24
    cmpb $'s', buf+2(%rip)
    jne fail24
    mov %rbx, %rcx
    lea msg_env(%rip), %rdx
    mov $msg_env_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* GetCurrentDirectoryA(64, buf) -> 2、".\" */
    mov $64, %ecx
    lea buf(%rip), %rdx
    call *__imp_GetCurrentDirectoryA(%rip)
    cmp $2, %rax
    jne fail25
    cmpb $'.', buf(%rip)
    jne fail25
    cmpb $92, buf+1(%rip)       /* '\\' */
    jne fail25
    mov %rbx, %rcx
    lea msg_cwd(%rip), %rdx
    mov $msg_cwd_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* GetComputerNameA(buf, &size32) -> "PRISM" */
    movl $64, size32(%rip)
    lea buf(%rip), %rcx
    lea size32(%rip), %rdx
    call *__imp_GetComputerNameA(%rip)
    cmp $1, %rax
    jne fail26
    cmpl $5, size32(%rip)
    jne fail26
    cmpb $'P', buf(%rip)
    jne fail26
    mov %rbx, %rcx
    lea msg_name(%rip), %rdx
    mov $msg_name_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* GetSystemInfo(buf) -> arch 9 */
    lea buf(%rip), %rcx
    call *__imp_GetSystemInfo(%rip)
    movzwl buf(%rip), %eax
    cmp $9, %eax
    jne fail27
    mov %rbx, %rcx
    lea msg_sys(%rip), %rdx
    mov $msg_sys_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* GetDiskFreeSpaceA(NULL, &spc, &bps, &free, &total) */
    xor %ecx, %ecx
    lea spc(%rip), %rdx
    lea bps(%rip), %r8
    lea nfree(%rip), %r9
    lea ntotal(%rip), %rax
    movq %rax, 32(%rsp)
    call *__imp_GetDiskFreeSpaceA(%rip)
    cmp $1, %rax
    jne fail28
    cmpl $8, spc(%rip)
    jne fail28
    cmpl $512, bps(%rip)
    jne fail28
    mov %rbx, %rcx
    lea msg_disk(%rip), %rdx
    mov $msg_disk_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* FindFirstFileA("about.txt", finddata) -> 0x200 */
    lea fname(%rip), %rcx
    lea finddata(%rip), %rdx
    call *__imp_FindFirstFileA(%rip)
    cmp $0x200, %rax
    jne fail29
    mov %rax, %r12              /* 検索ハンドル */
    cmpb $'a', finddata+44(%rip)
    jne fail30
    cmpb $'b', finddata+45(%rip)
    jne fail30
    mov %r12, %rcx
    call *__imp_FindClose(%rip)
    cmp $1, %rax
    jne fail31
    mov %rbx, %rcx
    lea msg_find(%rip), %rdx
    mov $msg_find_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* FormatMessageA(0x1000, NULL, 2, 0, buf, 64) -> 15 */
    mov $0x1000, %ecx
    xor %edx, %edx
    mov $2, %r8d
    xor %r9d, %r9d
    lea buf(%rip), %rax
    movq %rax, 32(%rsp)
    movq $64, 40(%rsp)
    call *__imp_FormatMessageA(%rip)
    cmp $15, %rax
    jne fail32
    mov %rbx, %rcx
    lea msg_fmt(%rip), %rdx
    mov $msg_fmt_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* WriteConsoleW(1, "W2\n", 3, &written) */
    mov %rbx, %rcx
    lea wbuf(%rip), %rdx
    mov $3, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteConsoleW(%rip)
    cmp $1, %rax
    jne fail33

    /* lstrlenA("hello") -> 5 */
    lea str_hello(%rip), %rcx
    call *__imp_lstrlenA(%rip)
    cmp $5, %rax
    jne fail34
    mov %rbx, %rcx
    lea msg_str(%rip), %rdx
    mov $msg_str_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    /* ファイル: pe2out.txt へ書き、閉じ、読み直して確かめる */
    lea fname2(%rip), %rcx
    mov $0x40000000, %edx       /* GENERIC_WRITE */
    xor %r8d, %r8d
    xor %r9d, %r9d
    movq $2, 32(%rsp)           /* CREATE_ALWAYS */
    movq $0, 40(%rsp)
    movq $0, 48(%rsp)
    call *__imp_CreateFileA(%rip)
    cmp $-1, %rax
    je fail35
    mov %rax, %r12
    mov %r12, %rcx
    lea fdata(%rip), %rdx
    mov $7, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)
    cmp $1, %rax
    jne fail35
    mov %r12, %rcx
    call *__imp_CloseHandle(%rip)
    lea fname2(%rip), %rcx
    mov $0x80000000, %edx       /* GENERIC_READ */
    xor %r8d, %r8d
    xor %r9d, %r9d
    movq $3, 32(%rsp)           /* OPEN_EXISTING */
    movq $0, 40(%rsp)
    movq $0, 48(%rsp)
    call *__imp_CreateFileA(%rip)
    cmp $-1, %rax
    je fail35
    mov %rax, %r12
    mov %r12, %rcx
    xor %edx, %edx              /* lpFileSizeHigh = NULL */
    call *__imp_GetFileSize(%rip)
    cmp $7, %rax
    jne fail35
    lea fname2(%rip), %rcx
    call *__imp_GetFileAttributesA(%rip)
    cmp $0x80, %rax
    jne fail36
    mov %r12, %rcx
    call *__imp_CloseHandle(%rip)
    mov %rbx, %rcx
    lea msg_file(%rip), %rdx
    mov $msg_file_len, %r8d
    lea written(%rip), %r9
    movq $0, 32(%rsp)
    call *__imp_WriteFile(%rip)

    mov $11, %ecx
    call *__imp_ExitProcess(%rip)
1:  hlt
    jmp 1b

fail21:
    mov $21, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail22:
    mov $22, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail23:
    mov $23, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail24:
    mov $24, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail25:
    mov $25, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail26:
    mov $26, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail27:
    mov $27, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail28:
    mov $28, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail29:
    mov $29, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail30:
    mov $30, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail31:
    mov $31, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail32:
    mov $32, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail33:
    mov $33, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail34:
    mov $34, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail35:
    mov $35, %ecx
    call *__imp_ExitProcess(%rip)
    hlt
fail36:
    mov $36, %ecx
    call *__imp_ExitProcess(%rip)
    hlt

nl:
    .byte 10
msg_ver:
    .ascii "ver ok\n"
    .set msg_ver_len, . - msg_ver
msg_time:
    .ascii "time ok\n"
    .set msg_time_len, . - msg_time
msg_env:
    .ascii "env ok\n"
    .set msg_env_len, . - msg_env
msg_cwd:
    .ascii "cwd ok\n"
    .set msg_cwd_len, . - msg_cwd
msg_name:
    .ascii "name ok\n"
    .set msg_name_len, . - msg_name
msg_sys:
    .ascii "sysinfo ok\n"
    .set msg_sys_len, . - msg_sys
msg_disk:
    .ascii "disk ok\n"
    .set msg_disk_len, . - msg_disk
msg_find:
    .ascii "find ok\n"
    .set msg_find_len, . - msg_find
msg_fmt:
    .ascii "fmt ok\n"
    .set msg_fmt_len, . - msg_fmt
msg_str:
    .ascii "str ok\n"
    .set msg_str_len, . - msg_str
msg_file:
    .ascii "file ok\n"
    .set msg_file_len, . - msg_file
env_name:
    .ascii "PE2VAR"
    .byte 0
env_val:
    .ascii "yes"
    .byte 0
fname:
    .ascii "about.txt"
    .byte 0
fname2:
    .ascii "pe2out.txt"
    .byte 0
str_hello:
    .ascii "hello"
    .byte 0
fdata:
    .ascii "pe2data"

    /* ---- インポート (RVA 0x2000 から) ---- */
    .section .idata,"aw"
    /* ディスクリプタ (1 DLL + 終端) */
    .org 0x00
    .long 0x2040                /* OriginalFirstThunk (ILT) */
    .long 0                     /* TimeDateStamp */
    .long 0                     /* ForwarderChain */
    .long 0x2480                /* Name */
    .long 0x2100                /* FirstThunk (IAT) */
    .org 0x14
    .long 0, 0, 0, 0, 0         /* 終端ディスクリプタ */
    /* ILT (0x2040): hint/name RVA は 0x21C0 + i*0x20 */
    .org 0x40
    .quad 0x21C0
    .quad 0x21E0
    .quad 0x2200
    .quad 0x2220
    .quad 0x2240
    .quad 0x2260
    .quad 0x2280
    .quad 0x22A0
    .quad 0x22C0
    .quad 0x22E0
    .quad 0x2300
    .quad 0x2320
    .quad 0x2340
    .quad 0x2360
    .quad 0x2380
    .quad 0x23A0
    .quad 0x23C0
    .quad 0x23E0
    .quad 0x2400
    .quad 0x2420
    .quad 0x2440
    .quad 0x2460
    .quad 0
    /* IAT (0x2100): ローダーがスタブに書き換える */
    .org 0x100
    .globl __imp_GetStdHandle
    .set __imp_GetStdHandle, 0x140002100
    .quad 0x21C0
    .globl __imp_WriteFile
    .set __imp_WriteFile, 0x140002108
    .quad 0x21E0
    .globl __imp_ExitProcess
    .set __imp_ExitProcess, 0x140002110
    .quad 0x2200
    .globl __imp_CreateFileA
    .set __imp_CreateFileA, 0x140002118
    .quad 0x2220
    .globl __imp_ReadFile
    .set __imp_ReadFile, 0x140002120
    .quad 0x2240
    .globl __imp_CloseHandle
    .set __imp_CloseHandle, 0x140002128
    .quad 0x2260
    .globl __imp_GetModuleFileNameA
    .set __imp_GetModuleFileNameA, 0x140002130
    .quad 0x2280
    .globl __imp_GetVersion
    .set __imp_GetVersion, 0x140002138
    .quad 0x22A0
    .globl __imp_GetSystemTime
    .set __imp_GetSystemTime, 0x140002140
    .quad 0x22C0
    .globl __imp_SetEnvironmentVariableA
    .set __imp_SetEnvironmentVariableA, 0x140002148
    .quad 0x22E0
    .globl __imp_GetEnvironmentVariableA
    .set __imp_GetEnvironmentVariableA, 0x140002150
    .quad 0x2300
    .globl __imp_GetCurrentDirectoryA
    .set __imp_GetCurrentDirectoryA, 0x140002158
    .quad 0x2320
    .globl __imp_GetComputerNameA
    .set __imp_GetComputerNameA, 0x140002160
    .quad 0x2340
    .globl __imp_GetSystemInfo
    .set __imp_GetSystemInfo, 0x140002168
    .quad 0x2360
    .globl __imp_GetDiskFreeSpaceA
    .set __imp_GetDiskFreeSpaceA, 0x140002170
    .quad 0x2380
    .globl __imp_FindFirstFileA
    .set __imp_FindFirstFileA, 0x140002178
    .quad 0x23A0
    .globl __imp_FindClose
    .set __imp_FindClose, 0x140002180
    .quad 0x23C0
    .globl __imp_FormatMessageA
    .set __imp_FormatMessageA, 0x140002188
    .quad 0x23E0
    .globl __imp_WriteConsoleW
    .set __imp_WriteConsoleW, 0x140002190
    .quad 0x2400
    .globl __imp_lstrlenA
    .set __imp_lstrlenA, 0x140002198
    .quad 0x2420
    .globl __imp_GetFileSize
    .set __imp_GetFileSize, 0x1400021A0
    .quad 0x2440
    .globl __imp_GetFileAttributesA
    .set __imp_GetFileAttributesA, 0x1400021A8
    .quad 0x2460
    .quad 0
    /* hint/name (0x21C0 から 0x20 刻み) */
    .org 0x1C0
    .short 0
    .ascii "GetStdHandle"
    .byte 0
    .org 0x1E0
    .short 0
    .ascii "WriteFile"
    .byte 0
    .org 0x200
    .short 0
    .ascii "ExitProcess"
    .byte 0
    .org 0x220
    .short 0
    .ascii "CreateFileA"
    .byte 0
    .org 0x240
    .short 0
    .ascii "ReadFile"
    .byte 0
    .org 0x260
    .short 0
    .ascii "CloseHandle"
    .byte 0
    .org 0x280
    .short 0
    .ascii "GetModuleFileNameA"
    .byte 0
    .org 0x2A0
    .short 0
    .ascii "GetVersion"
    .byte 0
    .org 0x2C0
    .short 0
    .ascii "GetSystemTime"
    .byte 0
    .org 0x2E0
    .short 0
    .ascii "SetEnvironmentVariableA"
    .byte 0
    .org 0x300
    .short 0
    .ascii "GetEnvironmentVariableA"
    .byte 0
    .org 0x320
    .short 0
    .ascii "GetCurrentDirectoryA"
    .byte 0
    .org 0x340
    .short 0
    .ascii "GetComputerNameA"
    .byte 0
    .org 0x360
    .short 0
    .ascii "GetSystemInfo"
    .byte 0
    .org 0x380
    .short 0
    .ascii "GetDiskFreeSpaceA"
    .byte 0
    .org 0x3A0
    .short 0
    .ascii "FindFirstFileA"
    .byte 0
    .org 0x3C0
    .short 0
    .ascii "FindClose"
    .byte 0
    .org 0x3E0
    .short 0
    .ascii "FormatMessageA"
    .byte 0
    .org 0x400
    .short 0
    .ascii "WriteConsoleW"
    .byte 0
    .org 0x420
    .short 0
    .ascii "lstrlenA"
    .byte 0
    .org 0x440
    .short 0
    .ascii "GetFileSize"
    .byte 0
    .org 0x460
    .short 0
    .ascii "GetFileAttributesA"
    .byte 0
    /* DLL 名 */
    .org 0x480
    .ascii "KERNEL32.dll"
    .byte 0
    /* 作業領域 */
    .org 0x4A0
    .globl written
written:
    .quad 0
    .globl nread
nread:
    .quad 0
    .globl rbuf
rbuf:
    .space 64
    .globl buf
buf:
    .space 128
    .globl finddata
finddata:
    .space 320
    .globl wbuf
wbuf:
    .short 0x57, 0x32, 0x0A      /* "W2\n" (UTF-16) */
    .globl spc
spc:
    .long 0
    .globl bps
bps:
    .long 0
    .globl nfree
nfree:
    .long 0
    .globl ntotal
ntotal:
    .long 0
    .globl size32
size32:
    .long 0
