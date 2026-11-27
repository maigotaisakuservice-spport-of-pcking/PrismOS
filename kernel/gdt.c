/* PrismOS — 自前の GDT と TSS (gdt.h を見ること)。 */
#include "gdt.h"

#include <stddef.h>
#include <stdint.h>

#include "string.h"

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t flags_limit;
    uint8_t base_high;
} __attribute__((packed)) gdt_entry;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr;

/* TSS (64 ビット形式、104 バイト)。 */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_entry;

static gdt_entry gdt[11]; /* 0x00..0x50 + TSS の上半分 */
static tss_entry tss;
static uint8_t tss_stack[32768] __attribute__((aligned(16)));

static void set_entry(int idx, uint32_t base, uint32_t limit,
                      uint8_t access, uint8_t flags) {
    gdt[idx].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[idx].base_low = (uint16_t)(base & 0xFFFF);
    gdt[idx].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[idx].access = access;
    gdt[idx].flags_limit =
        (uint8_t)(((limit >> 16) & 0x0F) | (flags << 4));
    gdt[idx].base_high = (uint8_t)((base >> 24) & 0xFF);
}

static void set_tss(int idx, uint64_t base, uint32_t limit) {
    set_entry(idx, (uint32_t)(base & 0xFFFFFFFFu), limit, 0x89, 0x0);
    /* TSS 記述子の上半分 (ベースの残り 32 ビット)。 */
    uint32_t *upper = (uint32_t *)&gdt[idx + 1];
    *upper = (uint32_t)((base >> 32) & 0xFFFFFFFFu);
    *(upper + 1) = 0;
}

void gdt_init(void) {
    memset(gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (uint64_t)(tss_stack + sizeof(tss_stack));
    tss.iopb_offset = (uint16_t)sizeof(tss_entry);

    /* アクセスバイト: P(0x80) | DPL | S(0x10) | 種別。
     * フラグ: G=8, D/B=4, L=2 (ニブル)。 */
    set_entry(1, 0, 0xFFFF, 0x9A, 0x0);        /* 0x08 16bit code */
    set_entry(2, 0, 0xFFFF, 0x92, 0x0);        /* 0x10 16bit data */
    set_entry(3, 0, 0xFFFFFFFFu, 0x9A, 0xC);   /* 0x18 32bit code */
    set_entry(4, 0, 0xFFFFFFFFu, 0x92, 0xC);   /* 0x20 32bit data */
    set_entry(5, 0, 0xFFFFFFFFu, 0x9A, 0xA);   /* 0x28 64bit code */
    set_entry(6, 0, 0xFFFFFFFFu, 0x92, 0xC);   /* 0x30 kernel data */
    set_entry(7, 0, 0xFFFFFFFFu, 0xF2, 0xC);   /* 0x38 user data */
    set_entry(8, 0, 0xFFFFFFFFu, 0xFA, 0xA);   /* 0x40 64bit user code */
    set_tss(9, (uint64_t)&tss, (uint32_t)sizeof(tss_entry) - 1);

    gdt_ptr ptr;
    ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    ptr.base = (uint64_t)gdt;
    __asm__ volatile("lgdt %0" : : "m"(ptr));

    /* データセグメントを読み直し、CS は far return で読み直す。 */
    __asm__ volatile(
        "mov $0x30, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "push $0x28\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        :
        :
        : "rax", "memory");
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

/* AP 用: 既存の GDT を読み直すだけ (LTR はしない。
 * TSS は BSP のものを指したままだが、AP はタスク切替を
 * しないので問題ない)。 */
void gdt_load_existing(void) {
    gdt_ptr ptr;
    ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    ptr.base = (uint64_t)gdt;
    __asm__ volatile("lgdt %0" : : "m"(ptr));
    __asm__ volatile(
        "mov $0x30, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "push $0x28\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        :
        :
        : "rax", "memory");
}
