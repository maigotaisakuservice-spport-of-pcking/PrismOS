/* PrismOS - page table mapping (see paging.h). */
#include "paging.h"

#include "pmm.h"
#include "string.h"

#define PAGE_PRESENT (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_LARGE (1ULL << 7)
#define PAGE_NO_CACHE (1ULL << 4)

static uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static void invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static uint64_t *table_virt(uint64_t phys, uint64_t hhdm) {
    return (uint64_t *)(uintptr_t)(phys + hhdm);
}

static int alloc_table(uint64_t *out_phys, uint64_t hhdm) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        return -1;
    }
    memset(table_virt(phys, hhdm), 0, 4096);
    *out_phys = phys;
    return 0;
}

/* Get or create the next-level table for index idx in
 * table (whose entries are physical addresses). */
static int next_table(uint64_t *table, size_t idx, uint64_t hhdm,
                      uint64_t **out) {
    uint64_t entry = table[idx];
    if ((entry & PAGE_PRESENT) == 0) {
        uint64_t phys;
        if (alloc_table(&phys, hhdm) != 0) {
            return -1;
        }
        table[idx] = phys | PAGE_PRESENT | PAGE_WRITABLE;
        entry = table[idx];
    }
    *out = table_virt(entry & 0x000FFFFFFFFFF000ULL, hhdm);
    return 0;
}

int paging_map_mmio(uint64_t phys, uint64_t size, uint64_t hhdm_offset) {
    if (size == 0) {
        return -1;
    }
    uint64_t start = phys & ~0x1FFFFFULL;
    uint64_t end = (phys + size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    uint64_t *pml4 = table_virt(read_cr3(), hhdm_offset);
    for (uint64_t chunk = start; chunk < end; chunk += 0x200000ULL) {
        uint64_t va = chunk + hhdm_offset;
        uint64_t *pdpt;
        uint64_t *pd;
        if (next_table(pml4, (size_t)((va >> 39) & 0x1FF), hhdm_offset,
                       &pdpt) != 0) {
            return -1;
        }
        if (next_table(pdpt, (size_t)((va >> 30) & 0x1FF), hhdm_offset,
                       &pd) != 0) {
            return -1;
        }
        size_t idx = (size_t)((va >> 21) & 0x1FF);
        pd[idx] = chunk | PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE |
                  PAGE_NO_CACHE;
        invlpg(va);
    }
    return 0;
}

/* phys == va の 4KB 恒等マップを現在の CR3 に作る。
 * AP 起動トランポリン (低位メモリ) 用。大きなページが
 * 既にある場所では失敗する (-1)。 */
int paging_map_page_identity(uint64_t phys, uint64_t hhdm_offset) {
    uint64_t va = phys & ~0xFFFULL;
    uint64_t *pml4 = table_virt(read_cr3(), hhdm_offset);
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    if (next_table(pml4, (size_t)((va >> 39) & 0x1FF), hhdm_offset,
                   &pdpt) != 0) {
        return -1;
    }
    if (next_table(pdpt, (size_t)((va >> 30) & 0x1FF), hhdm_offset,
                   &pd) != 0) {
        return -1;
    }
    size_t pd_idx = (size_t)((va >> 21) & 0x1FF);
    if ((pd[pd_idx] & PAGE_PRESENT) &&
        (pd[pd_idx] & PAGE_LARGE)) {
        return -1; /* 大きなページで覆われている */
    }
    if (next_table(pd, pd_idx, hhdm_offset, &pt) != 0) {
        return -1;
    }
    pt[(va >> 12) & 0x1FF] = va | PAGE_PRESENT | PAGE_WRITABLE;
    invlpg(va);
    return 0;
}
