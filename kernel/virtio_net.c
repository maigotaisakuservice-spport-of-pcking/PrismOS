/* PrismOS — virtio-net ドライバの実装 (virtio_blk.c と同じ
 * レガシー virtio PCI の作法で書く)。 */

#include "virtio_net.h"

#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "virtio_blk.h" /* virtio_queue_layout を共用 */

#define VNET_PCI_HOST_FEATURES 0x00
#define VNET_PCI_GUEST_FEATURES 0x04
#define VNET_PCI_QUEUE_ADDRESS 0x08
#define VNET_PCI_QUEUE_SIZE 0x0C
#define VNET_PCI_QUEUE_SELECT 0x0E
#define VNET_PCI_QUEUE_NOTIFY 0x10
#define VNET_PCI_STATUS 0x12
#define VNET_PCI_CONFIG 0x14

#define VNET_STATUS_ACK 1
#define VNET_STATUS_DRIVER 2
#define VNET_STATUS_DRIVER_OK 4

#define VNET_HDR_LEN 10
#define VNET_BUF_SIZE 2048
#define VNET_RX_BUFS 32

static uint64_t vnet_hhdm;
static uint16_t vnet_iobase;
static int vnet_found;
static uint8_t vnet_mac[6];

/* キューごとの状態。 */
typedef struct {
    uint64_t phys;       /* キュー領域の物理アドレス */
    uint32_t used_off;   /* used リングのオフセット */
    uint16_t qsize;
    uint16_t avail_idx;  /* 次に avail へ書く位置 (単調増加) */
    uint16_t used_seen;  /* used を読んだ位置 */
} vnet_queue;

static vnet_queue vnet_rxq;
static vnet_queue vnet_txq;
static uint64_t vnet_rx_data_phys; /* VNET_RX_BUFS 個の受信バッファ */
static uint64_t vnet_tx_data_phys; /* 送信用 1 バッファ */

static uint8_t io_in8(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static uint16_t io_in16(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static uint32_t io_in32(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void io_out8(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port));
}
static void io_out16(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port));
}
static void io_out32(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port));
}

static void *phys_virt(uint64_t phys) {
    return (void *)(uintptr_t)(phys + vnet_hhdm);
}

/* desc テーブルへの書き込み (16 バイト項目)。 */
static void desc_set(uint64_t qphys, uint16_t idx, uint64_t addr,
                     uint32_t len, uint16_t flags, uint16_t next) {
    uint8_t *d = (uint8_t *)phys_virt(qphys) + (size_t)idx * 16;
    for (int i = 0; i < 8; i++) {
        d[i] = (uint8_t)(addr >> (8 * i));
    }
    for (int i = 0; i < 4; i++) {
        d[8 + i] = (uint8_t)(len >> (8 * i));
    }
    d[12] = (uint8_t)(flags & 0xFF);
    d[13] = (uint8_t)(flags >> 8);
    d[14] = (uint8_t)(next & 0xFF);
    d[15] = (uint8_t)(next >> 8);
}

static void avail_push(vnet_queue *q, uint16_t desc_idx) {
    uint32_t desc_bytes, avail_off, used_off, total;
    virtio_queue_layout(q->qsize, &desc_bytes, &avail_off, &used_off,
                        &total);
    uint8_t *base = (uint8_t *)phys_virt(q->phys);
    /* avail リング: flags(2) idx(2) ring[qsize](2×) */
    uint16_t ring_pos = q->avail_idx % q->qsize;
    base[avail_off + 4 + (size_t)ring_pos * 2] =
        (uint8_t)(desc_idx & 0xFF);
    base[avail_off + 4 + (size_t)ring_pos * 2 + 1] =
        (uint8_t)(desc_idx >> 8);
    __asm__ volatile("mfence" ::: "memory");
    q->avail_idx++;
    base[avail_off + 2] = (uint8_t)(q->avail_idx & 0xFF);
    base[avail_off + 3] = (uint8_t)(q->avail_idx >> 8);
}

static uint16_t used_idx_now(const vnet_queue *q) {
    const uint8_t *base = (const uint8_t *)phys_virt(q->phys);
    return (uint16_t)(base[q->used_off + 2] |
                      (base[q->used_off + 3] << 8));
}

static int queue_setup(vnet_queue *q, uint16_t select) {
    io_out16(vnet_iobase + VNET_PCI_QUEUE_SELECT, select);
    q->qsize = io_in16(vnet_iobase + VNET_PCI_QUEUE_SIZE);
    if (q->qsize == 0) {
        return -1;
    }
    uint32_t desc_bytes, avail_off, total;
    virtio_queue_layout(q->qsize, &desc_bytes, &avail_off, &q->used_off,
                        &total);
    uint32_t pages = (total + 4095) / 4096;
    q->phys = pmm_alloc_pages(pages);
    if (q->phys == 0) {
        return -1;
    }
    uint8_t *base = (uint8_t *)phys_virt(q->phys);
    for (uint32_t i = 0; i < pages * 4096; i++) {
        base[i] = 0;
    }
    q->avail_idx = 0;
    q->used_seen = 0;
    io_out32(vnet_iobase + VNET_PCI_QUEUE_ADDRESS,
             (uint32_t)(q->phys >> 12));
    return 0;
}

int virtio_net_init(uint64_t hhdm_offset) {
    uint8_t bus, slot, func;
    vnet_found = 0;
    vnet_hhdm = hhdm_offset;
    if (!pci_find_device(0x1AF4, 0x1000, &bus, &slot, &func, NULL)) {
        return 0;
    }
    vnet_iobase = (uint16_t)pci_bar32_io(bus, slot, func, 0);
    if (vnet_iobase == 0) {
        return 0;
    }
    pci_enable_bus_master(bus, slot, func);

    io_out8(vnet_iobase + VNET_PCI_STATUS, 0);
    io_out8(vnet_iobase + VNET_PCI_STATUS, VNET_STATUS_ACK);
    io_out8(vnet_iobase + VNET_PCI_STATUS,
            VNET_STATUS_ACK | VNET_STATUS_DRIVER);
    (void)io_in32(vnet_iobase + VNET_PCI_HOST_FEATURES);
    io_out32(vnet_iobase + VNET_PCI_GUEST_FEATURES, 0);

    if (queue_setup(&vnet_rxq, 0) != 0 ||
        queue_setup(&vnet_txq, 1) != 0) {
        return 0;
    }
    /* 受信バッファ (32 個 × 2048) と送信バッファを用意する。 */
    vnet_rx_data_phys = pmm_alloc_pages(16);
    vnet_tx_data_phys = pmm_alloc_page();
    if (vnet_rx_data_phys == 0 || vnet_tx_data_phys == 0) {
        return 0;
    }
    uint8_t *rxb = (uint8_t *)phys_virt(vnet_rx_data_phys);
    for (int i = 0; i < VNET_RX_BUFS * VNET_BUF_SIZE; i++) {
        rxb[i] = 0;
    }
    /* 受信記述子を全部 avail に載せる。 */
    uint16_t nbuf = VNET_RX_BUFS;
    if (nbuf > vnet_rxq.qsize) {
        nbuf = vnet_rxq.qsize;
    }
    for (uint16_t i = 0; i < nbuf; i++) {
        desc_set(vnet_rxq.phys, i,
                 vnet_rx_data_phys + (uint64_t)i * VNET_BUF_SIZE,
                 VNET_BUF_SIZE, 2 /* WRITE */, 0);
        avail_push(&vnet_rxq, i);
    }
    io_out16(vnet_iobase + VNET_PCI_QUEUE_NOTIFY, 0);

    io_out8(vnet_iobase + VNET_PCI_STATUS,
            VNET_STATUS_ACK | VNET_STATUS_DRIVER |
                VNET_STATUS_DRIVER_OK);
    for (int i = 0; i < 6; i++) {
        vnet_mac[i] = io_in8(vnet_iobase + VNET_PCI_CONFIG + i);
    }
    vnet_found = 1;
    serial_write_line("[net] virtio-net found");
    return 1;
}

int virtio_net_present(void) {
    return vnet_found;
}

int virtio_net_link_up(void) {
    return vnet_found;
}

const uint8_t *virtio_net_ethaddr(void) {
    return vnet_mac;
}

int virtio_net_send(const uint8_t *frame, size_t len) {
    if (!vnet_found || len + VNET_HDR_LEN > VNET_BUF_SIZE) {
        return -1;
    }
    uint8_t *tx = (uint8_t *)phys_virt(vnet_tx_data_phys);
    for (int i = 0; i < VNET_HDR_LEN; i++) {
        tx[i] = 0;
    }
    for (size_t i = 0; i < len; i++) {
        tx[VNET_HDR_LEN + i] = frame[i];
    }
    desc_set(vnet_txq.phys, 0, vnet_tx_data_phys,
             (uint32_t)(VNET_HDR_LEN + len), 0, 0);
    avail_push(&vnet_txq, 0);
    io_out16(vnet_iobase + VNET_PCI_QUEUE_NOTIFY, 1);
    /* 送信完了 (used が進む) を上限つきで待つ。 */
    uint16_t want = (uint16_t)(vnet_txq.used_seen + 1);
    for (int spin = 0; spin < 100000; spin++) {
        if (used_idx_now(&vnet_txq) == want) {
            vnet_txq.used_seen = want;
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

int virtio_net_recv(uint8_t *buf, size_t max_len) {
    if (!vnet_found) {
        return -1;
    }
    if (used_idx_now(&vnet_rxq) == vnet_rxq.used_seen) {
        return 0;
    }
    const uint8_t *base = (const uint8_t *)phys_virt(vnet_rxq.phys);
    size_t elem_off = vnet_rxq.used_off + 4 +
                      (size_t)(vnet_rxq.used_seen % vnet_rxq.qsize) * 8;
    uint32_t id = (uint32_t)base[elem_off] |
                  ((uint32_t)base[elem_off + 1] << 8) |
                  ((uint32_t)base[elem_off + 2] << 16) |
                  ((uint32_t)base[elem_off + 3] << 24);
    uint32_t elen = (uint32_t)base[elem_off + 4] |
                    ((uint32_t)base[elem_off + 5] << 8) |
                    ((uint32_t)base[elem_off + 6] << 16) |
                    ((uint32_t)base[elem_off + 7] << 24);
    vnet_rxq.used_seen++;
    if (id >= VNET_RX_BUFS || elen <= VNET_HDR_LEN) {
        if (id < VNET_RX_BUFS) {
            avail_push(&vnet_rxq, (uint16_t)id);
            io_out16(vnet_iobase + VNET_PCI_QUEUE_NOTIFY, 0);
        }
        return 0;
    }
    uint32_t flen = elen - VNET_HDR_LEN;
    if (flen > max_len) {
        flen = (uint32_t)max_len;
    }
    const uint8_t *src = (const uint8_t *)phys_virt(
        vnet_rx_data_phys + (uint64_t)id * VNET_BUF_SIZE) + VNET_HDR_LEN;
    for (uint32_t i = 0; i < flen; i++) {
        buf[i] = src[i];
    }
    avail_push(&vnet_rxq, (uint16_t)id);
    io_out16(vnet_iobase + VNET_PCI_QUEUE_NOTIFY, 0);
    return (int)flen;
}
