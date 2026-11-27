/* PrismOS — 互換レイヤーの入口 (アセンブリ)
 *
 * int $0x80 のゲートと、ゲストの入口を呼ぶ関数。
 * ゲストはサンドボックスで動く: リング 3 + ゲスト専用の
 * ページテーブル (CR3 は compat_guest_cr3 に入れておく)。
 *
 * ゲートに入るときのスタック: ゲストがリング 3 にいるため、
 * CPU は TSS の RSP0 (カーネルスタック) に切り替え、SS/RSP/
 * RFLAGS/CS/RIP の 5 ワードを積む。汎用レジスタを全部積んで
 * C の dispatcher に渡す。戻り値は保存しておいた rax の場所
 * に書き戻す。ゲストが exit を呼んだら、dispatcher が立てた
 * 合図を見てランナーに直接戻る (積んだレジスタはゲストの
 * スタックごと捨てる)。
 * 注意: ゲートは割り込みゲート (0xEE) なので入った時点で
 * IF が落ちる。通常の経路は iretq で RFLAGS を復元する
 * が、終了経路はスタックを捨てて ret で戻るため、ここで
 * 明示的に sti して割り込みを戻さないと、ゲスト終了後に
 * キーボード等の割り込みが二度と届かなくなる。
 *
 * compat_call_entry はカーネルのスタックと CR3 を保存し、
 * ゲストの CR3 に切り替えてから iretq でリング 3 の入口へ
 * 落ちる。ゲストが終わる経路は 3 つ:
 *   1. exit の呼び出し → ゲートの .Lexit が CR3 とスタック
 *      を戻してランナーへ ret する。
 *   2. 例外 (カーネルメモリへのアクセス等) → 例外ハンドラ
 *      が compat_guest_bail を呼ぶ。
 *   3. 暴走 → ウォッチドッグが compat_guest_bail を呼ぶ。
 * 2 と 3 も CR3 とスタックを戻してランナーへ ret する。
 * どの経路でもカーネルは生き続ける。
 */
    .text
    .globl compat_syscall_gate
    .globl compat_call_entry
    .globl compat_guest_bail

compat_syscall_gate:
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rdi
    push %rsi
    push %rbp
    push %rbx
    push %rdx
    push %rcx
    push %rax
    mov %rsp, %rbx
    mov %rsp, %rdi
    and $-16, %rsp
    call compat_syscall_dispatch_c
    mov %rbx, %rsp
    cmp $0, compat_exit_pending(%rip)
    jne .Lexit
    mov %rax, (%rsp)        /* 戻り値を保存した rax に書く */
    pop %rax
    pop %rcx
    pop %rdx
    pop %rbx
    pop %rbp
    pop %rsi
    pop %rdi
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    iretq
.Lexit:
    mov compat_exit_code(%rip), %eax
    jmp .Lreturn

/* int compat_call_entry(uint64_t entry, uint64_t stack_top);
 * ゲストの CR3 (compat_guest_cr3) に切り替え、iretq で
 * リング 3 の入口へ落ちる。スタックは本物のプロセス起動と
 * 同じ並びで、rsp が argc を指す。戻りは .Lexit か
 * compat_guest_bail 経由で、終了コードが eax に入る。
 * callee-saved レジスタ (rbx, rbp, r12-r15) は入口で積んで
 * .Lreturn で戻す。終了経路はゲスト側のスタックを捨てる
 * ため、これをしないと呼び出し側の変数が壊れる。 */
compat_call_entry:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, compat_saved_rsp(%rip)
    mov %cr3, %rax
    mov %rax, compat_saved_cr3(%rip)
    mov compat_guest_cr3(%rip), %rax
    mov %rax, %cr3
    push $0x3B              /* ユーザーデータセグメント | RPL3 */
    push %rsi               /* ゲストのスタック先頭 */
    push $0x202             /* RFLAGS: IF=1、予約ビット */
    push $0x43              /* ユーザーコードセグメント | RPL3 */
    push %rdi               /* 入口 RIP */
    xor %rbp, %rbp
    xor %rax, %rax
    xor %rcx, %rcx
    xor %rdx, %rdx
    xor %rsi, %rsi
    xor %rdi, %rdi
    xor %r8, %r8
    xor %r9, %r9
    xor %r10, %r10
    xor %r11, %r11
    iretq

/* void compat_guest_bail(int code) — 戻らない。
 * 例外ハンドラとウォッチドッグが、ゲストを見捨てるときに
 * 呼ぶ。.Lreturn へ合流し、code が compat_call_entry の
 * 戻り値になる。 */
compat_guest_bail:
    mov %edi, %eax
    jmp .Lreturn

/* 終了コード (eax) を保ったまま、CR3 とスタックと
 * callee-saved レジスタをランナーのものに戻して ret する。 */
.Lreturn:
    mov compat_saved_rsp(%rip), %rsp
    mov compat_saved_cr3(%rip), %rcx
    mov %rcx, %cr3          /* カーネルのページテーブルに戻す */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    sti                 /* 割り込みゲートで落ちた IF を戻す */
    ret

    .section .bss
    .align 8
    .globl compat_saved_rsp
compat_saved_rsp:
    .skip 8
    .globl compat_saved_cr3
compat_saved_cr3:
    .skip 8
    .globl compat_guest_cr3
compat_guest_cr3:
    .skip 8
