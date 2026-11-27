/* PrismOS — Bochs VBE の実装。 */

#include "vbe.h"

#include "fb.h"
#include "paging.h"
#include "pci.h"
#include "serial.h"
#include "virtio_gpu.h"

#define DISPI_INDEX 0x1CE
#define DISPI_DATA 0x1CF
#define DISPI_ID 0x00
#define DISPI_XRES 0x01
#define DISPI_YRES 0x02
#define DISPI_BPP 0x03
#define DISPI_ENABLE 0x04
#define DISPI_VIRT_WIDTH 0x06
#define DISPI_VIRT_HEIGHT 0x07
#define DISPI_X_OFFSET 0x08
#define DISPI_Y_OFFSET 0x09

#define VBE_ENABLE 0x01
#define VBE_LFB_ENABLE 0x40
#define VBE_NO_CLEAR 0x80

static void io_out16(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port));
}
static uint16_t io_in16(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void dispi_write(uint16_t idx, uint16_t v) {
    io_out16(DISPI_INDEX, idx);
    io_out16(DISPI_DATA, v);
}
static uint16_t dispi_read(uint16_t idx) {
    io_out16(DISPI_INDEX, idx);
    return io_in16(DISPI_DATA);
}

static int vbe_checked;
static uint64_t vbe_hhdm;

void vbe_set_hhdm(uint64_t hhdm_offset) {
    vbe_hhdm = hhdm_offset;
}

int vbe_present(void) {
    uint8_t bus, slot, func;
    if (!pci_find_device(0x1234, 0x1111, &bus, &slot, &func,
                         NULL)) {
        return 0;
    }
    uint16_t id = dispi_read(DISPI_ID);
    vbe_checked = (id >= 0xB0C0 && id <= 0xB0C6);
    return vbe_checked;
}

int vbe_set_mode(uint32_t width, uint32_t height) {
    uint8_t bus, slot, func;
    /* virtio-gpu が表示を担っているときは、そちらの
     * モード変更を使う (VBE の LFB は表示先ではない)。 */
    if (virtio_gpu_active()) {
        return virtio_gpu_set_mode(width, height);
    }
    if (width < 320 || width > 2560 || height < 200 ||
        height > 1600) {
        return -1;
    }
    if (!pci_find_device(0x1234, 0x1111, &bus, &slot, &func,
                         NULL)) {
        return -1;
    }
    uint32_t bar0 = pci_config_read32(bus, slot, func, 0x10);
    uint64_t lfb = bar0 & 0xFFFFFFF0u;
    if ((bar0 & 0x6) == 0x4) {
        lfb |= (uint64_t)pci_config_read32(bus, slot, func,
                                           0x14) << 32;
    }
    if (lfb == 0) {
        return -1;
    }
    uint64_t bytes = (uint64_t)width * height * 4;
    if (bytes > 16u * 1024 * 1024) {
        return -1;
    }
    if (paging_map_mmio(lfb, 16u * 1024 * 1024, vbe_hhdm) !=
        0) {
        return -1;
    }
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (uint16_t)width);
    dispi_write(DISPI_YRES, (uint16_t)height);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)width);
    dispi_write(DISPI_VIRT_HEIGHT, (uint16_t)height);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE,
                VBE_ENABLE | VBE_LFB_ENABLE | VBE_NO_CLEAR);
    /* fb 層を新しい形状で初期化し直す (32bpp XRGB)。 */
    fb_init((void *)(uintptr_t)(lfb + vbe_hhdm), width,
            height, (uint64_t)width * 4, 32, 8, 16, 8, 8, 8, 0);
    serial_write("[vbe] mode set");
    serial_write_line("");
    return 0;
}
