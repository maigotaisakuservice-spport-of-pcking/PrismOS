/* PrismOS — 互換レイヤーの実証ゲスト: グラフィクス。
 *
 * ゲスト専用の窓 (640x480) へ描く呼び出しを使う:
 *   10 = fb_fill(x, y, w, h, color は r8)
 *   11 = fb_blit(x, y, w, h, pixels は r8)
 *   12 = fb_text(x, y, str, color は r10)
 *   13 = fb_flush()
 * 最後に stdout へ完了を出し、終了コード 0 で終わる。 */
    .text
    .globl _start
_start:
    /* 背景 */
    mov $10, %eax
    xor %edi, %edi
    xor %esi, %esi
    mov $640, %edx
    mov $480, %r10d
    mov $0x183048, %r8d
    int $0x80
    test %rax, %rax
    js gfxfail
    /* 赤い箱 */
    mov $10, %eax
    mov $40, %edi
    mov $40, %esi
    mov $200, %edx
    mov $120, %r10d
    mov $0xC0392B, %r8d
    int $0x80
    test %rax, %rax
    js gfxfail
    /* 青い箱 */
    mov $10, %eax
    mov $280, %edi
    mov $40, %esi
    mov $200, %edx
    mov $120, %r10d
    mov $0x2471A3, %r8d
    int $0x80
    test %rax, %rax
    js gfxfail
    /* 文字列 */
    mov $12, %eax
    mov $40, %edi
    mov $200, %esi
    lea text(%rip), %rdx
    mov $0xFFFFFF, %r10d
    int $0x80
    test %rax, %rax
    js gfxfail
    /* 市松模様を組み立てて blit する (16x16) */
    lea pixels(%rip), %rbx
    xor %ecx, %ecx          /* i = 0..255 */
1:  mov %ecx, %eax
    mov %ecx, %edx
    shr $4, %eax            /* y = i / 16 */
    and $15, %edx           /* x = i % 16 */
    xor %edx, %eax
    and $1, %eax
    mov $0xF1C40F, %edx     /* 黄 */
    mov $0x1B2631, %esi     /* 濃紺 */
    test %eax, %eax
    cmovz %esi, %edx
    mov %edx, (%rbx,%rcx,4)
    inc %ecx
    cmp $256, %ecx
    jb 1b
    mov $11, %eax
    mov $40, %edi
    mov $240, %esi
    mov $16, %edx
    mov $16, %r10d
    lea pixels(%rip), %r8
    int $0x80
    test %rax, %rax
    js gfxfail
    /* 画面へ反映 */
    mov $13, %eax
    int $0x80
    test %rax, %rax
    js gfxfail
    /* 完了を stdout へ */
    mov $1, %eax
    mov $1, %edi
    lea msg(%rip), %rsi
    mov $msg_len, %edx
    int $0x80
    mov $0, %eax
    xor %edi, %edi
    int $0x80
gfxfail:
    mov $1, %eax
    mov $1, %edi
    lea gmsg(%rip), %rsi
    mov $gmsg_len, %edx
    int $0x80
    mov $0, %eax
    mov $4, %edi
    int $0x80
1:  hlt
    jmp 1b
    .section .rodata
gmsg:
    .ascii "gfxdemo: gfx call failed\n"
    .set gmsg_len, . - gmsg
msg:
    .ascii "gfxdemo: done\n"
    .set msg_len, . - msg
text:
    .ascii "GFX FROM GUEST"
    .byte 0
    .bss
pixels:
    .space 256 * 4
