/* PrismOS — 32bit カーネル用リンカスクリプト (Multiboot1)
 *
 * 1MB (0x100000) に置く。.multiboot を先頭セクションにして、
 * Multiboot ヘッダがファイル先頭 8192 バイト以内に入るように
 * する。仮想アドレス = 物理アドレス (恒等) で、ページングは
 * 使わない。
 */
ENTRY(_start32)

SECTIONS {
    . = 0x100000;

    .multiboot : {
        *(.multiboot)
    }

    .text : {
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
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
}
