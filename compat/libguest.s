/* PrismOS — 互換レイヤーの実証用共有ライブラリ
 * (アセンブリで手書き)。動的リンクの検証に使う:
 *   - guest_add: 関数 (本体から PLT 経由で呼ぶ)
 *   - guest_counter: データ (本体が GOT 経由で読む)
 *   - guest_init_marker: .init_array に登録した初期化
 *     関数 (載せたときにローダーが呼ぶ。write で知らせる)
 * 呼び出し規約は hello.s と同じ (int $0x80、1 = write、
 * 0 = exit)。 */
    .text
    .globl guest_add
    .type guest_add, @function
guest_add:
    lea (%rdi, %rsi), %eax
    ret
    .globl guest_init_marker
    .type guest_init_marker, @function
guest_init_marker:
    mov $1, %eax
    mov $1, %edi
    lea gimsg(%rip), %rsi
    mov $gimsg_len, %edx
    int $0x80
    ret
    .section .init_array, "aw"
    .quad guest_init_marker
    .section .rodata
gimsg: .ascii "libguest.so: init ran\n"
    .set gimsg_len, . - gimsg
    .data
    .globl guest_counter
    .type guest_counter, @object
guest_counter: .long 7
