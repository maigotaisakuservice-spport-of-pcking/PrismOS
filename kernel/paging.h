/* PrismOS - page table mapping for device memory.
 *
 * The bootloader's higher-half map covers RAM, but device
 * memory (PCI BARs) is not in the memory map, so it is not
 * mapped there. This module walks the current page tables
 * (read from CR3) and maps a physical range with 2MB large
 * pages, uncacheable, at its higher-half address
 * (physical + HHDM offset).
 */
#ifndef PRISM_PAGING_H
#define PRISM_PAGING_H

#include <stdint.h>

/* Map [phys, phys+size) at phys + hhdm_offset, using 2MB
 * pages. Returns 0 on success. The tables are allocated
 * from the physical allocator, so call it after the PMM
 * is ready. */
int paging_map_mmio(uint64_t phys, uint64_t size, uint64_t hhdm_offset);
int paging_map_page_identity(uint64_t phys, uint64_t hhdm_offset);

#endif /* PRISM_PAGING_H */
