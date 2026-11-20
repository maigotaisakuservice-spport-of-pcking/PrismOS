/* PrismOS — カーネルリンカスクリプト (Limine, x86-64, higher half) */
ENTRY(_start)

SECTIONS {
    . = 0xffffffff80000000;

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

    /* Limine リクエストは limine.h のマクロが
     * .limine_requests セクションに置く。 */
    .limine_requests : {
        *(.limine_requests)
        *(.limine_requests.*)
    }
}
