/* PrismOS — 互換レイヤーの実証ゲスト 7: 動的リンク。
 * libguest.so に依存する PIE 実行ファイル。
 * guest_add@PLT (JUMP_SLOT) と guest_counter@GOTPCREL
 * (GLOB_DAT) をローダーが解決し、ライブラリの
 * .init_array が先に走ることを確かめる。 */
    .text
    .globl _start
_start:
    mov $20, %edi
    mov $22, %esi
    call guest_add@PLT
    cmp $42, %eax
    jne 9f
    mov guest_counter@GOTPCREL(%rip), %rax
    cmpl $7, (%rax)
    jne 9f
    mov $1, %eax
    mov $1, %edi
    lea okmsg(%rip), %rsi
    mov $okmsg_len, %edx
    int $0x80
    mov $0, %eax
    xor %edi, %edi
    int $0x80
9:  mov $0, %eax
    mov $3, %edi
    int $0x80
    .section .rodata
okmsg: .ascii "dyn link ok: sum and counter match\n"
    .set okmsg_len, . - okmsg
