/* PrismOS - PCI configuration access (see pci.h). */
#include "pci.h"

#include <stddef.h>

#include "io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset) {
    return 0x80000000u | ((uint32_t)bus << 16) |
           ((uint32_t)(slot & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8) | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
    /* 生の inline asm ("Nd" 制約つき outl/inl) は新しい
     * アセンブラ (CI) が operand type mismatch で弾くため、
     * io.h のラッパー (ポートが uint16_t で宣言され、
     * オペランドサイズが一致する) を使う。 */
    prism_outl(PCI_ADDR, pci_address(bus, slot, func, offset));
    return prism_inl(PCI_DATA);
}

static void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t offset, uint32_t value) {
    prism_outl(PCI_ADDR, pci_address(bus, slot, func, offset));
    prism_outl(PCI_DATA, value);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint16_t value) {
    uint32_t v = pci_config_read32(bus, slot, func, offset & 0xFC);
    uint32_t shift = (uint32_t)(offset & 0x02) * 8u;
    v &= ~(0xFFFFu << shift);
    v |= (uint32_t)value << shift;
    pci_config_write32(bus, slot, func, offset & 0xFC, v);
}

void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t cmd = pci_config_read16(bus, slot, func, 0x04);
    cmd |= 0x06; /* memory space (bit 1) + bus master (bit 2) */
    pci_config_write16(bus, slot, func, 0x04, cmd);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
    uint32_t v = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

int pci_find_device(uint16_t vendor, uint16_t device, uint8_t *bus,
                    uint8_t *slot, uint8_t *func, uint32_t *bar0) {
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t s = 0; s < 32; s++) {
            for (uint8_t f = 0; f < 8; f++) {
                if (pci_config_read16((uint8_t)b, s, f, 0) != vendor) {
                    continue;
                }
                if (pci_config_read16((uint8_t)b, s, f, 2) != device) {
                    continue;
                }
                if (bus != NULL) {
                    *bus = (uint8_t)b;
                }
                if (slot != NULL) {
                    *slot = s;
                }
                if (func != NULL) {
                    *func = f;
                }
                if (bar0 != NULL) {
                    uint32_t bar = pci_config_read32((uint8_t)b, s, f, 0x10);
                    *bar0 = (bar & 1) ? 0 : (bar & 0xFFFFFFF0u);
                }
                return 1;
            }
        }
    }
    return 0;
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint8_t *bus, uint8_t *slot, uint8_t *func) {
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t s = 0; s < 32; s++) {
            for (uint8_t f = 0; f < 8; f++) {
                uint32_t cls = pci_config_read32((uint8_t)b, s, f, 0x08);
                if ((cls & 0xFFFF0000u) == 0xFFFF0000u) {
                    continue; /* 空きスロット */
                }
                uint8_t cc = (uint8_t)(cls >> 24);
                uint8_t sc = (uint8_t)(cls >> 16);
                uint8_t pi = (uint8_t)(cls >> 8);
                if (cc != class_code || sc != subclass) {
                    continue;
                }
                if (prog_if != 0xFF && pi != prog_if) {
                    continue;
                }
                if (bus != NULL) {
                    *bus = (uint8_t)b;
                }
                if (slot != NULL) {
                    *slot = s;
                }
                if (func != NULL) {
                    *func = f;
                }
                return 1;
            }
        }
    }
    return 0;
}

uint32_t pci_bar32_mem(uint8_t bus, uint8_t slot, uint8_t func, int index) {
    uint32_t bar =
        pci_config_read32(bus, slot, func, (uint8_t)(0x10 + index * 4));
    if (bar & 1) {
        return 0;
    }
    return bar & 0xFFFFFFF0u;
}

uint32_t pci_bar32_io(uint8_t bus, uint8_t slot, uint8_t func, int index) {
    uint32_t bar =
        pci_config_read32(bus, slot, func, (uint8_t)(0x10 + index * 4));
    if ((bar & 1) == 0) {
        return 0;
    }
    return bar & 0xFFFFFFFCu;
}

/* --- S3 レジューム用 PCI 設定の保存/復元 -----------------------
 * S3 復帰ではプラットフォームが PCI 設定空間 (BAR とコマンド
 * レジスタ) を初期状態に戻す (実測: VGA の BAR0 が 0、
 * コマンドが 0 になり、フレームバッファも NIC も到達不能に
 * なる)。スリープ前に全デバイス分を保存し、復帰時に書き戻す
 * のが OS の責任。 */
#define PCI_SNAP_MAX 32

static struct {
    uint8_t bus, slot, func;
    uint32_t bar[6];
    uint16_t cmd;
    uint8_t irq_line;
} pci_snap[PCI_SNAP_MAX];
static int pci_snap_count;

void pci_save_all(void) {
    pci_snap_count = 0;
    for (int bus = 0; bus < 256 && pci_snap_count < PCI_SNAP_MAX;
         bus++) {
        for (int slot = 0; slot < 32 && pci_snap_count < PCI_SNAP_MAX;
             slot++) {
            for (int func = 0; func < 8 &&
                 pci_snap_count < PCI_SNAP_MAX; func++) {
                uint32_t id = pci_config_read32(
                    (uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }
                int i = pci_snap_count++;
                pci_snap[i].bus = (uint8_t)bus;
                pci_snap[i].slot = (uint8_t)slot;
                pci_snap[i].func = (uint8_t)func;
                for (int b = 0; b < 6; b++) {
                    pci_snap[i].bar[b] = pci_config_read32(
                        (uint8_t)bus, (uint8_t)slot, (uint8_t)func,
                        (uint8_t)(0x10 + b * 4));
                }
                pci_snap[i].cmd = pci_config_read16(
                    (uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x04);
                pci_snap[i].irq_line = (uint8_t)pci_config_read32(
                    (uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x3C);
                if (func == 0) {
                    uint32_t hdr = pci_config_read32(
                        (uint8_t)bus, (uint8_t)slot, (uint8_t)func,
                        0x0C);
                    if (((hdr >> 16) & 0x80) == 0) {
                        break; /* 単機能デバイス */
                    }
                }
            }
        }
    }
}

void pci_restore_all(void) {
    for (int i = 0; i < pci_snap_count; i++) {
        uint8_t bus = pci_snap[i].bus;
        uint8_t slot = pci_snap[i].slot;
        uint8_t func = pci_snap[i].func;
        /* コマンドを落とした状態で BAR を書き戻し、最後に
         * コマンドで有効化する。 */
        pci_config_write16(bus, slot, func, 0x04, 0);
        for (int b = 0; b < 6; b++) {
            pci_config_write32(bus, slot, func,
                               (uint8_t)(0x10 + b * 4),
                               pci_snap[i].bar[b]);
        }
        uint32_t il = pci_config_read32(bus, slot, func, 0x3C);
        il = (il & 0xFFFFFF00u) | pci_snap[i].irq_line;
        pci_config_write32(bus, slot, func, 0x3C, il);
        pci_config_write16(bus, slot, func, 0x04, pci_snap[i].cmd);
    }
}
