/* PrismOS — NVMe ドライバの実装。 */

#include "nvme.h"

#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"

#define NVME_REG_CAP 0x00
#define NVME_REG_VS 0x08
#define NVME_REG_CC 0x14
#define NVME_REG_CSTS 0x1C
#define NVME_REG_AQA 0x24
#define NVME_REG_ASQ 0x28
#define NVME_REG_ACQ 0x30

#define NVME_CC_EN 0x00000001u
#define NVME_CC_IOSQES 0x00060000u /* 6 << 16: 項目 64 バイト */
#define NVME_CC_IOCQES 0x00400000u /* 4 << 20: 項目 16 バイト */
#define NVME_CSTS_RDY 0x1u
#define NVME_CSTS_CFS 0x2u

#define NVME_QENTRIES 64
#define NVME_TIMEOUT 20000000

static volatile uint32_t *nvme_regs;
static uint64_t nvme_hhdm;
static int nvme_found;
static uint32_t nvme_last_status;
static uint64_t nvme_sectors;
static char nvme_model_str[41];

static uint64_t nvme_asq_phys, nvme_acq_phys;
static uint64_t nvme_iosq_phys, nvme_iocq_phys;
static uint64_t nvme_data_phys;
static uint16_t nvme_admin_tail, nvme_admin_head;
static int nvme_admin_phase;
static uint16_t nvme_io_tail, nvme_io_head;
static int nvme_io_phase;
static uint16_t nvme_cid;
static uint32_t nvme_db_stride; /* ドアベル間隔 (バイト) */

static uint32_t reg_read(uint32_t off) {
    return nvme_regs[off / 4];
}
static void reg_write(uint32_t off, uint32_t v) {
    nvme_regs[off / 4] = v;
}
static void reg_write64(uint32_t off, uint64_t v) {
    nvme_regs[off / 4] = (uint32_t)(v & 0xFFFFFFFFu);
    nvme_regs[off / 4 + 1] = (uint32_t)(v >> 32);
}
static void serial_write_hex32(uint32_t v) {
    const char *hex = "0123456789abcdef";
    char buf[9];
    for (int i = 0; i < 8; i++) {
        buf[i] = hex[(v >> (28 - 4 * i)) & 0xF];
    }
    buf[8] = '\0';
    serial_write(buf);
}

static void *phys_virt(uint64_t phys) {
    return (void *)(uintptr_t)(phys + nvme_hhdm);
}
static void zero_page(uint64_t phys) {
    uint8_t *p = (uint8_t *)phys_virt(phys);
    for (int i = 0; i < 4096; i++) {
        p[i] = 0;
    }
}

static uint32_t doorbell_off(int qid, int is_cq) {
    return 0x1000 + (uint32_t)(2 * qid + (is_cq ? 1 : 0)) *
                        nvme_db_stride;
}

/* 完了を待つ。0 = 成功、-1 = タイムアウト/エラー。 */
static int wait_completion(uint64_t cq_phys, uint16_t *head_io,
                           int *phase_io, int qid) {
    volatile uint32_t *cq =
        (volatile uint32_t *)phys_virt(cq_phys);
    for (int spin = 0; spin < NVME_TIMEOUT; spin++) {
        uint32_t dw3 = cq[*head_io * 4 + 3];
        int phase = (int)((dw3 >> 16) & 1);
        if (phase == *phase_io) {
            uint32_t status = (dw3 >> 17) & 0x7FFF;
            nvme_last_status = status;
            (*head_io)++;
            if (*head_io >= NVME_QENTRIES) {
                *head_io = 0;
                *phase_io = !*phase_io;
            }
            reg_write(doorbell_off(qid, 1), *head_io);
            return status == 0 ? 0 : -1;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

/* 64 バイトのコマンドを SQ に積んでドアベルを鳴らす。 */
static void submit(uint64_t sq_phys, uint16_t *tail_io,
                   const uint32_t *cmd, int qid) {
    volatile uint32_t *sq = (volatile uint32_t *)phys_virt(sq_phys);
    for (int i = 0; i < 16; i++) {
        sq[*tail_io * 16 + i] = cmd[i];
    }
    __asm__ volatile("mfence" ::: "memory");
    (*tail_io)++;
    if (*tail_io >= NVME_QENTRIES) {
        *tail_io = 0;
    }
    reg_write(doorbell_off(qid, 0), *tail_io);
}

static void make_cmd(uint32_t *cmd, uint8_t opcode, uint32_t nsid,
                     uint64_t prp1, uint32_t cdw10, uint32_t cdw11,
                     uint32_t cdw12) {
    for (int i = 0; i < 16; i++) {
        cmd[i] = 0;
    }
    nvme_cid++;
    cmd[0] = (uint32_t)opcode | ((uint32_t)nvme_cid << 16);
    cmd[1] = nsid;
    cmd[6] = (uint32_t)(prp1 & 0xFFFFFFFFu);
    cmd[7] = (uint32_t)(prp1 >> 32);
    cmd[10] = cdw10;
    cmd[11] = cdw11;
    cmd[12] = cdw12;
}

static int admin_cmd(uint8_t opcode, uint32_t nsid, uint64_t prp1,
                     uint32_t cdw10, uint32_t cdw11, uint32_t cdw12) {
    uint32_t cmd[16];
    make_cmd(cmd, opcode, nsid, prp1, cdw10, cdw11, cdw12);
    submit(nvme_asq_phys, &nvme_admin_tail, cmd, 0);
    return wait_completion(nvme_acq_phys, &nvme_admin_head,
                           &nvme_admin_phase, 0);
}

static int wait_ready(int want_ready) {
    for (int spin = 0; spin < NVME_TIMEOUT; spin++) {
        uint32_t csts = reg_read(NVME_REG_CSTS);
        if (csts & NVME_CSTS_CFS) {
            return -1;
        }
        if (((csts & NVME_CSTS_RDY) != 0) == (want_ready != 0)) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

int nvme_init(uint64_t hhdm_offset) {
    uint8_t bus, slot, func;
    nvme_found = 0;
    nvme_hhdm = hhdm_offset;
    if (!pci_find_class(0x01, 0x08, 0x02, &bus, &slot, &func)) {
        return 0;
    }
    uint32_t bar0 = pci_bar32_mem(bus, slot, func, 0);
    uint64_t bar_phys = bar0 & 0xFFFFFFF0u;
    if (bar_phys == 0) {
        return 0;
    }
    /* BAR が 64 ビットの場合は上位も読む。 */
    if ((bar0 & 0x6) == 0x4) {
        uint32_t hi = pci_config_read32(bus, slot, func, 0x14);
        bar_phys |= (uint64_t)hi << 32;
    }
    pci_enable_bus_master(bus, slot, func);
    if (paging_map_mmio(bar_phys, 0x4000, hhdm_offset) != 0) {
        return 0;
    }
    nvme_regs = (volatile uint32_t *)(uintptr_t)(bar_phys +
                                                 hhdm_offset);
    uint64_t cap = reg_read(NVME_REG_CAP) |
                   ((uint64_t)reg_read(NVME_REG_CAP + 4) << 32);
    nvme_db_stride = 4u << ((cap >> 32) & 0xF);

    /* コントローラを止めてから作り直す。 */
    if (reg_read(NVME_REG_CC) & NVME_CC_EN) {
        reg_write(NVME_REG_CC, 0);
        if (wait_ready(0) != 0) {
            return 0;
        }
    }
    nvme_asq_phys = pmm_alloc_page();
    nvme_acq_phys = pmm_alloc_page();
    nvme_iosq_phys = pmm_alloc_page();
    nvme_iocq_phys = pmm_alloc_page();
    nvme_data_phys = pmm_alloc_page();
    if (!nvme_asq_phys || !nvme_acq_phys || !nvme_iosq_phys ||
        !nvme_iocq_phys || !nvme_data_phys) {
        return 0;
    }
    zero_page(nvme_asq_phys);
    zero_page(nvme_acq_phys);
    zero_page(nvme_iosq_phys);
    zero_page(nvme_iocq_phys);
    zero_page(nvme_data_phys);
    nvme_admin_tail = nvme_admin_head = 0;
    nvme_admin_phase = 1;
    nvme_io_tail = nvme_io_head = 0;
    nvme_io_phase = 1;
    nvme_cid = 0;

    reg_write(NVME_REG_AQA, (NVME_QENTRIES - 1) |
                            ((NVME_QENTRIES - 1) << 16));
    reg_write64(NVME_REG_ASQ, nvme_asq_phys);
    reg_write64(NVME_REG_ACQ, nvme_acq_phys);
    reg_write(NVME_REG_CC, NVME_CC_EN | NVME_CC_IOSQES |
                           NVME_CC_IOCQES);
    if (wait_ready(1) != 0) {
        serial_write("[nvme] not ready: CC=");
        serial_write_hex32(reg_read(NVME_REG_CC));
        serial_write(" CSTS=");
        serial_write_hex32(reg_read(NVME_REG_CSTS));
        serial_write(" CAPlo=");
        serial_write_hex32(reg_read(NVME_REG_CAP));
        serial_write_line("");
        return 0;
    }

    /* Identify Controller: モデル名 (24..63 バイト目)。 */
    if (admin_cmd(0x06, 0, nvme_data_phys, 1, 0, 0) != 0) {
        serial_write_line("[nvme] identify controller failed");
        return 0;
    }
    const uint8_t *id = (const uint8_t *)phys_virt(nvme_data_phys);
    int mlen = 40;
    while (mlen > 0 && id[24 + mlen - 1] == ' ') {
        mlen--;
    }
    for (int i = 0; i < mlen; i++) {
        nvme_model_str[i] = (char)id[24 + i];
    }
    nvme_model_str[mlen] = '\0';

    /* Identify Namespace 1: NSZE と LBA サイズ。 */
    if (admin_cmd(0x06, 1, nvme_data_phys, 0, 0, 0) != 0) {
        serial_write_line("[nvme] identify namespace failed");
        return 0;
    }
    uint64_t nsze = 0;
    for (int i = 0; i < 8; i++) {
        nsze |= (uint64_t)id[i] << (8 * i);
    }
    uint8_t flbas = id[26] & 0xF;
    uint8_t ds = id[128 + (size_t)flbas * 4 + 2];
    if (ds != 9) {
        serial_write_line("[nvme] LBA size is not 512; skipped");
        return 0;
    }
    nvme_sectors = nsze;

    /* IO 完了キュー (ID 1) と IO 送信キュー (ID 1) を作る。
     * Set Features (Number of Queues) は送らない: QEMU 8.2 は
     * 要求値で conf_ioqpairs を上書きし、要求の解釈次第で
     * 作成可能なキューが 0 になってしまうため。既定の上限
     * (max_ioqpairs) の範囲でそのまま作る。 */
    if (admin_cmd(0x05, 0, nvme_iocq_phys,
                  ((NVME_QENTRIES - 1) << 16) | 1, 1, 0) != 0) {
        serial_write("[nvme] create io cq failed, status=");
        serial_write_hex32(nvme_last_status);
        serial_write_line("");
        return 0;
    }
    if (admin_cmd(0x01, 0, nvme_iosq_phys,
                  ((NVME_QENTRIES - 1) << 16) | 1, (1 << 16) | 1,
                  0) != 0) {
        serial_write_line("[nvme] create io sq failed");
        return 0;
    }
    nvme_found = 1;
    serial_write("[nvme] ready: ");
    serial_write_line(nvme_model_str);
    return 1;
}

int nvme_present(void) {
    return nvme_found;
}

uint64_t nvme_total_sectors(void) {
    return nvme_sectors;
}

const char *nvme_model(void) {
    return nvme_model_str;
}

static int nvme_rw(uint64_t lba, uint8_t *buf, int write) {
    if (!nvme_found || lba >= nvme_sectors) {
        return -1;
    }
    uint8_t *data = (uint8_t *)phys_virt(nvme_data_phys);
    if (write) {
        for (int i = 0; i < 512; i++) {
            data[i] = buf[i];
        }
    }
    uint32_t cmd[16];
    make_cmd(cmd, write ? 0x01 : 0x02, 1, nvme_data_phys,
             (uint32_t)(lba & 0xFFFFFFFFu),
             (uint32_t)(lba >> 32), 0 /* NLB = 1 セクタ */);
    submit(nvme_iosq_phys, &nvme_io_tail, cmd, 1);
    if (wait_completion(nvme_iocq_phys, &nvme_io_head,
                        &nvme_io_phase, 1) != 0) {
        return -1;
    }
    if (!write) {
        for (int i = 0; i < 512; i++) {
            buf[i] = data[i];
        }
    }
    return 0;
}

int nvme_read_sector(uint64_t lba, uint8_t *buf) {
    return nvme_rw(lba, buf, 0);
}

int nvme_write_sector(uint64_t lba, const uint8_t *buf) {
    return nvme_rw(lba, (uint8_t *)(uintptr_t)buf, 1);
}
