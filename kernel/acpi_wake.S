/* PrismOS — ACPI S3 復帰スタブ。
 *
 * このバイト列を物理 0x9000 にコピーし、FACS の
 * firmware_waking_vector に 0x9000 を書いておく。S3 から
 * 目覚めるとファームウェア (SeaBIOS) がここへ実モードで
 * 跳んでくる。AP トランポリンと同じ遷移をたどり、最後は
 * カーネル本来の GDT/IDT を読み直してから、保存済みの
 * CR3 とスタックで acpi_s3_resume へ far jump する。
 *
 * データスロットはこのブロブ内 (acpi.c が extern で参照)。
 */
    .section .rodata.acpi_wake, "a"
    .globl acpi_wake_blob
    .globl acpi_wake_blob_end
    .globl acpi_wake_cr3_slot
    .globl acpi_wake_rsp_slot
    .globl acpi_wake_entry_slot
    .globl acpi_wake_gdt_ptr
    .globl acpi_wake_idt_ptr

    .code16
acpi_wake_blob:
    cli
    cld
    movw $0x0900, %ax
    movw %ax, %ds
    movw %ax, %ss
    movw $0xFF0, %sp
    lgdt wake_gdt32_desc - acpi_wake_blob
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    .byte 0x66, 0xEA
    .long 0x9000 + (wake_prot32 - acpi_wake_blob)
    .word 0x08

    .code32
wake_prot32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4
    movl 0x9000 + (wake_cr3_slot - acpi_wake_blob), %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0
    lgdt 0x9000 + (wake_gdt64_desc - acpi_wake_blob)
    .byte 0xEA
    .long 0x9000 + (wake_long64 - acpi_wake_blob)
    .word 0x08

    .code64
wake_long64:
    /* カーネル本来の GDT / IDT に載せ替える (スロットは
     * acpi.c が sgdt / sidt の値を書いてある)。 */
    lgdt 0x9000 + (acpi_wake_gdt_ptr - acpi_wake_blob)
    lidt 0x9000 + (acpi_wake_idt_ptr - acpi_wake_blob)
    movw $0x30, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movq 0x9000 + (wake_rsp_slot - acpi_wake_blob), %rsp
    movq 0x9000 + (wake_entry_slot - acpi_wake_blob), %rax
    /* CS もカーネルコード (0x28) に載せ替えて入口へ。 */
    pushq $0x28
    pushq %rax
    lretq

    .align 8
wake_gdt32:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
wake_gdt32_desc:
    .word wake_gdt32_desc - wake_gdt32 - 1
    .long 0x9000 + (wake_gdt32 - acpi_wake_blob)
wake_gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
wake_gdt64_desc:
    .word wake_gdt64_desc - wake_gdt64 - 1
    .long 0x9000 + (wake_gdt64 - acpi_wake_blob)

    .align 8
wake_cr3_slot:
acpi_wake_cr3_slot:
    .quad 0
wake_rsp_slot:
acpi_wake_rsp_slot:
    .quad 0
wake_entry_slot:
acpi_wake_entry_slot:
    .quad 0
    .align 8
acpi_wake_gdt_ptr:
    .word 0
    .quad 0
acpi_wake_idt_ptr:
    .word 0
    .quad 0
acpi_wake_blob_end:
