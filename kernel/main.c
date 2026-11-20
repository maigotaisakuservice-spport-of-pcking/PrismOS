/* PrismOS — カーネルメイン (Limine 経由で起動)
 *
 * v0.1 の責務は最小限:
 *   1. シリアル (COM1) を初期化する
 *   2. バナーを出す (QEMU の boot test はこの文字列を探す)
 *   3. Limine からもらった情報 (ブートローダ名、フレームバッファ) を
 *      シリアルに報告する — GUI は後のフェーズでここを使う
 *   4. シリアルシェルに入る
 *
 * Limine 8 では terminal プロトコルが deprecated のため、画面出力は
 * 一切使わない。フレームバッファは「要求して寸法だけ報告する」に留め、
 * 描画はしない。
 */
#include <stdbool.h>
#include <stdint.h>

#include "limine.h"
#include "serial.h"
#include "shell.h"
#include "string.h"

#define PRISM_VERSION "0.1.0"

/* Limine リクエストは .limine_requests セクションに置く必要がある。
 * (limine.h のマクロがそのための属性を付けている) */
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

static void print_u64(uint64_t value) {
    char buf[21];
    int i = 0;
    if (value == 0) {
        serial_write_char('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        serial_write_char(buf[--i]);
    }
}

static void print_banner(void) {
    serial_write_line("");
    serial_write_line("  ____       _               ___  ____  ");
    serial_write_line(" |  _ \\ _ __(_)___ _ __ ___ / _ \\/ ___| ");
    serial_write_line(" | |_) | '__| / __| '_ ` _ \\ | | \\___ \\ ");
    serial_write_line(" |  __/| |  | \\__ \\ | | | | | |_| |___) |");
    serial_write_line(" |_|   |_|  |_|___/_| |_| |_|\\___/|____/ ");
    serial_write_line("");
    serial_write_line("PrismOS " PRISM_VERSION);
    serial_write_line("Windows 11-style UI, Mac-style features,");
    serial_write_line("Linux & Windows compatible shell.");
    serial_write_line("Type `help` for commands.");
    serial_write_line("");
}

void kmain(void) {
    serial_init();
    print_banner();

    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        serial_write_line("[boot] ERROR: Limine base revision not supported.");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (bootloader_info_request.response != NULL) {
        serial_write("[boot] bootloader: ");
        serial_write(bootloader_info_request.response->name);
        serial_write(" ");
        serial_write_line(bootloader_info_request.response->version);
    } else {
        serial_write_line("[boot] bootloader: (unknown)");
    }

    if (framebuffer_request.response != NULL &&
        framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb =
            framebuffer_request.response->framebuffers[0];
        serial_write("[boot] framebuffer: ");
        print_u64(fb->width);
        serial_write("x");
        print_u64(fb->height);
        serial_write(" pitch=");
        print_u64(fb->pitch);
        serial_write(" bpp=");
        print_u64(fb->bpp);
        serial_write_line(" (reserved for the future GUI)");
    } else {
        serial_write_line("[boot] framebuffer: (none)");
    }

    serial_write_line("[boot] entering serial shell on COM1.");
    shell_run();

    /* shell_run() は戻らない */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
