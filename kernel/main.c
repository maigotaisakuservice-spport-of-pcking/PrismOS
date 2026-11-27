/* PrismOS — カーネルメイン (Limine 経由で起動)
 *
 * v0.1 の責務は最小限:
 *   1. シリアル (COM1) を初期化する
 *   2. バナーを出す (QEMU の boot test はこの文字列を探す)
 *   3. Limine からもらった情報 (ブートローダ名、フレームバッファ) を
 *      シリアルに報告する — GUI は後のフェーズでここを使う
 *   4. シリアルシェルに入る
 *
 * v0.2 フェーズ1で追加:
 *   5. IDT・PIC・PS/2 キーボードを初期化し、割り込みを許可する。
 *      キーボード (IRQ1) とシリアルの両方からシェルに入力できる。
 *
 * v0.2 フェーズ2で追加:
 *   6. メモリマップから物理ページアロケータ (PMM) を初期化し、
 *      その上にカーネルヒープ (kmalloc/kfree) を作る。
 *      シェルの `mem` コマンドで状態を確認できる。
 *
 * v0.2 フェーズ3で追加:
 *   7. Limine からもらったフレームバッファで画面を描く。
 *      デスクトップ (壁紙・ウィンドウ・Dock・タスクバー) が使う。
 *
 * Limine 8 では terminal プロトコルが deprecated のため、画面出力は
 * フレームバッファに直接描く。terminal プロトコルは使わない。
 */
#include <stdbool.h>
#include <stdint.h>

#include "limine.h"
#include "block.h"
#include "compat_run.h"
#include "fb.h"
#include "gdt.h"
#include "heap.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "netstack.h"
#include "persist.h"
#include "pic.h"
#include "pmm.h"
#include "recovery.h"
#include "sb16.h"
#include "serial.h"
#include "hda.h"
#include "vinput.h"
#include "vbe.h"
#include "virtio_gpu.h"
#include "smp.h"
#include "acpi.h"
#include "shell.h"
#include "speaker.h"
#include "string.h"
#include "ehci.h"
#include "xhci.h"
#include "uhci.h"
#include "version.h"
#include "vfs.h"
#include "wallpaper.h"

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

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
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
    serial_write_line("Modern centered UI, Dock and quick search,");
    serial_write_line("A shell with two command styles.");
    serial_write_line("Type `help` for commands.");
    serial_write_line("");
}

/* メモリマップから PMM を初期化し、ヒープを作る。
 * 使用可の範囲だけを PMM に渡す (カーネル本体や予約範囲は
 * 含まれない)。HHDM が取れなければヒープは作らない。 */
static void memory_init(void) {
    if (memmap_request.response == NULL) {
        serial_write_line("[boot] memory: (no memmap response)");
        return;
    }
    struct limine_memmap_response *resp = memmap_request.response;
    prism_memmap_entry entries[64];
    size_t n = 0;
    for (uint64_t i = 0; i < resp->entry_count && n < 64; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        entries[n].base = e->base;
        entries[n].length = e->length;
        entries[n].type = e->type;
        n++;
    }
    pmm_init(entries, n);
    serial_write("[boot] memory: ");
    print_u64(pmm_total_pages());
    serial_write(" pages total (");
    print_u64(pmm_total_pages() * 4096 / (1024 * 1024));
    serial_write(" MB), ");
    print_u64(pmm_free_pages_count());
    serial_write_line(" free");

    if (hhdm_request.response != NULL) {
        net_set_hhdm(hhdm_request.response->offset);
        sb16_set_hhdm(hhdm_request.response->offset);
        block_set_hhdm(hhdm_request.response->offset);
        fb_set_hhdm(hhdm_request.response->offset);
        compat_set_hhdm(hhdm_request.response->offset);
        heap_init(hhdm_request.response->offset);
        if (heap_is_ready()) {
            serial_write("[boot] heap: ");
            print_u64(heap_total_bytes());
            serial_write_line(" bytes ready");
            /* VFS のファイル内容はヒープから確保する */
            vfs_set_allocator(kmalloc, kfree);
        } else {
            serial_write_line("[boot] heap: (failed to allocate arena)");
        }
    } else {
        serial_write_line("[boot] heap: (no HHDM response)");
    }
}

/* 起動画面: 公式ロゴ (明るい版) を白い画面の中央に描く。
 * ロゴは kernel/logo.bmp (公式ロゴの明るい版を 640px 幅にした
 * 24 ビット BMP) を埋め込んだもの。1.5 秒ほど見せてから
 * シェル (デスクトップ) へ進む。解析に失敗したときは
 * 何も描かずにそのまま進む。 */
static void boot_show_logo(void) {
    extern const uint8_t prism_logo_data[];
    extern const uint8_t prism_logo_end[];
    wallpaper_img img;
    if (wallpaper_parse(prism_logo_data,
                        (size_t)(prism_logo_end - prism_logo_data),
                        &img) != 0) {
        serial_write_line("[boot] logo: (could not parse)");
        return;
    }
    uint64_t sw = fb_width();
    uint64_t sh = fb_height();
    uint64_t x = sw > (uint64_t)img.width
                     ? (sw - (uint64_t)img.width) / 2
                     : 0;
    uint64_t y = sh > (uint64_t)img.height
                     ? (sh - (uint64_t)img.height) / 2
                     : 0;
    /* アニメーション: 約 2.2 秒、20 fps。
     * 前半 18 フレームでロゴが白からフェードインし、
     * ロゴ下のプリズム色ドット列はハイライトが往復し、
     * 一番下のバーが進捗に合わせて伸びる。整数演算のみ。 */
    static const uint32_t dot_rgb[8] = {
        0xE5484D, 0xF76B15, 0xFFB224, 0x46A758,
        0x0090FF, 0x3E63DD, 0x8E4EC6, 0xE93D82
    };
    uint64_t dots_y = y + (uint64_t)img.height + 36;
    uint64_t bar_y = dots_y + 30;
    uint64_t bar_w = 320;
    uint64_t bar_x = sw > bar_w ? (sw - bar_w) / 2 : 0;
    const int frames = 44;
    fb_clear(fb_rgb(0xFF, 0xFF, 0xFF));
    for (int frame = 0; frame < frames; frame++) {
        int fade = frame < 18 ? (frame * 256) / 18 : 256;
        if (fade < 256 || frame == 18) {
            fb_draw_image_rgb24_fade(
                x, y, (uint64_t)img.width, (uint64_t)img.height,
                img.pixels, img.width, img.height, img.stride,
                img.top_down, fade);
        }
        /* ドット列: ハイライト位置は三角波で往復する。 */
        fb_fill_rect(bar_x, dots_y - 12, bar_w, 24,
                     fb_rgb(0xFF, 0xFF, 0xFF));
        int pos = frame % 14;
        int hi = pos < 8 ? pos : 14 - pos;
        for (int d = 0; d < 8; d++) {
            uint64_t cx = bar_x + 24 + (uint64_t)d * 39;
            uint32_t c = dot_rgb[d];
            int r = (d == hi) ? 9 : 5;
            if (d != hi) {
                /* 非ハイライトは白へ寄せて薄くする。 */
                uint32_t cr = 255 - (255 - ((c >> 16) & 0xFF)) * 3 / 8;
                uint32_t cg = 255 - (255 - ((c >> 8) & 0xFF)) * 3 / 8;
                uint32_t cb = 255 - (255 - (c & 0xFF)) * 3 / 8;
                c = (cr << 16) | (cg << 8) | cb;
            }
            /* 円を行ごとの矩形で塗る。 */
            for (int dy = -r; dy <= r; dy++) {
                int span = 0;
                while ((span + 1) * (span + 1) + dy * dy <= r * r) {
                    span++;
                }
                fb_fill_rect(cx - (uint64_t)span,
                             (uint64_t)((int64_t)dots_y + dy),
                             (uint64_t)(2 * span + 1), 1, c);
            }
        }
        /* 進捗バー: 枠と中身。 */
        fb_fill_rect(bar_x, bar_y, bar_w, 6, fb_rgb(0xE4, 0xE4, 0xE4));
        fb_fill_rect(bar_x, bar_y,
                     bar_w * (uint64_t)(frame + 1) / (uint64_t)frames,
                     6, fb_rgb(0x3E, 0x63, 0xDD));
        serial_write("[boot] anim frame ");
        print_u64((uint64_t)frame);
        serial_write_line("");
        fb_flush();
        /* 1 フレーム = 約 50ms (PIT 1193182 Hz)。 */
        speaker_wait_ticks(1193182u / 20);
    }
    serial_write_line("[boot] logo: light version (animated)");
}

void kmain(void) {
    serial_init();
    /* 例外ハンドラはバナーの前に入れておく。以後の例外は IDT が
     * 受け止めて表示して止まる (v0.1 はここでトリプルフォルト)。 */
    idt_init();
    /* GDT を自前に載せ替える (ユーザーモード用セグメントと
     * TSS が要るため。互換ゲストのサンドボックス用)。 */
    gdt_init();
    print_banner();
    memory_init();
    persist_init();

    pic_init();
    keyboard_init();
    serial_write_line("[boot] keyboard: PS/2 ready (IRQ1, scancode set 1).");
    mouse_init();
    /* USB ホスト (UHCI) があれば初期化し、機器を列挙する。
     * 無ければ何もしない (従来の構成では出ない)。 */
    if (uhci_init(hhdm_request.response != NULL
                      ? hhdm_request.response->offset
                      : 0)) {
        serial_write("[boot] usb: UHCI ready, ");
        print_u64(uhci_device_count());
        serial_write_line(" device(s)");
        if (uhci_storage_present()) {
            block_register_usb(uhci_storage_sectors());
            serial_write_line(
                "[boot] usb: storage registered as block device 1");
        }
    }
    /* USB 2.0 ホスト (EHCI) があれば初期化し、機器を列挙
     * する。無ければ何もしない。 */
    if (ehci_init(hhdm_request.response != NULL
                      ? hhdm_request.response->offset
                      : 0)) {
        serial_write("[boot] usb2: EHCI ready, ");
        print_u64(ehci_device_count());
        serial_write_line(" device(s)");
        if (ehci_storage_present()) {
            block_register_ehci(ehci_storage_sectors());
            serial_write_line(
                "[boot] usb2: storage registered as block device 4");
        }
    }
    /* USB 3.x ホスト (xHCI) があれば初期化し、機器を列挙
     * する。無ければ何もしない。 */
    if (xhci_init(hhdm_request.response != NULL
                      ? hhdm_request.response->offset
                      : 0)) {
        serial_write("[boot] usb3: xHCI ready, ");
        print_u64(xhci_device_count());
        serial_write_line(" device(s)");
        if (xhci_storage_present()) {
            block_register_xhci(xhci_storage_sectors());
            serial_write_line(
                "[boot] usb3: storage registered as block device 5");
        }
    }
    vbe_set_hhdm(hhdm_request.response != NULL
                     ? hhdm_request.response->offset
                     : 0);

    /* virtio タブレット (あれば)。絶対座標ポインタ。 */
    if (hhdm_request.response != NULL) {
        vinput_init(hhdm_request.response->offset);
    }

    /* HD Audio (あれば)。SB16 とは独立の追加ドライバ。 */
    if (hda_init(hhdm_request.response != NULL
                     ? hhdm_request.response->offset
                     : 0)) {
        serial_write_line("[boot] audio: HD Audio codec ready");
    }

    /* ここから割り込みを許可する。キーボードの IRQ1 とマウスの
     * IRQ12 が来る。 */
    prism_sti();

    /* SMP: ACPI MADT があれば他の CPU も起こす。 */
    smp_init(rsdp_request.response != NULL
                 ? (uint64_t)(uintptr_t)rsdp_request.response->address
                 : 0,
             hhdm_request.response != NULL
                 ? hhdm_request.response->offset
                 : 0);

    /* 電源管理: ACPI FADT から S3 スリープ / S5 電源断の
     * 値を読む。 */
    acpi_init(rsdp_request.response != NULL
                  ? (uint64_t)(uintptr_t)rsdp_request.response->address
                  : 0,
              hhdm_request.response != NULL
                  ? hhdm_request.response->offset
                  : 0);

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

    /* virtio-gpu があれば、それを主表示にする (無ければ
     * 従来どおりブートローダのフレームバッファを使う)。 */
    int gpu_display = 0;
    if (hhdm_request.response != NULL) {
        gpu_display = virtio_gpu_init(hhdm_request.response->offset);
    }

    if (gpu_display) {
        serial_write("[boot] framebuffer: ");
        print_u64(virtio_gpu_width());
        serial_write("x");
        print_u64(virtio_gpu_height());
        serial_write_line(" via virtio-gpu (desktop draws here)");
        fb_init(virtio_gpu_framebuffer(), virtio_gpu_width(),
                virtio_gpu_height(), virtio_gpu_pitch(), 32,
                8, 16, 8, 8, 8, 0);
        fb_set_present_hook(virtio_gpu_present);
        if (fb_ready()) {
            serial_write_line("[boot] framebuffer: driver ready");
            boot_show_logo();
        } else {
            serial_write_line("[boot] framebuffer: (unsupported mode)");
        }
    } else if (framebuffer_request.response != NULL &&
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
        serial_write_line(" (desktop draws here)");
        fb_init(fb->address, fb->width, fb->height, fb->pitch, fb->bpp,
                fb->red_mask_size, fb->red_mask_shift,
                fb->green_mask_size, fb->green_mask_shift,
                fb->blue_mask_size, fb->blue_mask_shift);
        /* 2 つ目のフレームバッファがあればミラー先にする。 */
        if (framebuffer_request.response->framebuffer_count > 1) {
            struct limine_framebuffer *fb2 =
                framebuffer_request.response->framebuffers[1];
            fb_set_mirror(fb2->address, fb2->pitch, fb2->height,
                          fb2->bpp);
            if (fb_mirror_active()) {
                serial_write("[boot] framebuffer: second display "
                             "mirrored (");
                print_u64(fb2->width);
                serial_write("x");
                print_u64(fb2->height);
                serial_write_line(")");
            }
        }
        if (fb_ready()) {
            serial_write_line("[boot] framebuffer: driver ready");
            boot_show_logo();
        } else {
            serial_write_line("[boot] framebuffer: (unsupported mode)");
        }
    } else {
        serial_write_line("[boot] framebuffer: (none)");
    }

    /* 起動時の回復モード: ロゴ画面に案内を出し、R キーが
     * 押されれば回復モードの印を付ける (メニューはログイン
     * の後に shell_run() が出す)。フレームバッファの準備が
     * 済んでから行うので、案内が画面にも出る。 */
    recovery_key_check();

    serial_write_line("[boot] entering serial shell on COM1.");
    shell_run();

    /* shell_run() は戻らない */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
