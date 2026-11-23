/* ドライバのホストテスト用スタブ。
 * ahci.c / virtio_blk.c / uhci.c の純粋関数だけをテストする
 * ため、ハードウェアに触る関数 (PCI 設定・PMM・ページング・
 * PIT 待ち) を何もしない版で用意する。ドライバの初期化
 * (ahci_init 等) は呼ばないことが前提。
 * 他のテストには影響しないよう、このファイルは
 * test_drivers だけが使う。 */
#include <stddef.h>
#include <stdint.h>

uint64_t pmm_alloc_page(void) {
    return 0;
}

uint64_t pmm_alloc_pages(size_t count) {
    (void)count;
    return 0;
}

int paging_map_mmio(uint64_t phys, uint64_t size, uint64_t hhdm_offset) {
    (void)phys;
    (void)size;
    (void)hhdm_offset;
    return -1;
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
    (void)bus;
    (void)slot;
    (void)func;
    (void)offset;
    return 0xFFFF;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
    (void)bus;
    (void)slot;
    (void)func;
    (void)offset;
    return 0xFFFFFFFF;
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint16_t value) {
    (void)bus;
    (void)slot;
    (void)func;
    (void)offset;
    (void)value;
}

void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    (void)bus;
    (void)slot;
    (void)func;
}

int pci_find_device(uint16_t vendor, uint16_t device, uint8_t *bus,
                    uint8_t *slot, uint8_t *func, uint32_t *bar0) {
    (void)vendor;
    (void)device;
    (void)bus;
    (void)slot;
    (void)func;
    (void)bar0;
    return 0;
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint8_t *bus, uint8_t *slot, uint8_t *func) {
    (void)class_code;
    (void)subclass;
    (void)prog_if;
    (void)bus;
    (void)slot;
    (void)func;
    return 0;
}

uint32_t pci_bar32_mem(uint8_t bus, uint8_t slot, uint8_t func, int index) {
    (void)bus;
    (void)slot;
    (void)func;
    (void)index;
    return 0;
}

uint32_t pci_bar32_io(uint8_t bus, uint8_t slot, uint8_t func, int index) {
    (void)bus;
    (void)slot;
    (void)func;
    (void)index;
    return 0;
}

void speaker_wait_ticks(uint32_t ticks) {
    (void)ticks;
}

uint16_t speaker_pit0_count(void) {
    /* ホストテストでは呼ばれないはずだが、呼ばれても待ちが
     * 終わるよう、呼ぶたびに進む値を返す。 */
    static uint16_t v = 0;
    v = (uint16_t)(v + 1000);
    return v;
}
