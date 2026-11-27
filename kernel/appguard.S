/* PrismOS — appguard の実体 (アセンブリ)。
 *
 * appguard_enter は callee-saved レジスタと rsp・戻り先を
 * 文脈スタックに保存して 0 を返す。appguard_bail は
 * いちばん新しい文脈を復元し、enter の呼び出し元へ
 * 1 を返して戻る (compat_gate.S の .Lreturn と同じ考え方)。
 * 入れ子 (入力処理中の再描画など) に備えて深さ 4 まで
 * 積める。それ以上は武装せず 0 を返す (封じ込めなしで
 * そのまま実行する)。 */
    .text
    .globl appguard_enter
    .globl appguard_leave
    .globl appguard_armed
    .globl appguard_bail

    .equ CTX_SIZE, 64
    .equ CTX_MAX, 4

appguard_enter:
    mov appguard_depth(%rip), %eax
    cmp $CTX_MAX, %eax
    jae 1f
    lea appguard_ctxs(%rip), %rcx
    mov %eax, %edx
    shl $6, %rdx            /* depth * 64 */
    add %rdx, %rcx
    mov %rbx, 0(%rcx)
    mov %rbp, 8(%rcx)
    mov %r12, 16(%rcx)
    mov %r13, 24(%rcx)
    mov %r14, 32(%rcx)
    mov %r15, 40(%rcx)
    mov %rsp, 48(%rcx)
    mov (%rsp), %rdx
    mov %rdx, 56(%rcx)      /* 戻り先 */
    inc %eax
    mov %eax, appguard_depth(%rip)
    xor %eax, %eax
    ret
1:  xor %eax, %eax
    ret

appguard_leave:
    mov appguard_depth(%rip), %eax
    test %eax, %eax
    jz 1f
    dec %eax
    mov %eax, appguard_depth(%rip)
1:  ret

appguard_armed:
    xor %eax, %eax
    cmpl $0, appguard_depth(%rip)
    setne %al
    ret

appguard_bail:
    mov appguard_depth(%rip), %eax
    test %eax, %eax
    jz 2f                   /* 武装が無いのに呼ばれたら止める */
    dec %eax
    mov %eax, appguard_depth(%rip)
    lea appguard_ctxs(%rip), %rcx
    mov %eax, %edx
    shl $6, %rdx
    add %rdx, %rcx
    mov 0(%rcx), %rbx
    mov 8(%rcx), %rbp
    mov 16(%rcx), %r12
    mov 24(%rcx), %r13
    mov 32(%rcx), %r14
    mov 40(%rcx), %r15
    mov 48(%rcx), %rsp
    mov 56(%rcx), %rdx
    /* 保存した rsp は戻りアドレスのスロットを指す。
     * 通常の ret が済んだ直後の状態に合わせるため +8
     * する (これをしないと呼び出し側のエピローグが
     * ずれて、ゴミへ飛ぶ)。 */
    lea 8(%rsp), %rsp
    mov $1, %eax
    jmp *%rdx
2:  cli
3:  hlt
    jmp 3b

    .bss
appguard_ctxs:
    .skip CTX_SIZE * CTX_MAX
appguard_depth:
    .skip 4
