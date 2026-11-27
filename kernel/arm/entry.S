/* PrismOS — ARM カーネル入口 (AArch64)
 *
 * AArch64 のカーネルイメージヘッダ (先頭 64 バイト) を持つ
 * raw イメージとして起動する。起動側 (QEMU や実機のブート
 * ローダ) はヘッダのマジックで通常のカーネルイメージと
 * 認識し、起動規約に従って x0 に DTB のアドレスを入れて
 * イメージ先頭へ跳ぶ。ELF のまま -kernel で渡すと起動側は
 * 起動規約を適用せず x0 = 0 になる (QEMU の hw/arm/boot.c で
 * ELF は通常イメージ扱いされない) ため、raw イメージが要る。
 *
 * 入り口の例外レベルは構成で変わる (EL3 / EL2 / EL1 のどれか)。
 * どのレベルから始まっても EL1 まで落としてから C の arm_main() を呼ぶ。
 *  * EL3 から: SCR_EL3 で下位を 64 ビットにし、EL2h へ eret する。
 *  * EL2 から: HCR_EL2.RW で EL1 を 64 ビットにし、汎用タイマを
 *    EL1 から触れるよう CNTHCTL_EL2 を開けて、EL1h へ eret する。
 *  * EL1 から: そのまま進む。
 *
 * スタックは 32KB (.bss 内)。bss を消してから arm_main() を呼ぶ。
 */
    .section .text.boot
    .global _start
_start:
    /* --- カーネルイメージヘッダ (64 バイト) --- */
    b arm_start                 /* code0: ヘッダを飛び越える */
    .word 0                     /* code1: 予約 */
    .quad 0x80000               /* text_offset: RAM 先頭から 512KB */
    .quad __kernel_end - _start /* image_size: 全体の大きさ */
    .quad 0                     /* flags */
    .quad 0                     /* res2 */
    .quad 0                     /* res3 */
    .quad 0                     /* res4 */
    .word 0x644d5241            /* magic: "ARM\x64" */
    .word 0                     /* res5 */

    .section .text
arm_start:
    /* x0 は DTB のアドレス (起動規約)。
     * EL を落とす処理は x0 を使い潰すので、先に x19 へ退避する
     * (x19 は例外レベルをまたいでも保持される)。 */
    mov x19, x0
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #3
    b.eq el3_entry
    cmp x0, #2
    b.eq el2_entry
    b el1_entry

el3_entry:
    /* RW (bit 10): 下位を 64 ビットに。HCE (bit 8): HVC を許可。
     * NS (bit 0): 非セキュア側へ。 */
    mov x0, #0x501
    msr scr_el3, x0
    /* SPSR: EL2h (M=1001)、割り込みは全部ふさぐ。 */
    mov x0, #0x3c9
    msr spsr_el3, x0
    adr x0, el2_entry
    msr elr_el3, x0
    eret

el2_entry:
    /* HCR_EL2.RW (bit 31): EL1 を 64 ビットにする。 */
    mov x0, #0x80000000
    msr hcr_el2, x0
    /* CNTHCTL_EL2: EL1PCTEN (bit 0) と EL1PCEN (bit 1) を立て、
     * EL1 から汎用タイマのカウンタと制御を触れるようにする。
     * これを忘れると EL1 からのタイマ操作が EL2 へ飛ぶ。 */
    mov x0, #0x3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    /* SPSR: EL1h (M=0101)、割り込みは全部ふさぐ。 */
    mov x0, #0x3c5
    msr spsr_el2, x0
    adr x0, el1_entry
    msr elr_el2, x0
    eret

el1_entry:
    ldr x0, =stack_top
    mov sp, x0
    /* .bss を消す。スタック領域も .bss 内だが、まだ何も積んで
     * いないので消して構わない。 */
    ldr x0, =__bss_start
    ldr x1, =__bss_end
1:
    cmp x0, x1
    b.hs 2f
    str xzr, [x0], #8
    b 1b
2:
    /* arm_main(dtb_addr): 退避しておいた DTB を第1引数に戻す。 */
    mov x0, x19
    bl arm_main
    /* arm_main() は戻らないはず。戻ったら止まる。 */
    wfi
    b 2b

    .section .bss
    .align 16
/* カーネルスタックは 32KB。x86 側と同じ理由 (深い呼び出しで
 * 溢れた前例があるため)。 */
stack_bottom:
    .skip 32768
stack_top:
