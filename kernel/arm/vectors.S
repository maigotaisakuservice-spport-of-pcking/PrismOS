/* PrismOS — ARM 例外ベクタ表 (AArch64, VBAR_EL1)
 *
 * 16 個の入口が 128 バイトずつ並ぶ。使うのは同期例外と IRQ
 * (EL1h) だけで、他も同じ扱いにしてある。
 *  * 同期例外: ESR_EL1 と ELR_EL1 を C の arm_sync_handler() に
 *   渡し、原因をシリアルに出して止まる。
 *  * IRQ: 呼び出し側保存レジスタを退避して C の arm_irq_handler()
 *   を呼び、復帰して eret する。
 *  * FIQ: このカーネルでは FIQ を使わない。来たら止まる。
 */
    .section .text
    .align 11
    .global arm_vectors
arm_vectors:
    /* 同期例外 (EL1t) */
    b sync_entry
    .align 7
    /* IRQ (EL1t) */
    b irq_entry
    .align 7
    /* FIQ (EL1t) */
    b fiq_hang
    .align 7
    /* SError (EL1t) */
    b sync_entry
    .align 7
    /* 同期例外 (EL1h) */
    b sync_entry
    .align 7
    /* IRQ (EL1h) */
    b irq_entry
    .align 7
    /* FIQ (EL1h) */
    b fiq_hang
    .align 7
    /* SError (EL1h) */
    b sync_entry
    .align 7
    /* 同期例外 (EL0, 64 ビット) */
    b sync_entry
    .align 7
    /* IRQ (EL0, 64 ビット) */
    b irq_entry
    .align 7
    /* FIQ (EL0, 64 ビット) */
    b fiq_hang
    .align 7
    /* SError (EL0, 64 ビット) */
    b sync_entry
    .align 7
    /* 同期例外 (EL0, 32 ビット) */
    b sync_entry
    .align 7
    /* IRQ (EL0, 32 ビット) */
    b irq_entry
    .align 7
    /* FIQ (EL0, 32 ビット) */
    b fiq_hang
    .align 7
    /* SError (EL0, 32 ビット) */
    b sync_entry
    .align 7

sync_entry:
    mrs x0, esr_el1
    mrs x1, elr_el1
    bl arm_sync_handler
    /* arm_sync_handler() は戻らない。 */
1:
    wfi
    b 1b

irq_entry:
    stp x0, x1, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    stp x8, x9, [sp, #-16]!
    stp x10, x11, [sp, #-16]!
    stp x12, x13, [sp, #-16]!
    stp x14, x15, [sp, #-16]!
    stp x16, x17, [sp, #-16]!
    str x18, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    bl arm_irq_handler
    ldp x29, x30, [sp], #16
    ldr x18, [sp], #16
    ldp x16, x17, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x8, x9, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x0, x1, [sp], #16
    eret

fiq_hang:
1:
    wfi
    b 1b
