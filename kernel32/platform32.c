/* PrismOS — 32bit 版のプラットフォームサービス。
 *
 * 共有カーネルコード (netstack / e1000 / tls / fb) が期待
 * する口を、ページングなし・恒等アドレスの 32bit 環境に
 * 合わせて提供する:
 *   - serial_* : COM1 への直接入出力 (main32 の静的関数とは
 *     別に、共有コード向けの公開版)
 *   - paging_map_* : 恒等写像なので何もしなくてよい
 *   - pmm_* : alloc32 のアリーナで代用する
 *   - virtio_net_* : 32bit 版は e1000 のみ対応 (不在を返す)
 * 時刻は kernel/rtc.c (CMOS) をそのまま使う。 */
#include <stddef.h>
#include <stdint.h>

#include "kernel32.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "virtio_net.h"

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_write_char(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    if (s == NULL) {
        return;
    }
    while (*s) {
        if (*s == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(*s);
        s++;
    }
}

void serial_write_line(const char *s) {
    serial_write(s);
    serial_write("\n");
}

int serial_has_data(void) {
    return (inb(COM1 + 5) & 0x01) != 0;
}

int serial_try_read_char(void) {
    if (!serial_has_data()) {
        return -1;
    }
    return inb(COM1);
}

char serial_read_char(void) {
    int c;
    do {
        c = serial_try_read_char();
    } while (c < 0);
    return (char)c;
}

size_t serial_read_line(char *buf, size_t cap) {
    size_t n = 0;
    if (cap == 0) {
        return 0;
    }
    for (;;) {
        char c = serial_read_char();
        if (c == '\r' || c == '\n') {
            serial_write("\r\n");
            break;
        }
        if ((c == '\b' || c == 0x7F) && n > 0) {
            n--;
            serial_write("\b \b");
            continue;
        }
        if (n + 1 < cap) {
            buf[n++] = c;
            serial_write_char(c);
        }
    }
    buf[n] = '\0';
    return n;
}

/* ページングなし: 物理アドレスがそのままアクセスできる。 */
int paging_map_mmio(uint64_t phys, uint64_t size,
                    uint64_t hhdm_offset) {
    (void)phys;
    (void)size;
    (void)hhdm_offset;
    return 0;
}

int paging_map_page_identity(uint64_t phys,
                             uint64_t hhdm_offset) {
    (void)phys;
    (void)hhdm_offset;
    return 0;
}

void pmm_init(const prism_memmap_entry *entries, size_t count) {
    (void)entries;
    (void)count;
}

uint64_t pmm_alloc_pages(size_t count) {
    void *p = alloc32(count * 4096u);
    return (uint64_t)(uintptr_t)p;
}

uint64_t pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

void pmm_free_pages(uint64_t addr, size_t count) {
    (void)count;
    free32((void *)(uintptr_t)addr);
}

void pmm_free_page(uint64_t addr) {
    free32((void *)(uintptr_t)addr);
}

uint64_t pmm_total_pages(void) {
    return (12u * 1024u * 1024u) / 4096u;
}

uint64_t pmm_free_pages_count(void) {
    return alloc32_free_bytes() / 4096u;
}

uint64_t pmm_used_pages_count(void) {
    return pmm_total_pages() - pmm_free_pages_count();
}

/* 32bit 版の NIC は e1000 のみ。virtio-net は不在として
 * 返し、netstack 側の選択を e1000 に倒す。 */
int virtio_net_init(uint64_t hhdm_offset) {
    (void)hhdm_offset;
    return -1;
}

int virtio_net_present(void) {
    return 0;
}

int virtio_net_link_up(void) {
    return 0;
}

static const uint8_t no_mac[6];

const uint8_t *virtio_net_ethaddr(void) {
    return no_mac;
}

int virtio_net_send(const uint8_t *frame, size_t len) {
    (void)frame;
    (void)len;
    return -1;
}

int virtio_net_recv(uint8_t *buf, size_t max_len) {
    (void)buf;
    (void)max_len;
    return -1;
}
