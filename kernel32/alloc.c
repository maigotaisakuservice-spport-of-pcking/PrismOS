/* PrismOS — 32bit 用ヒープ (静的アリーナ上のフリーリスト)
 *
 * VFS が使う割り当て器。2MB の静的アリーナをブロックに切り、
 * 最初に合う空きブロックを使う (first-fit)。解放時は隣接する
 * 空きブロックと結合する。x86-64 版の heap.c と同じ考え方で、
 * PMM に頼らない 32bit 版。
 */
#include <stddef.h>
#include <stdint.h>

#define ALLOC_ARENA_SIZE (12u * 1024u * 1024u)
#define ALLOC_ALIGN 16u

typedef struct block {
    size_t size;        /* ヘッダを含まない本体の大きさ */
    int free;
    struct block *next;
    uint32_t pad;       /* ヘッダを 16 バイトに揃える:
                           返却ポインタが常に 16 バイト境界に
                           乗り、DMA リングの先頭に使える */
} block_t;

static uint8_t arena[ALLOC_ARENA_SIZE]
    __attribute__((aligned(ALLOC_ALIGN)));
static block_t *head;

static size_t round_up(size_t n) {
    return (n + ALLOC_ALIGN - 1) & ~(size_t)(ALLOC_ALIGN - 1);
}

void alloc32_init(void) {
    head = (block_t *)arena;
    head->size = ALLOC_ARENA_SIZE - sizeof(block_t);
    head->free = 1;
    head->next = NULL;
}

void *alloc32(size_t size) {
    block_t *b;
    if (size == 0) {
        size = 1;
    }
    size = round_up(size);
    for (b = head; b != NULL; b = b->next) {
        if (!b->free || b->size < size) {
            continue;
        }
        /* 余りが次のブロックとして成り立つなら分割する。 */
        if (b->size >= size + sizeof(block_t) + ALLOC_ALIGN) {
            block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
            nb->size = b->size - size - sizeof(block_t);
            nb->free = 1;
            nb->next = b->next;
            b->next = nb;
            b->size = size;
        }
        b->free = 0;
        return (uint8_t *)b + sizeof(block_t);
    }
    return NULL;
}

void free32(void *ptr) {
    block_t *b;
    if (ptr == NULL) {
        return;
    }
    b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    b->free = 1;
    /* 隣接する空きブロックを結合する (全体を走査する単純な方法)。 */
    for (b = head; b != NULL && b->next != NULL;) {
        block_t *n = b->next;
        if (b->free && n->free &&
            (uint8_t *)b + sizeof(block_t) + b->size == (uint8_t *)n) {
            b->size += sizeof(block_t) + n->size;
            b->next = n->next;
        } else {
            b = b->next;
        }
    }
}

size_t alloc32_free_bytes(void) {
    size_t total = 0;
    block_t *b;
    for (b = head; b != NULL; b = b->next) {
        if (b->free) {
            total += b->size;
        }
    }
    return total;
}
