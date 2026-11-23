/* PrismOS — virtio-blk ドライバ (従来型 virtio PCI、キュー 0) */
#include "virtio_blk.h"

#include "io.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

/* I/O ポートの配置 (BAR0 からのずれ)。 */
#define VIRTIO_PCI_HOST_FEATURES 0x00 /* 読む: 機器の機能 */
#define VIRTIO_PCI_GUEST_FEATURES 0x04 /* 書く: 使う機能 */
#define VIRTIO_PCI_QUEUE_ADDRESS 0x08 /* 書く: キューの PFN */
#define VIRTIO_PCI_QUEUE_SIZE 0x0C /* 読む: キューの項目数 (16) */
#define VIRTIO_PCI_QUEUE_SELECT 0x0E /* 書く: キューの番号 (16) */
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10 /* 書く: 通知するキュー (16) */
#define VIRTIO_PCI_STATUS 0x12 /* 書く: 状態 (8) */
#define VIRTIO_PCI_ISR 0x13 /* 読む: 割り込み状態 (8) */
#define VIRTIO_PCI_CONFIG 0x14 /* 機器固有の設定 */

#define VIRTIO_STATUS_ACK 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04

#define VIRTIO_DESC_F_NEXT 1
#define VIRTIO_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

static uint16_t vblk_iobase;
static uint64_t vblk_hhdm;
static int vblk_found;
static uint64_t vblk_sectors;

/* キューの物理ページ (記述子表 + 利用可能リング + 使用済み
 * リングを連続したページに置く)。データと状態も 1 ページ。 */
static uint64_t vblk_queue_phys;
static uint64_t vblk_queue_pages;
static uint16_t vblk_qsize;
static uint32_t vblk_used_off;
static uint64_t vblk_data_phys;
static uint16_t vblk_avail_idx; /* 次に置く利用可能リングの番号 */
static uint16_t vblk_used_idx;  /* 次に確認する使用済みリングの番号 */

static uint16_t vblk_io16(uint16_t off) {
    return prism_inw((uint16_t)(vblk_iobase + off));
}

static uint32_t vblk_io32(uint16_t off) {
    return prism_inl((uint16_t)(vblk_iobase + off));
}

static void vblk_io8_write(uint16_t off, uint8_t value) {
    prism_outb((uint16_t)(vblk_iobase + off), value);
}

static void vblk_io16_write(uint16_t off, uint16_t value) {
    prism_outw((uint16_t)(vblk_iobase + off), value);
}

static void vblk_io32_write(uint16_t off, uint32_t value) {
    prism_outl((uint16_t)(vblk_iobase + off), value);
}

/* ---- 純粋関数 ---- */

void virtio_blk_build_header(uint8_t *hdr, uint32_t type, uint64_t sector) {
    hdr[0] = (uint8_t)(type & 0xFF);
    hdr[1] = (uint8_t)((type >> 8) & 0xFF);
    hdr[2] = (uint8_t)((type >> 16) & 0xFF);
    hdr[3] = (uint8_t)((type >> 24) & 0xFF);
    hdr[4] = 0; /* ioprio */
    hdr[5] = 0;
    hdr[6] = 0;
    hdr[7] = 0;
    for (int i = 0; i < 8; i++) {
        hdr[8 + i] = (uint8_t)((sector >> (i * 8)) & 0xFF);
    }
}

void virtio_queue_layout(uint16_t qsize, uint32_t *desc_bytes,
                         uint32_t *avail_off, uint32_t *used_off,
                         uint32_t *total) {
    uint32_t desc = (uint32_t)qsize * 16;
    uint32_t avail = desc + 6 + (uint32_t)qsize * 2;
    /* 使用済みリングはページ境界から始める。 */
    uint32_t used = (avail + 4095) & ~4095u;
    uint32_t tot = used + 6 + (uint32_t)qsize * 8;
    if (desc_bytes != NULL) {
        *desc_bytes = desc;
    }
    if (avail_off != NULL) {
        *avail_off = desc;
    }
    if (used_off != NULL) {
        *used_off = used;
    }
    if (total != NULL) {
        *total = tot;
    }
}

void virtio_desc(uint8_t *desc, uint64_t addr, uint32_t len,
                 uint16_t flags, uint16_t next) {
    for (int i = 0; i < 8; i++) {
        desc[i] = (uint8_t)((addr >> (i * 8)) & 0xFF);
    }
    desc[8] = (uint8_t)(len & 0xFF);
    desc[9] = (uint8_t)((len >> 8) & 0xFF);
    desc[10] = (uint8_t)((len >> 16) & 0xFF);
    desc[11] = (uint8_t)((len >> 24) & 0xFF);
    desc[12] = (uint8_t)(flags & 0xFF);
    desc[13] = (uint8_t)((flags >> 8) & 0xFF);
    desc[14] = (uint8_t)(next & 0xFF);
    desc[15] = (uint8_t)((next >> 8) & 0xFF);
}

/* ---- ドライバ ---- */

static void *vblk_phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(phys + vblk_hhdm);
}

static uint16_t vblk_read16_at(uint32_t off) {
    uint8_t *p = (uint8_t *)vblk_phys_to_virt(vblk_queue_phys);
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}

static void vblk_write16_at(uint32_t off, uint16_t value) {
    uint8_t *p = (uint8_t *)vblk_phys_to_virt(vblk_queue_phys);
    p[off] = (uint8_t)(value & 0xFF);
    p[off + 1] = (uint8_t)((value >> 8) & 0xFF);
}

int virtio_blk_init(uint64_t hhdm_offset) {
    uint8_t bus, slot, func;
    vblk_found = 0;
    vblk_sectors = 0;
    vblk_hhdm = hhdm_offset;
    vblk_avail_idx = 0;
    vblk_used_idx = 0;

    if (!pci_find_device(0x1AF4, 0x1001, &bus, &slot, &func, NULL)) {
        return 0; /* virtio-blk が無い */
    }
    vblk_iobase = (uint16_t)pci_bar32_io(bus, slot, func, 0);
    if (vblk_iobase == 0) {
        return 0;
    }
    pci_enable_bus_master(bus, slot, func);

    /* 状態を 0 に戻してから ACK、DRIVER と進める。 */
    vblk_io8_write(VIRTIO_PCI_STATUS, 0);
    vblk_io8_write(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
    vblk_io8_write(VIRTIO_PCI_STATUS,
                   VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    /* 機能は使わない (0)。読み書きに要るものは無い。 */
    (void)vblk_io32(VIRTIO_PCI_HOST_FEATURES);
    vblk_io32_write(VIRTIO_PCI_GUEST_FEATURES, 0);

    /* キュー 0 の大きさを読む。 */
    vblk_io16_write(VIRTIO_PCI_QUEUE_SELECT, 0);
    vblk_qsize = vblk_io16(VIRTIO_PCI_QUEUE_SIZE);
    if (vblk_qsize == 0) {
        return 0;
    }
    uint32_t desc_bytes, avail_off, total;
    virtio_queue_layout(vblk_qsize, &desc_bytes, &avail_off, &vblk_used_off,
                        &total);
    vblk_queue_pages = (total + 4095) / 4096;
    vblk_queue_phys = pmm_alloc_pages((size_t)vblk_queue_pages);
    vblk_data_phys = pmm_alloc_page();
    if (vblk_queue_phys == 0 || vblk_data_phys == 0) {
        return 0;
    }
    memset(vblk_phys_to_virt(vblk_queue_phys), 0,
           (size_t)vblk_queue_pages * 4096);
    memset(vblk_phys_to_virt(vblk_data_phys), 0, 4096);
    /* キューの場所 (PFN) を教え、DRIVER_OK にする。 */
    vblk_io32_write(VIRTIO_PCI_QUEUE_ADDRESS,
                    (uint32_t)(vblk_queue_phys >> 12));
    vblk_io8_write(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK |
                                          VIRTIO_STATUS_DRIVER |
                                          VIRTIO_STATUS_DRIVER_OK);
    /* 設定の先頭 8 バイトが総セクタ数 (capacity)。 */
    uint32_t lo = vblk_io32(VIRTIO_PCI_CONFIG + 0);
    uint32_t hi = vblk_io32(VIRTIO_PCI_CONFIG + 4);
    vblk_sectors = (uint64_t)lo | ((uint64_t)hi << 32);
    if (vblk_sectors == 0) {
        return 0;
    }
    vblk_found = 1;
    return 1;
}

int virtio_blk_present(void) {
    return vblk_found;
}

uint64_t virtio_blk_total_sectors(void) {
    return vblk_sectors;
}

/* 1 回の要求を実行する。write = 1 なら書き込み。成功したら 0。 */
static int virtio_blk_request(uint64_t lba, int write) {
    uint8_t *q = (uint8_t *)vblk_phys_to_virt(vblk_queue_phys);
    uint8_t *data = (uint8_t *)vblk_phys_to_virt(vblk_data_phys);
    /* 要求ヘッダはデータページの後半に置く (16 バイト)。
     * 状態バイトはその次に置く。 */
    uint8_t *hdr = data + 1024;
    uint8_t *status = data + 1040;
    uint64_t hdr_phys = vblk_data_phys + 1024;
    uint64_t status_phys = vblk_data_phys + 1040;

    virtio_blk_build_header(hdr, write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN,
                            lba);
    *status = 0xFF;

    /* 記述子 0: ヘッダ (読み出し)、1: データ、2: 状態 (書き込み)。 */
    virtio_desc(q + 0 * 16, hdr_phys, 16, VIRTIO_DESC_F_NEXT, 1);
    virtio_desc(q + 1 * 16, vblk_data_phys, VIRTIO_BLK_SECTOR_SIZE,
                VIRTIO_DESC_F_NEXT | (write ? 0 : VIRTIO_DESC_F_WRITE), 2);
    virtio_desc(q + 2 * 16, status_phys, 1, VIRTIO_DESC_F_WRITE, 0);

    /* 利用可能リングに記述子 0 を置く。 */
    uint32_t desc_bytes, avail_off, used_off, total;
    virtio_queue_layout(vblk_qsize, &desc_bytes, &avail_off, &used_off,
                        &total);
    uint16_t idx = vblk_avail_idx;
    vblk_write16_at(avail_off + 4 + (idx % vblk_qsize) * 2, 0);
    vblk_avail_idx = (uint16_t)(idx + 1);
    vblk_write16_at(avail_off + 2, vblk_avail_idx);
    /* 通知して完了を待つ。virtio は非同期 (データ面) で処理する
     * ため、完了まで少し時間がかかる。TC-G 下では細かい輪で
     * 回し続けると相手の処理が進まないので、1 回ごとに pause
     * を入れる。完了判定はこの要求を出す直前の used 値を基準に
     * する (前の要求の遅れた完了を誤認しない)。 */
    uint16_t used_before = vblk_read16_at(used_off + 2);
    vblk_io16_write(VIRTIO_PCI_QUEUE_NOTIFY, 0);
    uint16_t used_seen = used_before;
    int rc = -1;
    for (int i = 0; i < 100000000; i++) {
        used_seen = vblk_read16_at(used_off + 2);
        if (used_seen != used_before) {
            vblk_used_idx = used_seen;
            rc = (*status == 0) ? 0 : -1;
            break;
        }
        __asm__ volatile("pause");
    }
    return rc;
}

int virtio_blk_read_sector(uint64_t lba, uint8_t *buf) {
    if (!vblk_found) {
        return -1;
    }
    if (virtio_blk_request(lba, 0) != 0) {
        return -1;
    }
    memcpy(buf, vblk_phys_to_virt(vblk_data_phys),
           VIRTIO_BLK_SECTOR_SIZE);
    return 0;
}

int virtio_blk_write_sector(uint64_t lba, const uint8_t *buf) {
    if (!vblk_found) {
        return -1;
    }
    memcpy(vblk_phys_to_virt(vblk_data_phys), buf,
           VIRTIO_BLK_SECTOR_SIZE);
    if (virtio_blk_request(lba, 1) != 0) {
        return -1;
    }
    return 0;
}
