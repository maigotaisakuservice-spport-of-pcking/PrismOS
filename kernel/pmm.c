/* PrismOS — 物理ページアロケータ (PMM): 空き範囲リスト版 */
#include "pmm.h"

#define PMM_MAX_RANGES 64
#define PMM_LOW_RESERVE 0x100000ULL /* 1MB より下は使わない */

typedef struct {
    uint64_t base;   /* 範囲の先頭 (ページ境界) */
    uint64_t length; /* バイト数 (ページサイズの倍数) */
} free_range;

static free_range ranges[PMM_MAX_RANGES];
static size_t range_count;
static uint64_t total_pages;

/* pmm_init で登録した使用可範囲 (ページ境界に揃えた後)。
 * 解放時に、返すアドレスがここに含まれるかを検査する。
 * 管理外のアドレスを空きリストに混ぜないためのもの。 */
static free_range managed[PMM_MAX_RANGES];
static size_t managed_count;

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static uint64_t align_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

/* 空きリストに範囲を挿入し、隣り合う範囲と結合する。
 * 既存の空き範囲と重なる場合は、その重なり分を除いて挿入する。 */
static void insert_range(uint64_t base, uint64_t length) {
    if (length == 0) {
        return;
    }
    /* 既存範囲と重なる部分を切り落とす */
    for (size_t i = 0; i < range_count; i++) {
        uint64_t rs = ranges[i].base;
        uint64_t re = rs + ranges[i].length;
        uint64_t ns = base;
        uint64_t ne = base + length;
        if (ne <= rs || ns >= re) {
            continue; /* 重なりなし */
        }
        /* 重なりがある: 既存範囲の外側だけを残す。
         * 簡単のため、重なる部分より前だけを残し、後ろは
         * 次の反復で扱うのではなく、ここでは前部分だけにする。
         * 呼び出し側が二重解放しない限り、ここには来ない。 */
        if (ns < rs) {
            insert_range(ns, rs - ns);
        }
        if (ne > re) {
            insert_range(re, ne - re);
        }
        return;
    }
    /* 挿入位置: base の昇順を保つ */
    size_t pos = 0;
    while (pos < range_count && ranges[pos].base < base) {
        pos++;
    }
    if (range_count >= PMM_MAX_RANGES) {
        return; /* 枠が無い: この範囲は使わない (安全側に倒す) */
    }
    for (size_t i = range_count; i > pos; i--) {
        ranges[i] = ranges[i - 1];
    }
    ranges[pos].base = base;
    ranges[pos].length = length;
    range_count++;
    /* 前後と結合する */
    if (pos > 0 && ranges[pos - 1].base + ranges[pos - 1].length == base) {
        ranges[pos - 1].length += ranges[pos].length;
        for (size_t i = pos; i + 1 < range_count; i++) {
            ranges[i] = ranges[i + 1];
        }
        range_count--;
        pos--;
    }
    if (pos + 1 < range_count &&
        ranges[pos].base + ranges[pos].length == ranges[pos + 1].base) {
        ranges[pos].length += ranges[pos + 1].length;
        for (size_t i = pos + 1; i + 1 < range_count; i++) {
            ranges[i] = ranges[i + 1];
        }
        range_count--;
    }
}

void pmm_init(const prism_memmap_entry *entries, size_t count) {
    range_count = 0;
    managed_count = 0;
    total_pages = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].type != PMM_TYPE_USABLE) {
            continue;
        }
        uint64_t base = entries[i].base;
        uint64_t end = base + entries[i].length;
        if (base < PMM_LOW_RESERVE) {
            base = PMM_LOW_RESERVE;
        }
        base = align_up(base, PMM_PAGE_SIZE);
        end = align_down(end, PMM_PAGE_SIZE);
        if (end <= base) {
            continue;
        }
        total_pages += (end - base) / PMM_PAGE_SIZE;
        if (managed_count < PMM_MAX_RANGES) {
            managed[managed_count].base = base;
            managed[managed_count].length = end - base;
            managed_count++;
        }
        insert_range(base, end - base);
    }
}

uint64_t pmm_alloc_pages(size_t count) {
    if (count == 0) {
        return 0;
    }
    uint64_t need = (uint64_t)count * PMM_PAGE_SIZE;
    for (size_t i = 0; i < range_count; i++) {
        if (ranges[i].length >= need) {
            uint64_t addr = ranges[i].base;
            ranges[i].base += need;
            ranges[i].length -= need;
            if (ranges[i].length == 0) {
                for (size_t j = i; j + 1 < range_count; j++) {
                    ranges[j] = ranges[j + 1];
                }
                range_count--;
            }
            return addr;
        }
    }
    return 0;
}

uint64_t pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

void pmm_free_pages(uint64_t addr, size_t count) {
    if (addr == 0 || count == 0) {
        return;
    }
    if ((addr & (PMM_PAGE_SIZE - 1)) != 0) {
        return; /* ページ境界でないものは受け付けない */
    }
    /* 管理している使用可範囲にすっぽり含まれる場合だけ受け付ける。
     * 管理外のアドレスを返すと、存在しないページを貸し出して
     * しまうため、ここで弾く。 */
    uint64_t len = (uint64_t)count * PMM_PAGE_SIZE;
    int inside = 0;
    for (size_t i = 0; i < managed_count; i++) {
        if (addr >= managed[i].base &&
            addr + len <= managed[i].base + managed[i].length) {
            inside = 1;
            break;
        }
    }
    if (!inside) {
        return;
    }
    insert_range(addr, len);
}

void pmm_free_page(uint64_t addr) {
    pmm_free_pages(addr, 1);
}

uint64_t pmm_total_pages(void) {
    return total_pages;
}

uint64_t pmm_free_pages_count(void) {
    uint64_t free = 0;
    for (size_t i = 0; i < range_count; i++) {
        free += ranges[i].length / PMM_PAGE_SIZE;
    }
    return free;
}

uint64_t pmm_used_pages_count(void) {
    return total_pages - pmm_free_pages_count();
}
