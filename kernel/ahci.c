/* PrismOS — AHCI SATA ディスクドライバ (q35 構成、ポート 0) */
#include "ahci.h"

#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

/* ABAR の配置 (AHCI 仕様 1.3)。 */
#define AHCI_CAP 0x00   /* 機能 */
#define AHCI_GHC 0x04   /* 全体制御 */
#define AHCI_PI 0x0C    /* 実装ポート */
#define AHCI_GHC_AE (1u << 31)

/* ポートレジスタ (ポート開始 + 0x80 * n)。 */
#define AHCI_P_CLB 0x00  /* コマンド一覧ベース (下位) */
#define AHCI_P_CLBU 0x04 /* コマンド一覧ベース (上位) */
#define AHCI_P_FB 0x08   /* 受信 FIS ベース (下位) */
#define AHCI_P_FBU 0x0C  /* 受信 FIS ベース (上位) */
#define AHCI_P_IS 0x10   /* 割り込み状態 */
#define AHCI_P_CMD 0x18  /* コマンドと状態 */
#define AHCI_P_TFD 0x20  /* タスクファイルデータ */
#define AHCI_P_SIG 0x24  /* 署名 */
#define AHCI_P_SSTS 0x28 /* シリアル ATA 状態 */
#define AHCI_P_CI 0x38   /* コマンド発行 */

#define AHCI_P_CMD_ST (1u << 0)  /* 開始 */
#define AHCI_P_CMD_FRE (1u << 4) /* FIS 受信許可 */
#define AHCI_P_CMD_FR (1u << 14) /* FIS 受信中 */
#define AHCI_P_CMD_CR (1u << 15) /* コマンド一覧実行中 */

#define AHCI_CMD_READ_DMA_EXT 0x25
#define AHCI_CMD_WRITE_DMA_EXT 0x35
#define AHCI_CMD_IDENTIFY 0xEC

#define AHCI_FIS_REG_H2D 0x27

static volatile uint32_t *ahci_abar;
static uint64_t ahci_hhdm;
static int ahci_found;
static int ahci_port; /* 使うポート番号 */
static uint64_t ahci_sectors;
static char ahci_model_str[41];

/* DMA 用の物理ページ (コマンド一覧 1KB、受信 FIS 256B、
 * コマンド表 256B、データ 512B を各 1 ページに置く)。 */
static uint64_t ahci_cmd_list_phys;
static uint64_t ahci_fis_phys;
static uint64_t ahci_cmd_table_phys;
static uint64_t ahci_data_phys;

static uint32_t abar_read(uint32_t off) {
    return ahci_abar[off / 4];
}

static void abar_write(uint32_t off, uint32_t value) {
    ahci_abar[off / 4] = value;
}

static uint32_t port_read(uint32_t off) {
    return abar_read(0x100 + (uint32_t)ahci_port * 0x80 + off);
}

static void port_write(uint32_t off, uint32_t value) {
    abar_write(0x100 + (uint32_t)ahci_port * 0x80 + off, value);
}

/* ---- 純粋関数 ---- */

/* コマンド一覧の項目 0 の DW0 を作る。CFL=5、書き込みなら
 * W ビット、PRDTL は DW0 のビット 31:16 に入る。
 * prdtl は PRDT 項目の数。 */
uint32_t ahci_cmd_header_flags(int write, uint16_t prdtl) {
    uint32_t flags = 5; /* CFL = 5 (20 バイト / 4) */
    if (write) {
        flags |= (1u << 6); /* W ビット */
    }
    flags |= (uint32_t)prdtl << 16; /* PRDTL */
    return flags;
}

void ahci_build_reg_fis(uint8_t *fis, uint8_t cmd, uint64_t lba,
                        uint16_t count) {
    for (int i = 0; i < 20; i++) {
        fis[i] = 0;
    }
    fis[0] = AHCI_FIS_REG_H2D;
    fis[1] = 0x80; /* C ビット: コマンド FIS */
    fis[2] = cmd;
    fis[3] = 0;    /* 機能 */
    fis[4] = (uint8_t)(lba & 0xFF);
    fis[5] = (uint8_t)((lba >> 8) & 0xFF);
    fis[6] = (uint8_t)((lba >> 16) & 0xFF);
    fis[7] = 0x40; /* デバイス: LBA モード */
    fis[8] = (uint8_t)((lba >> 24) & 0xFF);
    fis[9] = (uint8_t)((lba >> 32) & 0xFF);
    fis[10] = (uint8_t)((lba >> 40) & 0xFF);
    fis[11] = 0;
    fis[12] = (uint8_t)(count & 0xFF);
    fis[13] = (uint8_t)((count >> 8) & 0xFF);
    fis[14] = 0;
    fis[15] = 0;
    fis[16] = 0;
    fis[17] = 0;
    fis[18] = 0;
    fis[19] = 0;
}

void ahci_prdt_entry(uint8_t *entry, uint64_t addr, uint32_t byte_count) {
    uint32_t dbc = byte_count - 1; /* 0 起算のバイト数 */
    entry[0] = (uint8_t)(addr & 0xFF);
    entry[1] = (uint8_t)((addr >> 8) & 0xFF);
    entry[2] = (uint8_t)((addr >> 16) & 0xFF);
    entry[3] = (uint8_t)((addr >> 24) & 0xFF);
    entry[4] = (uint8_t)((addr >> 32) & 0xFF);
    entry[5] = (uint8_t)((addr >> 40) & 0xFF);
    entry[6] = (uint8_t)((addr >> 48) & 0xFF);
    entry[7] = (uint8_t)((addr >> 56) & 0xFF);
    entry[8] = 0;
    entry[9] = 0;
    entry[10] = 0;
    entry[11] = 0;
    entry[12] = (uint8_t)(dbc & 0xFF);
    entry[13] = (uint8_t)((dbc >> 8) & 0xFF);
    entry[14] = (uint8_t)((dbc >> 16) & 0xFF);
    entry[15] = (uint8_t)((dbc >> 24) & 0xFF); /* I ビットは 0 */
}

void ahci_parse_identify(const uint16_t *id, char *model, size_t model_cap,
                         uint64_t *sectors) {
    if (model_cap > 0) {
        size_t n = 0;
        for (int i = 0; i < 20 && n + 2 < model_cap; i++) {
            uint16_t w = id[27 + i];
            model[n++] = (char)((w >> 8) & 0xFF);
            model[n++] = (char)(w & 0xFF);
        }
        model[n] = '\0';
        /* 末尾の空白を落とす。 */
        for (size_t i = n; i > 0; i--) {
            if (model[i - 1] == ' ' || model[i - 1] == '\0') {
                model[i - 1] = '\0';
            } else {
                break;
            }
        }
    }
    uint64_t s48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                   ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    if (s48 != 0) {
        *sectors = s48;
    } else {
        *sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    }
}

/* ---- ドライバ ---- */

/* コマンド一覧・受信 FIS・コマンド表・データの各ページを
 * 0 で埋める。仮想アドレスは物理 + HHDM。 */
static void *ahci_phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(phys + ahci_hhdm);
}

static int ahci_wait_port_idle(void) {
    for (int i = 0; i < 1000000; i++) {
        uint32_t cmd = port_read(AHCI_P_CMD);
        if ((cmd & (AHCI_P_CMD_ST | AHCI_P_CMD_FRE | AHCI_P_CMD_FR |
                    AHCI_P_CMD_CR)) == 0) {
            return 0;
        }
    }
    return -1;
}

/* 1 コマンドを実行する。cmd は ATA コマンド、lba/count は
 * 読み書き用。data_phys は転送先 (読み) または転送元
 * (書き) の物理アドレス。write = 1 なら書き込み。
 * 成功したら 0。 */
static int ahci_run_cmd(uint8_t cmd, uint64_t lba, uint16_t count,
                        uint64_t data_phys, int write) {
    uint8_t *table = (uint8_t *)ahci_phys_to_virt(ahci_cmd_table_phys);
    uint8_t *list = (uint8_t *)ahci_phys_to_virt(ahci_cmd_list_phys);
    /* コマンド表: CFIS (64B) + 予約 + PRDT (128B から)。 */
    for (int i = 0; i < 256; i++) {
        table[i] = 0;
    }
    ahci_build_reg_fis(table, cmd, lba, count);
    ahci_prdt_entry(table + 128, data_phys, AHCI_SECTOR_SIZE);
    /* コマンド一覧の項目 0: 1 PRDT、書き込みなら W ビット。 */
    for (int i = 0; i < 32; i++) {
        list[i] = 0;
    }
    uint32_t flags = ahci_cmd_header_flags(write, 1);
    list[0] = (uint8_t)(flags & 0xFF);
    list[1] = (uint8_t)((flags >> 8) & 0xFF);
    list[2] = (uint8_t)((flags >> 16) & 0xFF);
    list[3] = (uint8_t)((flags >> 24) & 0xFF);
    /* PRDBC は 0 のまま。コマンド表のアドレス (64 ビット)。 */
    uint64_t ct = ahci_cmd_table_phys;
    for (int i = 0; i < 8; i++) {
        list[8 + i] = (uint8_t)((ct >> (i * 8)) & 0xFF);
    }
    /* 割り込み状態を消してから発行する。 */
    port_write(AHCI_P_IS, 0xFFFFFFFFu);
    port_write(AHCI_P_CI, 1u);
    for (int i = 0; i < 1000000; i++) {
        if ((port_read(AHCI_P_CI) & 1u) == 0) {
            uint8_t tfd = (uint8_t)(port_read(AHCI_P_TFD) & 0xFF);
            if (tfd & 0x01) { /* ERR */
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

int ahci_init(uint64_t hhdm_offset) {
    uint8_t bus, slot, func;
    ahci_found = 0;
    ahci_sectors = 0;
    ahci_model_str[0] = '\0';
    ahci_hhdm = hhdm_offset;

    if (!pci_find_class(0x01, 0x06, 0x01, &bus, &slot, &func)) {
        return 0; /* AHCI コントローラが無い */
    }
    uint32_t abar_phys = pci_bar32_mem(bus, slot, func, 5);
    if (abar_phys == 0) {
        return 0;
    }
    pci_enable_bus_master(bus, slot, func);
    if (paging_map_mmio(abar_phys, 0x2000, hhdm_offset) != 0) {
        return 0;
    }
    ahci_abar = (volatile uint32_t *)(uintptr_t)(abar_phys + hhdm_offset);

    /* AHCI を有効にする。 */
    uint32_t ghc = abar_read(AHCI_GHC);
    ghc |= AHCI_GHC_AE;
    abar_write(AHCI_GHC, ghc);
    uint32_t pi = abar_read(AHCI_PI);
    /* 実装ポートの中から、SATA ディスク (署名 0x00000101) が
     * 繋がっている最初のものを選ぶ。CD (ATAPI) は使わない。 */
    ahci_port = -1;
    for (int p = 0; p < 32; p++) {
        if ((pi & (1u << p)) == 0) {
            continue;
        }
        uint32_t ssts = abar_read(0x100 + (uint32_t)p * 0x80 + AHCI_P_SSTS);
        if ((ssts & 0x0F) != 3) {
            continue; /* 機器が繋がっていない */
        }
        uint32_t sig = abar_read(0x100 + (uint32_t)p * 0x80 + AHCI_P_SIG);
        if (sig == 0x00000101u) {
            ahci_port = p;
            break;
        }
    }
    if (ahci_port < 0) {
        return 0; /* SATA ディスクが無い */
    }

    /* DMA 用のページを確保する。 */
    ahci_cmd_list_phys = pmm_alloc_page();
    ahci_fis_phys = pmm_alloc_page();
    ahci_cmd_table_phys = pmm_alloc_page();
    ahci_data_phys = pmm_alloc_page();
    if (ahci_cmd_list_phys == 0 || ahci_fis_phys == 0 ||
        ahci_cmd_table_phys == 0 || ahci_data_phys == 0) {
        return 0;
    }
    memset(ahci_phys_to_virt(ahci_cmd_list_phys), 0, 4096);
    memset(ahci_phys_to_virt(ahci_fis_phys), 0, 4096);
    memset(ahci_phys_to_virt(ahci_cmd_table_phys), 0, 4096);
    memset(ahci_phys_to_virt(ahci_data_phys), 0, 4096);

    /* 選んだポートを止めてから一覧・FIS の場所を教える。
     * ST と FRE の両方を落とす (FRE を残すと FR が消えず、
     * ポートが止まらない)。 */
    uint32_t cmd = port_read(AHCI_P_CMD);
    cmd &= ~(AHCI_P_CMD_ST | AHCI_P_CMD_FRE);
    port_write(AHCI_P_CMD, cmd);
    if (ahci_wait_port_idle() != 0) {
        return 0;
    }
    port_write(AHCI_P_CLB, (uint32_t)(ahci_cmd_list_phys & 0xFFFFFFFFu));
    port_write(AHCI_P_CLBU, (uint32_t)(ahci_cmd_list_phys >> 32));
    port_write(AHCI_P_FB, (uint32_t)(ahci_fis_phys & 0xFFFFFFFFu));
    port_write(AHCI_P_FBU, (uint32_t)(ahci_fis_phys >> 32));
    /* FIS 受信とコマンド処理を開始する。 */
    cmd = port_read(AHCI_P_CMD);
    cmd |= AHCI_P_CMD_FRE | AHCI_P_CMD_ST;
    port_write(AHCI_P_CMD, cmd);
    /* デバイス検出 (SSTS の DET=3) を待つ。 */
    int det_ok = 0;
    for (int i = 0; i < 1000000; i++) {
        if ((port_read(AHCI_P_SSTS) & 0x0F) == 3) {
            det_ok = 1;
            break;
        }
    }
    if (!det_ok) {
        return 0;
    }
    /* 署名は SATA ディスクで 0x00000101。違っても続ける。 */
    (void)port_read(AHCI_P_SIG);

    /* IDENTIFY でモデル名と総セクタ数を取る。 */
    if (ahci_run_cmd(AHCI_CMD_IDENTIFY, 0, 1, ahci_data_phys, 0) != 0) {
        return 0;
    }
    ahci_parse_identify((const uint16_t *)ahci_phys_to_virt(ahci_data_phys),
                        ahci_model_str, sizeof(ahci_model_str),
                        &ahci_sectors);
    if (ahci_sectors == 0) {
        return 0;
    }
    ahci_found = 1;
    return 1;
}

int ahci_present(void) {
    return ahci_found;
}

uint64_t ahci_total_sectors(void) {
    return ahci_sectors;
}

const char *ahci_model(void) {
    return ahci_model_str;
}

int ahci_read_sector(uint64_t lba, uint8_t *buf) {
    if (!ahci_found) {
        return -1;
    }
    if (ahci_run_cmd(AHCI_CMD_READ_DMA_EXT, lba, 1, ahci_data_phys, 0) !=
        0) {
        return -1;
    }
    memcpy(buf, ahci_phys_to_virt(ahci_data_phys), AHCI_SECTOR_SIZE);
    return 0;
}

int ahci_write_sector(uint64_t lba, const uint8_t *buf) {
    if (!ahci_found) {
        return -1;
    }
    memcpy(ahci_phys_to_virt(ahci_data_phys), buf, AHCI_SECTOR_SIZE);
    if (ahci_run_cmd(AHCI_CMD_WRITE_DMA_EXT, lba, 1, ahci_data_phys, 1) !=
        0) {
        return -1;
    }
    return 0;
}
