/* PrismOS — ブロックデバイス層 (ATA PIO / AHCI / virtio-blk) */
#include "block.h"
#include "ehci.h"
#include "xhci.h"
#include "uhci.h"

#include "ahci.h"
#include "ata.h"
#include "nvme.h"
#include "sdhci.h"
#include "virtio_blk.h"

static uint64_t block_hhdm;
static int block_found;
static int block_kind_id; /* 0=なし、1=ata、2=ahci、3=virtio、4=nvme */
static int nvme_extra; /* システムでない NVMe (機器 2) */

void block_set_hhdm(uint64_t hhdm_offset) {
    block_hhdm = hhdm_offset;
}

int block_init(void) {
    block_found = 0;
    block_kind_id = 0;
    nvme_extra = 0;
    if (ata_init()) {
        block_found = 1;
        block_kind_id = 1;
    } else if (ahci_init(block_hhdm)) {
        block_found = 1;
        block_kind_id = 2;
    } else if (virtio_blk_init(block_hhdm)) {
        block_found = 1;
        block_kind_id = 3;
    }
    /* NVMe は最後の候補 (システム) 兼、機器 2。 */
    if (nvme_init(block_hhdm)) {
        if (!block_found) {
            block_found = 1;
            block_kind_id = 4;
        } else {
            nvme_extra = 1;
        }
    }
    /* SD カードは機器 3 (追加デバイス専用)。 */
    sdhci_init(block_hhdm);
    return block_found;
}

int block_present(void) {
    return block_found;
}

const char *block_kind(void) {
    switch (block_kind_id) {
        case 1:
            return "ata";
        case 2:
            return "ahci";
        case 3:
            return "virtio";
        case 4:
            return "nvme";
        default:
            return "none";
    }
}

uint64_t block_total_sectors(void) {
    switch (block_kind_id) {
        case 1:
            return ata_total_sectors();
        case 2:
            return ahci_total_sectors();
        case 3:
            return virtio_blk_total_sectors();
        case 4:
            return nvme_total_sectors();
        default:
            return 0;
    }
}

const char *block_model(void) {
    switch (block_kind_id) {
        case 1:
            return ata_model();
        case 2:
            return ahci_model();
        case 3:
            return "VirtIO Block Device";
        case 4:
            return nvme_model();
        default:
            return "";
    }
}

/* Range check shared by all backends (AHCI and
 * virtio-blk used to trust the LBA; only ATA checked).
 * A device that reports 0 sectors is treated as unknown
 * and left to the backend, as before. */
static int block_lba_ok(uint64_t lba) {
    uint64_t total = block_total_sectors();
    return total == 0 || lba < total;
}

/* ---- 第 2 ディスク (USB マスストレージ) ----
 * 機器 0 がシステムディスク (従来通り)、機器 1 が
 * USB ストレージ。登録は main が UHCI 列挙の後に行う。
 * persist は機器 0 だけを使う。 */
static int usb_registered;
static uint64_t usb_sectors;

void block_register_usb(uint64_t sectors) {
    usb_registered = 1;
    usb_sectors = sectors;
}

/* ---- EHCI 経由の USB ストレージ (機器 4) ---- */
static int ehci_registered;
static uint64_t ehci_sectors;

void block_register_ehci(uint64_t sectors) {
    ehci_registered = 1;
    ehci_sectors = sectors;
}

/* ---- xHCI 経由の USB ストレージ (機器 5) ---- */
static int xhci_registered;
static uint64_t xhci_sectors;

void block_register_xhci(uint64_t sectors) {
    xhci_registered = 1;
    xhci_sectors = sectors;
}

int block_dev_count(void) {
    int n = block_found ? 1 : 0;
    if (usb_registered) {
        n++;
    }
    if (nvme_extra) {
        n++;
    }
    if (sdhci_present()) {
        n++;
    }
    if (ehci_registered) {
        n++;
    }
    if (xhci_registered) {
        n++;
    }
    return n;
}

const char *block_dev_kind(int dev) {
    if (dev == 0) {
        return block_kind();
    }
    if (dev == 1 && usb_registered) {
        return "usb";
    }
    if (dev == 2 && nvme_present()) {
        return "nvme";
    }
    if (dev == 3 && sdhci_present()) {
        return "sd";
    }
    if (dev == 4 && ehci_registered) {
        return "ehci";
    }
    if (dev == 5 && xhci_registered) {
        return "xhci";
    }
    return "none";
}

uint64_t block_dev_sectors(int dev) {
    if (dev == 0) {
        return block_total_sectors();
    }
    if (dev == 1 && usb_registered) {
        return usb_sectors;
    }
    if (dev == 2 && nvme_present()) {
        return nvme_total_sectors();
    }
    if (dev == 3 && sdhci_present()) {
        return sdhci_sectors();
    }
    if (dev == 4 && ehci_registered) {
        return ehci_sectors;
    }
    if (dev == 5 && xhci_registered) {
        return xhci_sectors;
    }
    return 0;
}

int block_read_sector_dev(int dev, uint64_t lba, uint8_t *buf) {
    if (dev == 0) {
        return block_read_sector(lba, buf);
    }
    if (dev == 1 && usb_registered) {
        if (lba >= usb_sectors) {
            return -1;
        }
        return uhci_storage_read_sector((uint32_t)lba, buf);
    }
    if (dev == 2 && nvme_present()) {
        return nvme_read_sector(lba, buf);
    }
    if (dev == 3 && sdhci_present()) {
        return sdhci_read_sector(lba, buf);
    }
    if (dev == 4 && ehci_registered) {
        if (lba >= ehci_sectors) {
            return -1;
        }
        return ehci_storage_read_sector((uint32_t)lba, buf);
    }
    if (dev == 5 && xhci_registered) {
        if (lba >= xhci_sectors) {
            return -1;
        }
        return xhci_storage_read_sector((uint32_t)lba, buf);
    }
    return -1;
}

int block_write_sector_dev(int dev, uint64_t lba,
                           const uint8_t *buf) {
    if (dev == 0) {
        return block_write_sector(lba, buf);
    }
    if (dev == 1 && usb_registered) {
        if (lba >= usb_sectors) {
            return -1;
        }
        return uhci_storage_write_sector((uint32_t)lba, buf);
    }
    if (dev == 2 && nvme_present()) {
        return nvme_write_sector(lba, buf);
    }
    if (dev == 3 && sdhci_present()) {
        return sdhci_write_sector(lba, buf);
    }
    if (dev == 4 && ehci_registered) {
        if (lba >= ehci_sectors) {
            return -1;
        }
        return ehci_storage_write_sector((uint32_t)lba, buf);
    }
    if (dev == 5 && xhci_registered) {
        if (lba >= xhci_sectors) {
            return -1;
        }
        return xhci_storage_write_sector((uint32_t)lba, buf);
    }
    return -1;
}

int block_read_sector(uint64_t lba, uint8_t *buf) {
    if (!block_lba_ok(lba)) {
        return -1;
    }
    switch (block_kind_id) {
        case 1:
            if (lba > 0x0FFFFFFFu) {
                return -1; /* ATA PIO は LBA28 まで */
            }
            return ata_read_sector((uint32_t)lba, buf);
        case 2:
            return ahci_read_sector(lba, buf);
        case 3:
            return virtio_blk_read_sector(lba, buf);
        case 4:
            return nvme_read_sector(lba, buf);
        default:
            return -1;
    }
}

int block_write_sector(uint64_t lba, const uint8_t *buf) {
    if (!block_lba_ok(lba)) {
        return -1;
    }
    switch (block_kind_id) {
        case 1:
            if (lba > 0x0FFFFFFFu) {
                return -1;
            }
            return ata_write_sector((uint32_t)lba, buf);
        case 2:
            return ahci_write_sector(lba, buf);
        case 3:
            return virtio_blk_write_sector(lba, buf);
        case 4:
            return nvme_write_sector(lba, buf);
        default:
            return -1;
    }
}
