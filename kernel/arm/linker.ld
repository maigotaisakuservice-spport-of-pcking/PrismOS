/* PrismOS — ARM カーネルリンカスクリプト (AArch64、恒等写像)
 *
 * イメージ先頭 (.text.boot のカーネルイメージヘッダ) が
 * 0x40080000 (RAM の先頭 0x40000000 から 512KB 後ろ =
 * ヘッダの text_offset) に来るように置く。ELF から作る
 * raw イメージ (kernel-arm.bin) を起動側がそのまま載せる。
 */
ENTRY(_start)

SECTIONS {
    . = 0x40080000;

    .text : {
        *(.text.boot)
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;

    /* コンパイラが付ける注記類はカーネルには不要。先頭に
     * オーファンとして割り込んで入口のアドレスをずらすので
     * 明示的に捨てる。 */
    /DISCARD/ : {
        *(.note.gnu.property)
        *(.note.gnu.build-id)
        *(.note.*)
        *(.note)
        *(.eh_frame)
    }
}
