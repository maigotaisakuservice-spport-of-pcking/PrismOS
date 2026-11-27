/* PrismOS — 互換ゲストのサンドボックス (sandbox.h を見ること)。 */
#include "sandbox.h"

#include "compat_run.h"
#include "idt.h"
#include "io.h"
#include "pic.h"
#include "pmm.h"
#include "string.h"

#define PG_PRESENT (1ULL << 0)
#define PG_WRITABLE (1ULL << 1)
#define PG_USER (1ULL << 2)
#define PG_NX (1ULL << 63)
#define PG_ADDR_MASK 0x000FFFFFFFFFF000ULL

static uint64_t g_hhdm;

volatile int compat_guest_active;

void sandbox_init(uint64_t hhdm_offset) {
    g_hhdm = hhdm_offset;
    /* NX ビットを有効にする (EFER.NXE)。カーネル側の既存の
     * ページは NX を使っていないので、有効化しても影響は
     * 無い。ゲストのデータページに NX を付けるために要る。 */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    if ((lo & (1u << 11)) == 0) {
        lo |= (1u << 11);
        __asm__ volatile("wrmsr" : : "c"(0xC0000080u), "a"(lo),
                         "d"(hi));
    }
}

static uint64_t *tbl_virt(uint64_t phys) {
    return (uint64_t *)(uintptr_t)(phys + g_hhdm);
}

static uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v & PG_ADDR_MASK;
}

uint64_t sandbox_create(void) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        return 0;
    }
    uint64_t *pml4 = tbl_virt(phys);
    memset(pml4, 0, 4096);
    /* 上半分 (256..511) はカーネルの PML4 をそのまま写す。
     * 指す先のテーブルは共有で、管理者専用のまま。 */
    const uint64_t *kern = tbl_virt(read_cr3());
    for (int i = 256; i < 512; i++) {
        pml4[i] = kern[i];
    }
    return phys;
}

/* 次レベルのテーブルを取る (無ければ作る)。ユーザー側の
 * 中間エントリには U/S と書き込み可を立てる (最終段の
 * 権限が効くよう、途中は緩くしておくのが x86 の流儀)。 */
static uint64_t *next_table(uint64_t *table, size_t idx, int create) {
    uint64_t e = table[idx];
    if ((e & PG_PRESENT) != 0 && (e & (1ULL << 7)) != 0) {
        /* PS ビットが立った中間エントリはラージページであって
         * テーブルではない。ゲストのテーブルには決して
         * 現れないはずで、見つけたら壊れたものとして扱う。 */
        return NULL;
    }
    if ((e & PG_PRESENT) == 0) {
        if (!create) {
            return NULL;
        }
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            return NULL;
        }
        memset(tbl_virt(phys), 0, 4096);
        table[idx] = phys | PG_PRESENT | PG_WRITABLE | PG_USER;
        e = table[idx];
    }
    return tbl_virt(e & PG_ADDR_MASK);
}

static uint64_t *leaf_entry(uint64_t pml4_phys, uint64_t va,
                            int create) {
    if ((va >> 47) != 0) {
        /* ユーザー空間 (PML4 の 0..255) の外はゲストの
         * テーブルで扱わない。上半分は共有カーネル側で、
         * ここで弾かないと細工したアドレスでカーネルの
         * テーブルをたどる経路になる。 */
        return NULL;
    }
    uint64_t *pml4 = tbl_virt(pml4_phys);
    uint64_t *pdpt = next_table(pml4, (size_t)((va >> 39) & 0x1FF),
                                create);
    if (pdpt == NULL) {
        return NULL;
    }
    uint64_t *pd = next_table(pdpt, (size_t)((va >> 30) & 0x1FF),
                              create);
    if (pd == NULL) {
        return NULL;
    }
    uint64_t *pt = next_table(pd, (size_t)((va >> 21) & 0x1FF),
                              create);
    if (pt == NULL) {
        return NULL;
    }
    return &pt[(va >> 12) & 0x1FF];
}

int sandbox_map(uint64_t pml4_phys, uint64_t va, uint64_t phys,
                int writable, int executable) {
    uint64_t *leaf = leaf_entry(pml4_phys, va, 1);
    if (leaf == NULL) {
        return -1;
    }
    uint64_t e = (phys & PG_ADDR_MASK) | PG_PRESENT | PG_USER;
    if (writable) {
        e |= PG_WRITABLE;
    }
    if (!executable) {
        e |= PG_NX;
    }
    *leaf = e;
    return 0;
}

int sandbox_map_new(uint64_t pml4_phys, uint64_t va, int writable,
                    int executable, uint64_t *out_phys) {
    uint64_t existing = sandbox_lookup(pml4_phys, va);
    if (existing != 0) {
        if (out_phys != NULL) {
            *out_phys = existing;
        }
        return 0;
    }
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        return -1;
    }
    memset(tbl_virt(phys), 0, 4096);
    if (sandbox_map(pml4_phys, va, phys, writable, executable) != 0) {
        pmm_free_page(phys);
        return -1;
    }
    if (out_phys != NULL) {
        *out_phys = phys;
    }
    return 0;
}

uint64_t sandbox_lookup(uint64_t pml4_phys, uint64_t va) {
    uint64_t *leaf = leaf_entry(pml4_phys, va, 0);
    if (leaf == NULL || (*leaf & PG_PRESENT) == 0) {
        return 0;
    }
    return *leaf & PG_ADDR_MASK;
}

void sandbox_destroy(uint64_t pml4_phys) {
    uint64_t *pml4 = tbl_virt(pml4_phys);
    /* 下半分 (ユーザー側) だけを歩いて解放する。上半分は
     * カーネルと共有なので触らない。 */
    for (int i = 0; i < 256; i++) {
        if ((pml4[i] & PG_PRESENT) == 0) {
            continue;
        }
        uint64_t *pdpt = tbl_virt(pml4[i] & PG_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if ((pdpt[j] & PG_PRESENT) == 0) {
                continue;
            }
            uint64_t *pd = tbl_virt(pdpt[j] & PG_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if ((pd[k] & PG_PRESENT) == 0) {
                    continue;
                }
                uint64_t *pt = tbl_virt(pd[k] & PG_ADDR_MASK);
                for (int m = 0; m < 512; m++) {
                    if ((pt[m] & PG_PRESENT) != 0) {
                        pmm_free_page(pt[m] & PG_ADDR_MASK);
                    }
                }
                pmm_free_page(pd[k] & PG_ADDR_MASK);
            }
            pmm_free_page(pdpt[j] & PG_ADDR_MASK);
        }
        pmm_free_page(pml4[i] & PG_ADDR_MASK);
    }
    pmm_free_page(pml4_phys);
}

/* ---- ウォッチドッグ (PIT IRQ0、約 100Hz) ---- */

/* ゲストの実行上限: 500 刻み = 約 5 秒。 */
#define WATCHDOG_LIMIT_TICKS 500u

static volatile uint32_t watchdog_ticks;
static int watchdog_running;

__attribute__((interrupt)) static void watchdog_irq(
    struct interrupt_frame *frame) {
    (void)frame;
    pic_send_eoi(0);
    if (compat_guest_active) {
        watchdog_ticks++;
        if (watchdog_ticks > WATCHDOG_LIMIT_TICKS) {
            /* このままゲストを捨ててランナーへ戻る。 */
            compat_guest_bail(COMPAT_KILL_WATCHDOG);
        }
    }
}

void sandbox_watchdog_start(void) {
    if (watchdog_running) {
        return;
    }
    watchdog_ticks = 0;
    idt_set_gate(32, (void *)watchdog_irq, 0x8E);
    /* PIT チャンネル 0 を約 100Hz にする (分周 11932)。
     * チャンネル 2 (スピーカー) とは独立。 */
    prism_outb(0x43, 0x36);
    prism_outb(0x40, (uint8_t)(11932 & 0xFF));
    prism_outb(0x40, (uint8_t)(11932 >> 8));
    /* IRQ0 のマスクを外す。 */
    uint8_t mask = prism_inb(0x21);
    prism_outb(0x21, (uint8_t)(mask & ~0x01));
    watchdog_running = 1;
}

void sandbox_watchdog_stop(void) {
    if (!watchdog_running) {
        return;
    }
    /* PIT チャンネル 0 の分周を元 (65536 = 約 18.2Hz) に
     * 戻す。戻し忘れると、ゲストを 1 回動かしただけで
     * 以後の PIT 待ち (UHCI の待ち・スピーカー・音楽)
     * が最大で約 5.5 倍速く狂う。 */
    prism_outb(0x43, 0x36);
    prism_outb(0x40, 0x00);
    prism_outb(0x40, 0x00);
    uint8_t mask = prism_inb(0x21);
    prism_outb(0x21, (uint8_t)(mask | 0x01));
    idt_set_gate(32, NULL, 0);
    watchdog_running = 0;
}
