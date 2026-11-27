/* PrismOS - PCI configuration access.
 *
 * Reads configuration space through ports 0xCF8/0xCFC.
 * Only the first 256 bytes of a function are used here.
 * No allocator, no clock.
 */
#ifndef PRISM_PCI_H
#define PRISM_PCI_H

#include <stdint.h>

/* Read one 16/32-bit config word. Returns 0xFFFF/0xFFFFFFFF
 * when the device is absent. */
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset);

/* Write one 16-bit config word. */
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint16_t value);

/* Enable memory space and bus mastering for a device
 * (command register bits 1 and 2). Needed before the
 * device can do DMA. */
void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func);

/* Find a device by vendor/device id. Returns 1 and fills
 * bus/slot/func and the BAR0 value (already masked to a
 * 32-bit memory base, 0 when BAR0 is not memory). */
int pci_find_device(uint16_t vendor, uint16_t device, uint8_t *bus,
                    uint8_t *slot, uint8_t *func, uint32_t *bar0);

/* S3 レジューム用: 全 PCI デバイスの設定 (BAR/コマンド/IRQ
 * ライン) を保存し、復帰時に書き戻す。 */
void pci_save_all(void);
void pci_restore_all(void);

/* Find a device by PCI class (class code, subclass, prog-if).
 * prog_if = 0xFF matches any prog-if. Returns 1 and fills
 * bus/slot/func. */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint8_t *bus, uint8_t *slot, uint8_t *func);

/* Read one BAR as a 32-bit memory base (masked). Returns 0
 * when the BAR is an I/O BAR or absent. */
uint32_t pci_bar32_mem(uint8_t bus, uint8_t slot, uint8_t func, int index);

/* Read one BAR as a 32-bit I/O base (masked). Returns 0
 * when the BAR is a memory BAR or absent. */
uint32_t pci_bar32_io(uint8_t bus, uint8_t slot, uint8_t func, int index);

#endif /* PRISM_PCI_H */
