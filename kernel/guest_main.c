/* PrismOS — ゲストカーネル (デュアルブートのテスト用「もう一つの OS」)。
 *
 * これは製品の OS ではない。デュアルブートのメニュー機構を
 * 検証するための、独立した小さなカーネルである。Limine から
 * 起動し、シリアルにバナーを出して止まる。PrismOS のカーネル
 * とは別物 (別のエントリ、別のバナー、デスクトップも
 * ファイルシステムも無い) で、ブートメニューで選んだ側が
 * 本当に起動することを確かめるための代役である。
 * ビルド: make guest (build/guest.elf)。
 */
#include "serial.h"

void kmain(void) {
    serial_init();
    serial_write_line("GUEST SYSTEM 1.0");
    serial_write_line("Other system stand-in: booted from the dual-boot menu.");
    serial_write_line("guest> (no shell here)");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
