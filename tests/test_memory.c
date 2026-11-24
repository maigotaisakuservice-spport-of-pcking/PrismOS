/* PrismOS — メモリ管理のホストテスト
 *
 * カーネルと全く同じ kernel/pmm.c / kernel/heap.c をホストの
 * gcc でコンパイルして実行する。QEMU は不要。`make test` で走る。
 * 擬似物理メモリはホストの静的バッファで、ヒープのオフセットは
 * 0 (擬似物理アドレスがそのままホストのポインタになる)。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "heap.h"
#include "pmm.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

/* 擬似物理メモリ 4MB (ページ境界に揃える) */
static uint8_t fake_ram[4 * 1024 * 1024] __attribute__((aligned(4096)));

static void setup_pmm(void) {
    prism_memmap_entry entries[3];
    entries[0].base = (uint64_t)(uintptr_t)fake_ram;
    entries[0].length = sizeof(fake_ram);
    entries[0].type = PMM_TYPE_USABLE;
    /* 予約範囲は無視されること */
    entries[1].base = 0x100000;
    entries[1].length = 0x100000;
    entries[1].type = 1;
    /* 端数があって境界に揃わない使用可範囲: 切り詰められること。
     * 1 ページ分にも満たないので、総ページ数には寄与しない。 */
    entries[2].base = 0x90000000ULL;
    entries[2].length = 100;
    entries[2].type = PMM_TYPE_USABLE;
    pmm_init(entries, 3);
}

static int in_fake_ram(uint64_t addr) {
    uint64_t base = (uint64_t)(uintptr_t)fake_ram;
    return addr >= base && addr < base + sizeof(fake_ram);
}

int main(void) {
    /* ---- PMM ---- */
    setup_pmm();
    CHECK(pmm_total_pages() == 1024);
    CHECK(pmm_free_pages_count() == 1024);
    CHECK(pmm_used_pages_count() == 0);

    uint64_t p1 = pmm_alloc_page();
    CHECK(p1 != 0);
    CHECK(in_fake_ram(p1));
    CHECK((p1 & 0xFFF) == 0);
    CHECK(pmm_free_pages_count() == 1023);
    CHECK(pmm_used_pages_count() == 1);

    uint64_t p2 = pmm_alloc_page();
    CHECK(p2 != 0 && p2 != p1);
    CHECK(pmm_free_pages_count() == 1022);

    /* 連続 4 ページ: 先頭が返り、範囲内であること */
    uint64_t p4 = pmm_alloc_pages(4);
    CHECK(p4 != 0);
    CHECK((p4 & 0xFFF) == 0);
    CHECK(in_fake_ram(p4));
    CHECK(pmm_free_pages_count() == 1018);

    /* 解放して再利用されること */
    pmm_free_page(p1);
    CHECK(pmm_free_pages_count() == 1019);
    uint64_t p1b = pmm_alloc_page();
    CHECK(p1b != 0);
    CHECK(pmm_free_pages_count() == 1018);

    /* 二重解放は無視されること (空き数が増えない) */
    pmm_free_page(p1b);
    uint64_t free_before = pmm_free_pages_count();
    pmm_free_page(p1b);
    CHECK(pmm_free_pages_count() == free_before);

    /* 境界でない・範囲外の解放は無視されること */
    pmm_free_page(p2 + 1);
    CHECK(pmm_free_pages_count() == free_before);
    pmm_free_page(0x1000);
    CHECK(pmm_free_pages_count() == free_before);

    /* 連続解放: p4 の 4 ページを返し、結合されること */
    pmm_free_pages(p4, 4);
    CHECK(pmm_free_pages_count() == free_before + 4);
    uint64_t p4b = pmm_alloc_pages(4);
    CHECK(p4b != 0);
    CHECK(pmm_free_pages_count() == free_before);

    /* 枯渇: 残りを全部確保したら 0 が返ること */
    {
        uint64_t got = 0;
        while (pmm_alloc_page() != 0) {
            got++;
        }
        CHECK(pmm_free_pages_count() == 0);
        CHECK(pmm_alloc_page() == 0);
        CHECK(pmm_alloc_pages(2) == 0);
        /* 1 ページ返せばまた確保できること */
        pmm_free_page(p2);
        CHECK(pmm_alloc_page() != 0);
        (void)got;
    }

    /* ---- ヒープ ---- */
    setup_pmm();
    heap_init(0);
    CHECK(heap_is_ready() == 1);
    /* 擬似 RAM は 4MB (1024 ページ) しかないため、heap_init は
     * 2048 ページから 1024 ページへ落とす。実機では 8MB。 */
    CHECK(heap_total_bytes() == (size_t)1024 * 4096);
    size_t heap_free0 = heap_free_bytes();
    CHECK(heap_free0 > 0);
    CHECK(heap_used_bytes() == heap_total_bytes() - heap_free0);

    CHECK(kmalloc(0) == NULL);
    void *a = kmalloc(100);
    CHECK(a != NULL);
    memset(a, 0xAB, 100);
    CHECK(((uint8_t *)a)[0] == 0xAB && ((uint8_t *)a)[99] == 0xAB);
    void *b = kmalloc(200);
    CHECK(b != NULL && b != a);
    void *c = kmalloc(4096);
    CHECK(c != NULL);
    CHECK(heap_used_bytes() > 0);

    /* 大きすぎる確保は NULL */
    CHECK(kmalloc(heap_total_bytes()) == NULL);

    /* 解放して再利用: 全部解放すると空きが元に戻る
     * (結合が働いていないと、分割ヘッダ分だけ戻らない) */
    kfree(a);
    kfree(b);
    kfree(c);
    CHECK(heap_free_bytes() == heap_free0);

    /* 再利用: 同じ大きさを確保できること */
    void *a2 = kmalloc(100);
    CHECK(a2 != NULL);
    kfree(a2);
    CHECK(heap_free_bytes() == heap_free0);

    /* 二重解放・NULL 解放・ヒープ外ポインタは無視されること */
    void *d = kmalloc(64);
    CHECK(d != NULL);
    size_t free_before_d = heap_free_bytes();
    kfree(d);
    size_t free_after1 = heap_free_bytes();
    CHECK(free_after1 >= free_before_d + 64);
    kfree(d); /* 二重解放 */
    CHECK(heap_free_bytes() == free_after1);
    kfree(NULL);
    CHECK(heap_free_bytes() == free_after1);
    int outside = 42;
    kfree(&outside);
    CHECK(heap_free_bytes() == free_after1);

    /* 枯渇と回復: 全部使い切ってから解放すると元に戻る */
    {
        void *slots[64];
        int n = 0;
        while (n < 64) {
            slots[n] = kmalloc(1024);
            if (slots[n] == NULL) {
                break;
            }
            n++;
        }
        CHECK(n > 0);
        /* ヒープは 4MB (上のフォールバック)。3MB の確保は
         * 成功し、解放すれば空きが元に戻る。ヒープ全体より
         * 大きな確保は失敗すること。 */
        void *big3 = kmalloc(3 * 1024 * 1024);
        CHECK(big3 != NULL);
        kfree(big3);
        CHECK(kmalloc(heap_total_bytes()) == NULL);
        for (int i = 0; i < n; i++) {
            kfree(slots[i]);
        }
        CHECK(heap_free_bytes() == heap_free0);
        void *e = kmalloc(1024);
        CHECK(e != NULL);
        kfree(e);
    }

    printf("memory tests: %d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
