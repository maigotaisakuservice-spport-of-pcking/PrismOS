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

    /* Limine リクエストは limine.h のマクロが
     * .limine_requests セクションに置く。.bss の前へ置く
     * こと。後ろに置くと、NOBITS の .bss の分までファイルに
     * 0 埋めが入り、kernel.elf が 30MB 級に膨らんでブート
     * パーティション (32MB) に新旧 2 個が入らなくなる。 */
    .limine_requests : {
        *(.limine_requests)
        *(.limine_requests.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
}
