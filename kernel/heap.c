/* PrismOS — カーネルヒープ: PMM の上に載るブロックアロケータ */
#include "heap.h"

#include "pmm.h"

/* ブロックの先頭に置くヘッダ。ペイロードはヘッダの直後から
 * 16 バイト境界に揃える。 */
typedef struct block {
    size_t size; /* ペイロードの大きさ (ヘッダは含まない) */
    int free;
    struct block *next; /* アドレス順のリスト */
} block_t;

#define BLOCK_ALIGN 16
#define BLOCK_HDR_SIZE ((sizeof(block_t) + BLOCK_ALIGN - 1) & ~(size_t)(BLOCK_ALIGN - 1))

static uintptr_t heap_start; /* アリーナ先頭の仮想アドレス */
static uintptr_t heap_end;
static uint64_t heap_offset; /* 物理 -> 仮想のオフセット */
static block_t *heap_first;
static int heap_ready;

static size_t align_up_size(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

void heap_init(uint64_t phys_to_virt_offset) {
    uint64_t phys = pmm_alloc_pages(HEAP_PAGES);
    if (phys == 0) {
        heap_ready = 0;
        return;
    }
    heap_offset = phys_to_virt_offset;
    heap_start = (uintptr_t)(phys + heap_offset);
    heap_end = heap_start + (uintptr_t)HEAP_PAGES * PMM_PAGE_SIZE;
    heap_first = (block_t *)heap_start;
    heap_first->size = (size_t)HEAP_PAGES * PMM_PAGE_SIZE - BLOCK_HDR_SIZE;
    heap_first->free = 1;
    heap_first->next = NULL;
    heap_ready = 1;
}

int heap_is_ready(void) {
    return heap_ready;
}

size_t heap_total_bytes(void) {
    if (!heap_ready) {
        return 0;
    }
    return (size_t)HEAP_PAGES * PMM_PAGE_SIZE;
}

size_t heap_free_bytes(void) {
    size_t free = 0;
    for (block_t *b = heap_first; b != NULL; b = b->next) {
        if (b->free) {
            free += b->size;
        }
    }
    return free;
}

size_t heap_used_bytes(void) {
    return heap_total_bytes() - heap_free_bytes();
}

void *kmalloc(size_t size) {
    if (!heap_ready || size == 0) {
        return NULL;
    }
    size = align_up_size(size, BLOCK_ALIGN);
    for (block_t *b = heap_first; b != NULL; b = b->next) {
        if (!b->free || b->size < size) {
            continue;
        }
        /* 残りがヘッダ + 最小ペイロード分あるなら分割する */
        size_t rest = b->size - size;
        if (rest >= BLOCK_HDR_SIZE + BLOCK_ALIGN) {
            block_t *nb = (block_t *)((uintptr_t)b + BLOCK_HDR_SIZE + size);
            nb->size = rest - BLOCK_HDR_SIZE;
            nb->free = 1;
            nb->next = b->next;
            b->size = size;
            b->next = nb;
        }
        b->free = 0;
        return (void *)((uintptr_t)b + BLOCK_HDR_SIZE);
    }
    return NULL; /* 枯渇 */
}

void kfree(void *ptr) {
    if (!heap_ready || ptr == NULL) {
        return;
    }
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < heap_start + BLOCK_HDR_SIZE || addr >= heap_end) {
        return; /* ヒープ外のポインタは無視する */
    }
    block_t *b = (block_t *)(addr - BLOCK_HDR_SIZE);
    if (b->free) {
        return; /* 二重解放は無視する */
    }
    b->free = 1;
    /* 隣接する空きブロックと結合する (アドレス順リスト)。
     * 結合したら同じブロックで再検査する: 結合で新しく隣り合った
     * ブロックも結合しないと、空きが断片化したままになる。 */
    for (block_t *cur = heap_first; cur != NULL;) {
        if (!cur->free) {
            cur = cur->next;
            continue;
        }
        block_t *next = cur->next;
        if (next != NULL && next->free &&
            (uintptr_t)cur + BLOCK_HDR_SIZE + cur->size == (uintptr_t)next) {
            cur->size += BLOCK_HDR_SIZE + next->size;
            cur->next = next->next;
            continue; /* 同じ cur で次の隣も調べる */
        }
        cur = cur->next;
    }
}
