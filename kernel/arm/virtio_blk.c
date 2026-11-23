/* PrismOS — ARM 用 virtio ブロックデバイス (virtio-mmio) */
#include "virtio_blk.h"

#include <stddef.h>

#include "virtio.h"

#define VIRTIO_ID_BLOCK 2u
#define BLK_T_IN 0u
#define BLK_T_OUT 1u
#define BLK_S_OK 0u

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} blk_req_header;

static virtio_dev blk_dev;
static int blk_ready;
static uint64_t blk_capacity;
static blk_req_header req_hdr;
static uint8_t req_status;
static uint8_t bounce[512];

int arm_blk_init(void) {
    if (virtio_mmio_probe(VIRTIO_ID_BLOCK, &blk_dev) != 0) {
        return -1;
    }
    if (virtio_queue_init(&blk_dev, 0) != 0) {
        return -1;
    }
    virtio_driver_ok(&blk_dev);
    blk_capacity = virtio_config64(&blk_dev, 0);
    blk_ready = 1;
    return 0;
}

int arm_blk_present(void) {
    return blk_ready;
}

uint64_t arm_blk_sectors(void) {
    return blk_capacity;
}

static int blk_xfer(uint64_t lba, uint8_t *buf, int is_write) {
    virtio_seg segs[3];
    uint32_t used_len = 0;

    if (!blk_ready || lba >= blk_capacity) {
        return -1;
    }
    req_hdr.type = is_write ? BLK_T_OUT : BLK_T_IN;
    req_hdr.reserved = 0;
    req_hdr.sector = lba;
    req_status = 0xff;

    segs[0].buf = &req_hdr;
    segs[0].len = sizeof(req_hdr);
    segs[0].write = 0;
    segs[1].buf = buf;
    segs[1].len = 512;
    segs[1].write = is_write ? 0 : 1;
    segs[2].buf = &req_status;
    segs[2].len = 1;
    segs[2].write = 1;

    if (virtio_exec(&blk_dev, 0, segs, 3, &used_len, 5000) != 0) {
        return -1;
    }
    return req_status == BLK_S_OK ? 0 : -1;
}

int arm_blk_read(uint64_t lba, uint8_t *buf) {
    return blk_xfer(lba, buf, 0);
}

int arm_blk_write(uint64_t lba, const uint8_t *buf) {
    int i;
    for (i = 0; i < 512; i++) {
        bounce[i] = buf[i];
    }
    return blk_xfer(lba, bounce, 1);
}
