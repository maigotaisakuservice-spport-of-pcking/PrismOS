/* PrismOS - e1000 driver (see e1000.h).
 *
 * Register layout follows the 82540EM data sheet and the
 * xv6-style sequence: reset, disable interrupts, set up
 * rings, enable RX/TX, set link up. RX buffers are 2048
 * bytes; TX uses one buffer per descriptor, reused only
 * after the card reports the descriptor done.
 */
#include "e1000.h"

#include "paging.h"
#include "rtc.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

#define E1000_CTRL 0x0000
#define E1000_STATUS 0x0008
#define E1000_IMS 0x00D0
#define E1000_IMC 0x00D8
#define E1000_RCTL 0x0100
#define E1000_TCTL 0x0400
#define E1000_TIPG 0x0410
#define E1000_RDBAL 0x2800
#define E1000_RDBAH 0x2804
#define E1000_RDLEN 0x2808
#define E1000_RDH 0x2810
#define E1000_RDT 0x2818
#define E1000_RAL 0x5400
#define E1000_GPRC 0x4074
#define E1000_GPTC 0x4080
#define E1000_TPR 0x40D0
#define E1000_TPT 0x40D4
#define E1000_TDBAL 0x3800
#define E1000_TDBAH 0x3804
#define E1000_TDLEN 0x3808
#define E1000_TDH 0x3810
#define E1000_TDT 0x3818

#define E1000_CTRL_RST (1u << 26)
#define E1000_CTRL_SLU (1u << 6)
#define E1000_CTRL_ASDE (1u << 5)
#define E1000_STATUS_LU (1u << 1)

#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_SBP (1u << 2)
#define E1000_RCTL_UPE (1u << 3)
#define E1000_RCTL_MPE (1u << 4)
#define E1000_RCTL_BAM (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)
/* Buffer size 2048 is encoded as 0 in bits 16-17. */
#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3)

#define E1000_RX_COUNT 32
#define E1000_TX_COUNT 8
#define E1000_RX_BUF 2048
#define E1000_TX_BUF 2048

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

static volatile uint32_t *e1000_regs;
static uint64_t e1000_hhdm;
static int e1000_ok;
static uint8_t e1000_hwaddr[6];

/* The card writes descriptor status by DMA, so the rings
 * must be volatile: without it the compiler may hoist the
 * status read out of the wait loop and the transmit would
 * always look unfinished. */
static volatile rx_desc_t *rx_ring;
static volatile tx_desc_t *tx_ring;
static uint64_t rx_ring_phys;
static uint64_t tx_ring_phys;
static uint8_t *rx_bufs; /* E1000_RX_COUNT * 2048, contiguous */
static uint64_t rx_bufs_phys;
static uint8_t *tx_bufs; /* E1000_TX_COUNT * 2048, contiguous */
static uint64_t tx_bufs_phys;
static uint32_t rx_tail; /* next descriptor the card may fill */
static uint32_t tx_tail;

static uint32_t reg_read(uint32_t off) {
    return e1000_regs[off / 4];
}

static void reg_write(uint32_t off, uint32_t v) {
    e1000_regs[off / 4] = v;
}

int e1000_init(uint64_t hhdm_offset) {
    e1000_ok = 0;
    e1000_hhdm = hhdm_offset;
    uint32_t bar0 = 0;
    uint8_t bus = 0, slot = 0, func = 0;
    if (!pci_find_device(0x8086, 0x100E, &bus, &slot, &func, &bar0)) {
        return -1; /* no card */
    }
    /* The card must be allowed to do DMA (read the rings
     * and buffers, write back status) or it can never
     * send or receive. Firmware leaves this off. */
    pci_enable_bus_master(bus, slot, func);
    if (bar0 == 0) {
        return -1;
    }
    /* The bootloader map covers RAM, not device memory, so
     * map the 128KB register block before touching it. */
    if (paging_map_mmio(bar0, 0x20000, hhdm_offset) != 0) {
        return -1;
    }
    e1000_regs = (volatile uint32_t *)(uintptr_t)(bar0 + hhdm_offset);
    {
        const char *hex = "0123456789abcdef";
        char tmp[20];
        tmp[0] = '0';
        tmp[1] = 'x';
        for (int i = 0; i < 8; i++) {
            tmp[2 + i] = hex[(bar0 >> (28 - i * 4)) & 0xF];
        }
        tmp[10] = '\0';
        serial_write("[net] card BAR0: ");
        serial_write_line(tmp);
    }

    /* Rings and buffers: contiguous physical pages. */
    uint64_t rp = pmm_alloc_pages(1);
    uint64_t tp = pmm_alloc_pages(1);
    uint64_t rb = pmm_alloc_pages(16); /* 32 * 2048 */
    uint64_t tb = pmm_alloc_pages(4);  /* 8 * 2048 */
    if (rp == 0 || tp == 0 || rb == 0 || tb == 0) {
        return -1;
    }
    rx_ring_phys = rp;
    tx_ring_phys = tp;
    rx_bufs_phys = rb;
    tx_bufs_phys = tb;
    rx_ring = (volatile rx_desc_t *)(uintptr_t)(rp + hhdm_offset);
    tx_ring = (volatile tx_desc_t *)(uintptr_t)(tp + hhdm_offset);
    rx_bufs = (uint8_t *)(uintptr_t)(rb + hhdm_offset);
    tx_bufs = (uint8_t *)(uintptr_t)(tb + hhdm_offset);
    memset((void *)rx_ring, 0, 4096);
    memset((void *)tx_ring, 0, 4096);
    memset(rx_bufs, 0, 16 * 4096);
    memset(tx_bufs, 0, 4 * 4096);

    /* Hardware address from the receive address registers. */
    uint32_t ral = reg_read(E1000_RAL);
    uint32_t rah = reg_read(E1000_RAL + 4);
    e1000_hwaddr[0] = (uint8_t)(ral & 0xFF);
    e1000_hwaddr[1] = (uint8_t)((ral >> 8) & 0xFF);
    e1000_hwaddr[2] = (uint8_t)((ral >> 16) & 0xFF);
    e1000_hwaddr[3] = (uint8_t)((ral >> 24) & 0xFF);
    e1000_hwaddr[4] = (uint8_t)(rah & 0xFF);
    e1000_hwaddr[5] = (uint8_t)((rah >> 8) & 0xFF);

    /* Reset, then disable interrupts and the RX/TX units
     * (same order as the reference driver: IMS=0, not IMC). */
    reg_write(E1000_IMS, 0);
    reg_write(E1000_RCTL, 0);
    reg_write(E1000_TCTL, E1000_TCTL_PSP);
    (void)reg_read(E1000_STATUS);
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | E1000_CTRL_RST);
    {
        /* Wait for the reset to finish, with a bound: a
         * card that never clears RST must not hang the
         * boot forever. The reference behaviour is a few
         * microseconds; one million polls is far beyond
         * that even on a slow emulator. */
        int i = 0;
        while ((reg_read(E1000_CTRL) & E1000_CTRL_RST) != 0) {
            if (++i >= 1000000) {
                serial_write_line("[net] card reset timed out");
                return -1;
            }
            __asm__ volatile("pause");
        }
    }
    reg_write(E1000_IMS, 0);
    (void)reg_read(E1000_STATUS);

    /* Link up: auto-negotiate and set link up. */
    reg_write(E1000_CTRL,
              reg_read(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    /* RX ring. */
    for (int i = 0; i < E1000_RX_COUNT; i++) {
        rx_ring[i].addr = rx_bufs_phys + (uint64_t)i * E1000_RX_BUF;
        rx_ring[i].status = 0;
    }
    reg_write(E1000_RDBAL, (uint32_t)(rx_ring_phys & 0xFFFFFFFFu));
    reg_write(E1000_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    reg_write(E1000_RDLEN,
              (uint32_t)(E1000_RX_COUNT * sizeof(rx_desc_t)));
    reg_write(E1000_RDH, 0);
    reg_write(E1000_RDT, E1000_RX_COUNT - 1);
    rx_tail = E1000_RX_COUNT - 1;

    /* TX ring. */
    for (int i = 0; i < E1000_TX_COUNT; i++) {
        tx_ring[i].addr = tx_bufs_phys + (uint64_t)i * E1000_TX_BUF;
        tx_ring[i].status = 0;
    }
    reg_write(E1000_TDBAL, (uint32_t)(tx_ring_phys & 0xFFFFFFFFu));
    reg_write(E1000_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    reg_write(E1000_TDLEN,
              (uint32_t)(E1000_TX_COUNT * sizeof(tx_desc_t)));
    reg_write(E1000_TDH, 0);
    reg_write(E1000_TDT, 0);
    tx_tail = 0;

    /* Inter-packet gap: 10/10/10 (same as the reference driver). */
    reg_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                              (0x10u << 4) | (0x40u << 12));
    reg_write(E1000_TIPG, (10u) | (10u << 10) | (10u << 20));
    reg_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                              E1000_RCTL_MPE | E1000_RCTL_SECRC);
    e1000_ok = 1;
    return 0;
}

int e1000_ready(void) {
    return e1000_ok;
}

const uint8_t *e1000_ethaddr(void) {
    return e1000_hwaddr;
}

int e1000_link_up(void) {
    if (!e1000_ok) {
        return 0;
    }
    return (reg_read(E1000_STATUS) & E1000_STATUS_LU) != 0;
}

int e1000_send(const uint8_t *frame, size_t len) {
    if (!e1000_ok || frame == NULL || len < 14 || len > 1514) {
        return -1;
    }
    uint32_t idx = tx_tail;
    /* A descriptor is free when it was never used (length 0)
     * or the card set the done bit after a transmit. */
    int free = 0;
    for (int i = 0; i < 1000000; i++) {
        if (tx_ring[idx].length == 0 || (tx_ring[idx].status & 0x01)) {
            free = 1;
            break;
        }
    }
    if (!free) {
        return -1;
    }
    uint8_t *dst = tx_bufs + (size_t)idx * E1000_TX_BUF;
    for (size_t i = 0; i < len; i++) {
        dst[i] = frame[i];
    }
    tx_ring[idx].length = (uint16_t)len;
    tx_ring[idx].cmd = 0x01 | 0x02 | 0x08; /* EOP | IFCS | RS */
    tx_ring[idx].status = 0;
    tx_ring[idx].cso = 0;
    tx_ring[idx].css = 0;
    tx_tail = (tx_tail + 1) % E1000_TX_COUNT;
    reg_write(E1000_TDT, tx_tail);
    /* Wait until the card has finished with this descriptor,
     * up to about 1 second. The descriptor-done bit is the
     * usual signal, but the head pointer advancing past the
     * descriptor is the authoritative one: the card only
     * moves the head after processing it. (QEMU's emulated
     * card advances the head here without setting the done
     * bit, so accept either.) */
    {
        prism_rtc_time t0, t1;
        (void)rtc_read(&t0);
        for (;;) {
            if (tx_ring[idx].status & 0x01) {
                return 0;
            }
            if (reg_read(E1000_TDH) != idx) {
                return 0;
            }
            (void)rtc_read(&t1);
            if (t1.second != t0.second || t1.minute != t0.minute) {
                break;
            }
        }
    }
    serial_write_line("[net] tx timeout");
    return -1;
}

int e1000_recv(uint8_t *buf, size_t max_len) {
    if (!e1000_ok || buf == NULL) {
        return -1;
    }
    uint32_t idx = (rx_tail + 1) % E1000_RX_COUNT;
    if ((rx_ring[idx].status & 0x01) == 0) {
        return 0; /* nothing waiting */
    }
    uint16_t len = rx_ring[idx].length;
    if (len > max_len) {
        len = (uint16_t)max_len;
    }
    uint8_t *src = rx_bufs + (size_t)idx * E1000_RX_BUF;
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = src[i];
    }
    rx_ring[idx].status = 0;
    rx_tail = idx;
    reg_write(E1000_RDT, rx_tail);
    return len;
}
