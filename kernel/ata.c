/* PrismOS — ATA ディスクドライバ (PIO モード、プライマリ・マスタ) */
#include "ata.h"
#include "io.h"

#define ATA_DATA 0x1F0
#define ATA_ERROR 0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LO 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_COMMAND 0x1F7
#define ATA_ALT_STATUS 0x3F6

#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_DSC 0x10
#define ATA_STATUS_DRDY 0x40
#define ATA_STATUS_BSY 0x80

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ 0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_FLUSH 0xEA

static int ata_found;
static uint32_t ata_sectors;
static char ata_model_str[41];

/* 400ns ほどの待ち: 代替ステータスを 4 回読む。 */
static void ata_delay(void) {
    (void)prism_inb(ATA_ALT_STATUS);
    (void)prism_inb(ATA_ALT_STATUS);
    (void)prism_inb(ATA_ALT_STATUS);
    (void)prism_inb(ATA_ALT_STATUS);
}

/* BSY が消えるまで待つ。ERR が立ったら -2、時間切れは -1。 */
/* 待ち上限は回数ベース。以前は 100 万回だったが、CPU を
 * 絞られた仮想環境では QEMU のデバイス処理が間に合わず、
 * 正常な書き込みでも DRQ 待ちが時間切れになることが
 * あった (FDE 移行の連続書き込みで再現)。エラー時は
 * ERR ビットで即座に戻るので、上限を大きくしても
 * 異常系の応答は変わらない。 */
#define ATA_WAIT_MAX 30000000

static int ata_wait_not_busy(void) {
    for (int i = 0; i < ATA_WAIT_MAX; i++) {
        uint8_t st = prism_inb(ATA_STATUS);
        if ((st & ATA_STATUS_BSY) == 0) {
            if (st & ATA_STATUS_ERR) {
                return -2;
            }
            return 0;
        }
    }
    return -1;
}

/* BSY が消えて DRQ が立つまで待つ。 */
static int ata_wait_drq(void) {
    for (int i = 0; i < ATA_WAIT_MAX; i++) {
        uint8_t st = prism_inb(ATA_STATUS);
        if ((st & ATA_STATUS_BSY) == 0) {
            if (st & ATA_STATUS_ERR) {
                return -2;
            }
            if (st & ATA_STATUS_DRQ) {
                return 0;
            }
        }
    }
    return -1;
}

int ata_init(void) {
    ata_found = 0;
    ata_sectors = 0;
    ata_model_str[0] = '\0';

    /* マスタを選択する。 */
    prism_outb(ATA_DRIVE, 0xA0);
    ata_delay();
    uint8_t st = prism_inb(ATA_STATUS);
    if (st == 0xFF) {
        return 0; /* バスに何も無い (浮いたバス) */
    }
    if (ata_wait_not_busy() != 0) {
        return 0;
    }
    /* IDENTIFY を送る。 */
    prism_outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay();
    st = prism_inb(ATA_STATUS);
    if (st == 0) {
        return 0; /* 応答が無い */
    }
    if (ata_wait_drq() != 0) {
        /* ATAPI かもしれない: LBA 中位・上位が 0x14/0xEB なら
         * CD なので、ここでは対象外とする。 */
        return 0;
    }
    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        id[i] = prism_inw(ATA_DATA);
    }
    /* 総セクタ数 (ワード 60-61、リトルエンディアン)。 */
    ata_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    /* モデル名 (ワード 27-46、バイト順を入れ替える)。 */
    for (int i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        ata_model_str[i * 2] = (char)((w >> 8) & 0xFF);
        ata_model_str[i * 2 + 1] = (char)(w & 0xFF);
    }
    ata_model_str[40] = '\0';
    /* 末尾の空白を落とす。 */
    for (int i = 39; i >= 0; i--) {
        if (ata_model_str[i] == ' ' || ata_model_str[i] == '\0') {
            ata_model_str[i] = '\0';
        } else {
            break;
        }
    }
    if (ata_sectors == 0) {
        return 0;
    }
    ata_found = 1;
    return 1;
}

int ata_present(void) {
    return ata_found;
}

uint32_t ata_total_sectors(void) {
    return ata_sectors;
}

const char *ata_model(void) {
    return ata_model_str;
}

static int ata_setup_lba(uint32_t lba) {
    if (!ata_found || lba >= ata_sectors) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    prism_outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay();
    prism_outb(ATA_SECTOR_COUNT, 1);
    prism_outb(ATA_LBA_LO, (uint8_t)(lba & 0xFF));
    prism_outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    prism_outb(ATA_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
    return 0;
}

int ata_read_sector(uint32_t lba, uint8_t *buf) {
    if (buf == NULL || ata_setup_lba(lba) != 0) {
        return -1;
    }
    prism_outb(ATA_COMMAND, ATA_CMD_READ);
    if (ata_wait_drq() != 0) {
        return -1;
    }
    uint16_t *w = (uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        w[i] = prism_inw(ATA_DATA);
    }
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    if (buf == NULL || ata_setup_lba(lba) != 0) {
        return -1;
    }
    prism_outb(ATA_COMMAND, ATA_CMD_WRITE);
    if (ata_wait_drq() != 0) {
        return -1;
    }
    const uint16_t *w = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        prism_outw(ATA_DATA, w[i]);
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    return 0;
}

/* ディスクキャッシュを吐き出す。セクタごとにやると
 * ホスト側の同期書き込みが毎回走って同期処理全体が
 * 桁違いに遅くなるため、呼び出し側がひとまとまりの
 * 書き込みの最後に 1 回だけ呼ぶ。 */
int ata_flush_disk(void) {
    if (!ata_found) {
        return -1;
    }
    prism_outb(ATA_COMMAND, ATA_CMD_FLUSH);
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    return 0;
}
