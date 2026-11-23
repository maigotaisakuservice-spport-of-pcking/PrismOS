/* PrismOS — ATA PIO の実装 (32bit)。 */

#include "ata32.h"

#include <stddef.h>

#define ATA_DATA 0x1F0
#define ATA_ERROR 0x1F1
#define ATA_COUNT 0x1F2
#define ATA_LBA_LO 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_CMD 0x1F7
#define ATA_ALT_STATUS 0x3F6

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

static uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port));
}
static uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port));
}

static int ata32_present;
static uint32_t ata32_sectors;

static int wait_bsy_clear(void) {
    for (int i = 0; i < 1000000; i++) {
        if ((inb(ATA_STATUS) & ATA_SR_BSY) == 0) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int wait_drq(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & ATA_SR_ERR) {
            return -1;
        }
        if (st & ATA_SR_DRQ) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

int ata32_init(uint32_t *sectors) {
    ata32_present = 0;
    outb(ATA_DRIVE, 0xA0); /* マスター */
    for (int i = 0; i < 4; i++) {
        (void)inb(ATA_ALT_STATUS);
    }
    if (wait_bsy_clear() != 0) {
        return 0;
    }
    outb(ATA_COUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, 0xEC); /* IDENTIFY */
    uint8_t st = inb(ATA_STATUS);
    if (st == 0) {
        return 0; /* ドライブなし */
    }
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) {
        return 0; /* ATAPI 等は対象外 */
    }
    if (wait_drq() != 0) {
        return 0;
    }
    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        id[i] = inw(ATA_DATA);
    }
    ata32_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (ata32_sectors == 0) {
        return 0;
    }
    ata32_present = 1;
    if (sectors != NULL) {
        *sectors = ata32_sectors;
    }
    return 1;
}

static int ata32_prepare(uint32_t lba) {
    if (!ata32_present || lba >= ata32_sectors) {
        return -1;
    }
    if (wait_bsy_clear() != 0) {
        return -1;
    }
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_COUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
    return 0;
}

int ata32_read(uint32_t lba, void *buf) {
    if (ata32_prepare(lba) != 0) {
        return -1;
    }
    outb(ATA_CMD, 0x20); /* READ SECTORS */
    if (wait_drq() != 0) {
        return -1;
    }
    uint16_t *out = (uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        out[i] = inw(ATA_DATA);
    }
    return 0;
}

int ata32_write(uint32_t lba, const void *buf) {
    if (ata32_prepare(lba) != 0) {
        return -1;
    }
    outb(ATA_CMD, 0x30); /* WRITE SECTORS */
    if (wait_drq() != 0) {
        return -1;
    }
    const uint16_t *in = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, in[i]);
    }
    outb(ATA_CMD, 0xEA); /* FLUSH CACHE */
    if (wait_bsy_clear() != 0) {
        return -1;
    }
    if (inb(ATA_STATUS) & ATA_SR_ERR) {
        return -1;
    }
    return 0;
}
