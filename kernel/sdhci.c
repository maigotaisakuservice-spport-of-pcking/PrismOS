/* PrismOS — SDHCI の実装。 */

#include "sdhci.h"

#include "paging.h"
#include "pci.h"
#include "serial.h"
#include "string.h"

#define SDMA_ADDR 0x00
#define SD_BLOCK_SIZE 0x04
#define SD_BLOCK_COUNT 0x06
#define SD_ARG 0x08
#define SD_XFER_MODE 0x0C
#define SD_CMD 0x0E
#define SD_RESP 0x10
#define SD_DATA_PORT 0x20
#define SD_PRESENT 0x24
#define SD_HOST_CTL 0x28
#define SD_POWER_CTL 0x29
#define SD_CLOCK_CTL 0x2C
#define SD_TIMEOUT_CTL 0x2E
#define SD_SW_RESET 0x2F
#define SD_NORM_INT 0x30
#define SD_ERR_INT 0x32
#define SD_CAPS 0x40

#define PRESENT_CMD_INHIBIT (1u << 0)
#define PRESENT_DAT_INHIBIT (1u << 1)
#define PRESENT_DAT_ACTIVE (1u << 2)
#define PRESENT_READ_READY (1u << 11)

#define NINT_CMD_DONE (1u << 0)
#define NINT_XFER_DONE (1u << 1)
#define NINT_BUF_WRITE (1u << 4)
#define NINT_BUF_READ (1u << 5)
#define NINT_ERROR (1u << 15)

#define TIMEOUT_LOOPS 2000000

static volatile uint8_t *sd_regs;
static int sd_found;
static int sd_high_capacity;
static uint16_t sd_rca;
static uint64_t sd_sector_count;

static uint8_t r8(uint32_t off) { return sd_regs[off]; }
static uint16_t r16(uint32_t off) {
    return *(volatile uint16_t *)(sd_regs + off);
}
static uint32_t r32(uint32_t off) {
    return *(volatile uint32_t *)(sd_regs + off);
}
static void w8(uint32_t off, uint8_t v) { sd_regs[off] = v; }
static void w16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(sd_regs + off) = v;
}
static void w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(sd_regs + off) = v;
}

static void print_hex32(uint32_t v) {
    const char *hex = "0123456789abcdef";
    char b[9];
    for (int i = 0; i < 8; i++) {
        b[i] = hex[(v >> (28 - 4 * i)) & 0xF];
    }
    b[8] = '\0';
    serial_write(b);
}

static int wait_present_clear(uint32_t mask) {
    for (int i = 0; i < TIMEOUT_LOOPS; i++) {
        if ((r32(SD_PRESENT) & mask) == 0) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

/* コマンドを発行する。resp_type: 0=なし 1=136bit 2=48bit
 * 3=48bit+busy。data=1 でデータ転送あり。 */
static int sd_cmd(uint32_t idx, uint32_t arg, int resp_type, int data,
                  uint32_t *resp0) {
    if (wait_present_clear(PRESENT_CMD_INHIBIT |
                           (data ? PRESENT_DAT_INHIBIT : 0)) != 0) {
        return -1;
    }
    w16(SD_NORM_INT, 0xFFFF);
    w16(SD_ERR_INT, 0xFFFF);
    w32(SD_ARG, arg);
    uint16_t cmd = (uint16_t)(idx << 8);
    if (data) {
        cmd |= 1 << 5;
    }
    if (resp_type != 0) {
        cmd |= 1 << 4; /* インデックスチェック */
        cmd |= 1 << 3; /* CRC チェック */
    }
    if (resp_type == 1) {
        cmd |= 1; /* 136bit */
    } else if (resp_type == 2) {
        cmd |= 2; /* 48bit */
    } else if (resp_type == 3) {
        cmd |= 3; /* 48bit + busy */
    }
    w16(SD_CMD, cmd);
    for (int i = 0; i < TIMEOUT_LOOPS; i++) {
        uint16_t st = r16(SD_NORM_INT);
        if (st & NINT_ERROR) {
            return -2;
        }
        if (st & NINT_CMD_DONE) {
            break;
        }
        if (i == TIMEOUT_LOOPS - 1) {
            return -3;
        }
        __asm__ volatile("pause");
    }
    if (resp_type == 3) {
        if (wait_present_clear(PRESENT_DAT_INHIBIT) != 0) {
            return -4;
        }
    }
    if (resp0 != NULL) {
        *resp0 = r32(SD_RESP);
    }
    return 0;
}

static int wait_int(uint16_t mask) {
    for (int i = 0; i < TIMEOUT_LOOPS; i++) {
        uint16_t st = r16(SD_NORM_INT);
        if (st & NINT_ERROR) {
            return -1;
        }
        if (st & mask) {
            w16(SD_NORM_INT, mask);
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -2;
}

int sdhci_init(uint64_t hhdm_offset) {
    uint8_t bus, slot, func;
    sd_found = 0;
    if (!pci_find_class(0x08, 0x05, 0x00, &bus, &slot, &func) &&
        !pci_find_class(0x08, 0x05, 0x01, &bus, &slot, &func)) {
        return 0;
    }
    uint32_t bar0 = pci_bar32_mem(bus, slot, func, 0);
    uint64_t bar_phys = bar0 & 0xFFFFFFF0u;
    if (bar_phys == 0) {
        return 0;
    }
    if ((bar0 & 0x6) == 0x4) {
        bar_phys |= (uint64_t)pci_config_read32(bus, slot, func,
                                                0x14) << 32;
    }
    pci_enable_bus_master(bus, slot, func);
    if (paging_map_mmio(bar_phys, 0x1000, hhdm_offset) != 0) {
        return 0;
    }
    sd_regs = (volatile uint8_t *)(uintptr_t)(bar_phys + hhdm_offset);

    /* ソフトウェアリセット (全回路)。 */
    w8(SD_SW_RESET, 0x01);
    for (int i = 0; i < TIMEOUT_LOOPS; i++) {
        if ((r8(SD_SW_RESET) & 0x01) == 0) {
            break;
        }
        if (i == TIMEOUT_LOOPS - 1) {
            serial_write_line("[sd] reset timeout");
            return 0;
        }
        __asm__ volatile("pause");
    }
    /* クロック: 内部クロック有効化し、まず低速 (識別モード) に。
     * 分周は CAPS のベースクロック依存だが、QEMU は 100MHz。
     * SDCLK = base / (2 * div)。div=128 → 約 390kHz。 */
    uint32_t caps = r32(SD_CAPS);
    (void)caps;
    w16(SD_CLOCK_CTL, (128 << 8) | 0x01);
    for (int i = 0; i < TIMEOUT_LOOPS; i++) {
        if (r16(SD_CLOCK_CTL) & 0x02) {
            break;
        }
        __asm__ volatile("pause");
    }
    w16(SD_CLOCK_CTL, (128 << 8) | 0x01 | 0x04); /* SDCLK 有効 */
    /* 電源: 3.3V でバスパワーオン。 */
    w8(SD_POWER_CTL, 0x0F);
    /* タイムアウト最大、割り込みステータス有効化 (ポーリング用)。 */
    w8(SD_TIMEOUT_CTL, 0x0E);
    w16(0x34, 0xFFFF);
    w16(0x36, 0xFFFF);
    /* ホスト制御: 既定 (1bit) のまま。 */

    /* カード初期化シーケンス。 */
    if (sd_cmd(0, 0, 0, 0, NULL) != 0) {
        serial_write_line("[sd] CMD0 failed");
        return 0;
    }
    uint32_t resp = 0;
    sd_high_capacity = 0;
    int v2 = 0;
    if (sd_cmd(8, 0x1AA, 2, 0, &resp) == 0 && (resp & 0xFF) == 0xAA) {
        v2 = 1;
    }
    uint32_t ocr = 0;
    int ready = 0;
    for (int i = 0; i < 1000 && !ready; i++) {
        if (sd_cmd(55, 0, 2, 0, NULL) != 0) {
            break;
        }
        uint32_t arg = 0x00FF8000;
        if (v2) {
            arg |= 1u << 30; /* HCS: 大容量対応を申告 */
        }
        if (sd_cmd(41, arg, 2, 0, &ocr) != 0) {
            break;
        }
        if (ocr & (1u << 31)) {
            ready = 1;
            sd_high_capacity = (ocr >> 30) & 1;
        }
        for (int j = 0; j < 20000; j++) {
            __asm__ volatile("pause");
        }
    }
    if (!ready) {
        serial_write_line("[sd] card did not become ready");
        return 0;
    }
    if (sd_cmd(2, 0, 1, 0, NULL) != 0) { /* CID */
        serial_write_line("[sd] CMD2 failed");
        return 0;
    }
    if (sd_cmd(3, 0, 2, 0, &resp) != 0) { /* RCA */
        serial_write_line("[sd] CMD3 failed");
        return 0;
    }
    sd_rca = (uint16_t)(resp >> 16);
    /* CSD を読んで容量を出す (CMD9, 136bit 応答)。 */
    uint32_t csd[4];
    if (wait_present_clear(PRESENT_CMD_INHIBIT) != 0) {
        return 0;
    }
    w16(SD_NORM_INT, 0xFFFF);
    w16(SD_ERR_INT, 0xFFFF);
    w32(SD_ARG, (uint32_t)sd_rca << 16);
    w16(SD_CMD, (9 << 8) | (1 << 4) | (1 << 3) | 1);
    for (int i = 0; i < TIMEOUT_LOOPS; i++) {
        uint16_t st = r16(SD_NORM_INT);
        if (st & NINT_ERROR) {
            serial_write_line("[sd] CMD9 error");
            return 0;
        }
        if (st & NINT_CMD_DONE) {
            break;
        }
        __asm__ volatile("pause");
    }
    for (int i = 0; i < 4; i++) {
        csd[i] = r32(SD_RESP + (uint32_t)i * 4);
    }
    /* 応答レジスタから CSD (16 バイト) を再構成する。
     * SDHCI は R2 応答を CRC 込みでずらして格納する
     * ため、各ワードを 8 ビット上げて下位ワードの最上位
     * バイトでつなぐ。 */
    {
        uint32_t resp[4];
        resp[0] = (csd[3] << 8) | (csd[2] >> 24);
        resp[1] = (csd[2] << 8) | (csd[1] >> 24);
        resp[2] = (csd[1] << 8) | (csd[0] >> 24);
        resp[3] = csd[0] << 8;
        uint8_t b[16];
        for (int i = 0; i < 4; i++) {
            b[i * 4 + 0] = (uint8_t)(resp[i] >> 24);
            b[i * 4 + 1] = (uint8_t)(resp[i] >> 16);
            b[i * 4 + 2] = (uint8_t)(resp[i] >> 8);
            b[i * 4 + 3] = (uint8_t)resp[i];
        }
        int version = b[0] >> 6;
        if (version == 1) {
            /* CSD v2 (SDHC): C_SIZE [69:48]、容量は
             * (C_SIZE+1) * 512KiB。 */
            uint32_t csize = ((uint32_t)(b[7] & 0x3F) << 16) |
                             ((uint32_t)b[8] << 8) | b[9];
            sd_sector_count = ((uint64_t)csize + 1) * 1024;
        } else {
            /* CSD v1 (SDSC): C_SIZE [73:62]、
             * C_SIZE_MULT [49:47]、READ_BL_LEN [83:80]。 */
            uint32_t csize = ((uint32_t)(b[6] & 0x03) << 10) |
                             ((uint32_t)b[7] << 2) | (b[8] >> 6);
            uint32_t mult = ((uint32_t)(b[9] & 0x03) << 1) |
                            (b[10] >> 7);
            uint32_t blen = b[5] & 0x0F;
            uint64_t bytes = ((uint64_t)csize + 1) <<
                             (mult + 2 + blen);
            sd_sector_count = bytes / 512;
        }
    }
    if (sd_cmd(7, (uint32_t)sd_rca << 16, 3, 0, NULL) != 0) {
        serial_write_line("[sd] CMD7 failed");
        return 0;
    }
    if (!sd_high_capacity) {
        if (sd_cmd(16, 512, 2, 0, NULL) != 0) {
            serial_write_line("[sd] CMD16 failed");
            return 0;
        }
    }
    /* 高速クロックへ: div=2 → 25MHz。 */
    w16(SD_CLOCK_CTL, (2 << 8) | 0x01);
    for (int i = 0; i < TIMEOUT_LOOPS; i++) {
        if (r16(SD_CLOCK_CTL) & 0x02) {
            break;
        }
        __asm__ volatile("pause");
    }
    w16(SD_CLOCK_CTL, (2 << 8) | 0x01 | 0x04);

    sd_found = 1;
    serial_write("[sd] card ready: ");
    {
        char b[24];
        int n = 0;
        uint64_t v = sd_sector_count;
        char tmp[24];
        int t = 0;
        if (v == 0) {
            tmp[t++] = '0';
        }
        while (v > 0) {
            tmp[t++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (t > 0) {
            b[n++] = tmp[--t];
        }
        b[n] = '\0';
        serial_write(b);
    }
    serial_write_line(" sectors");
    return 1;
}

int sdhci_present(void) {
    return sd_found;
}

uint64_t sdhci_sectors(void) {
    return sd_found ? sd_sector_count : 0;
}

static uint32_t sd_addr(uint64_t lba) {
    return sd_high_capacity ? (uint32_t)lba : (uint32_t)(lba * 512);
}

int sdhci_read_sector(uint64_t lba, void *buf) {
    if (!sd_found || lba >= sd_sector_count) {
        return -1;
    }
    w16(SD_BLOCK_SIZE, 512);
    w16(SD_BLOCK_COUNT, 1);
    /* 転送モード: 読み取り方向。 */
    w16(SD_XFER_MODE, 1 << 4);
    if (sd_cmd(17, sd_addr(lba), 2, 1, NULL) != 0) {
        return -2;
    }
    if (wait_int(NINT_BUF_READ) != 0) {
        return -3;
    }
    uint32_t *out = (uint32_t *)buf;
    for (int i = 0; i < 128; i++) {
        out[i] = r32(SD_DATA_PORT);
    }
    if (wait_int(NINT_XFER_DONE) != 0) {
        return -4;
    }
    return 0;
}

int sdhci_write_sector(uint64_t lba, const void *buf) {
    if (!sd_found || lba >= sd_sector_count) {
        return -1;
    }
    w16(SD_BLOCK_SIZE, 512);
    w16(SD_BLOCK_COUNT, 1);
    w16(SD_XFER_MODE, 0); /* 書き込み方向 */
    if (sd_cmd(24, sd_addr(lba), 2, 1, NULL) != 0) {
        return -2;
    }
    if (wait_int(NINT_BUF_WRITE) != 0) {
        return -3;
    }
    const uint32_t *in = (const uint32_t *)buf;
    for (int i = 0; i < 128; i++) {
        w32(SD_DATA_PORT, in[i]);
    }
    if (wait_int(NINT_XFER_DONE) != 0) {
        return -4;
    }
    return 0;
}
