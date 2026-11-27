/* PrismOS — ACPI S3 の保存/復帰スイッチ。
 *
 * acpi_s3_switch(port, value, ctx):
 *   ctx[0] に CR3、ctx[1] に RSP を書いてから PM1a_CNT へ
 *   value を書く。ctx は復帰スタブ (物理 0x9000 のブロブ内
 *   スロット) への HHDM ポインタで、スタブがそのまま読む。
 *   機械が止まらなければ 0 で戻る。S3 から復帰した場合は
 *   スタブが保存したスタックで acpi_s3_resume に入り、
 *   レジスタを復元して 1 を返して戻る (setjmp 型の契約)。
 */
    .text
    .globl acpi_s3_switch
    .globl acpi_s3_resume
acpi_s3_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %cr3, %rax
    mov %rax, 0(%rdx)
    mov %rsp, 8(%rdx)
    mov %si, %ax
    mov %di, %dx
    out %ax, %dx
    /* 止まらなかった場合の経路。 */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    xor %eax, %eax
    ret
acpi_s3_resume:
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    mov $1, %eax
    ret
