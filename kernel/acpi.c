/* ACPI power management. See acpi.h for the design. */

#include "acpi.h"

#include "fb.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "pic.h"
#include "vbe.h"
#include "paging.h"
#include "serial.h"
#include "string.h"

extern const uint8_t acpi_wake_blob[];
extern const uint8_t acpi_wake_blob_end[];
extern uint64_t acpi_wake_cr3_slot;
extern uint64_t acpi_wake_rsp_slot;
extern uint64_t acpi_wake_entry_slot;
extern uint8_t acpi_wake_gdt_ptr[];
extern uint8_t acpi_wake_idt_ptr[];
extern void acpi_s3_resume(void);
extern int acpi_s3_switch(uint16_t port, uint16_t value,
                          uint64_t *ctx);

#define ACPI_WAKE_PHYS 0x9000ULL

static uint64_t acpi_hhdm;
static int acpi_have_fadt;
static uint8_t *facs_ptr; /* HHDM pointer to the FACS */
static uint32_t facs_len;
static int wake_ready;
static uint16_t pm1a_cnt_port;
static int s3_typ = -1;
static int s5_typ = -1;
static int battery_present;

static int acpi_checksum(const uint8_t *p, uint64_t len) {
    uint8_t sum = 0;
    for (uint64_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* ACPI tables can live outside the HHDM identity range;
 * map before reading (same approach as smp.c). */
static const uint8_t *acpi_ptr(uint64_t phys, uint64_t len) {
    if (paging_map_mmio(phys & ~0xFFFULL,
                        len + (phys & 0xFFFULL), acpi_hhdm) != 0) {
        return NULL;
    }
    return (const uint8_t *)(uintptr_t)(phys + acpi_hhdm);
}

/* Extract the SLP_TYP (first element) of a \_Sx package
 * from the DSDT. name is the 4-byte NameSeg ("_S3_" or
 * "_S5_"). Returns -1 when not found or not a simple
 * integer package. */
static int dsdt_slp_typ(const uint8_t *dsdt, uint32_t len,
                        const char *name) {
    if (len < 40) {
        return -1;
    }
    for (uint32_t i = 36; i + 8 < len; i++) {
        if (memcmp(dsdt + i, name, 4) != 0) {
            continue;
        }
        uint32_t p = i + 4;
        if (dsdt[p] != 0x12) { /* PackageOp */
            continue;
        }
        p++;
        /* PkgLength: the top two bits count follow bytes. */
        uint8_t lead = dsdt[p];
        int follow = lead >> 6;
        p += 1 + (uint32_t)follow;
        if (p >= len) {
            return -1;
        }
        /* NumElements, then the first element. */
        p++;
        if (p >= len) {
            return -1;
        }
        if (dsdt[p] == 0x0A) { /* ByteConst */
            if (p + 1 < len) {
                return dsdt[p + 1];
            }
            return -1;
        }
        if (dsdt[p] == 0x00) {
            return 0;
        }
        if (dsdt[p] == 0x01) {
            return 1;
        }
        return -1;
    }
    return -1;
}

static int dsdt_has(const uint8_t *dsdt, uint32_t len,
                    const char *needle) {
    size_t nl = strlen(needle);
    if (len < nl) {
        return 0;
    }
    for (uint32_t i = 0; i + nl <= len; i++) {
        if (memcmp(dsdt + i, needle, nl) == 0) {
            return 1;
        }
    }
    return 0;
}

int acpi_init(uint64_t rsdp_addr, uint64_t hhdm_offset) {
    acpi_hhdm = hhdm_offset;
    acpi_have_fadt = 0;
    pm1a_cnt_port = 0;
    s3_typ = -1;
    s5_typ = -1;
    battery_present = 0;
    if (rsdp_addr == 0) {
        serial_write_line("[acpi] no RSDP; power management off");
        return -1;
    }
    uint64_t rsdp_phys = rsdp_addr >= hhdm_offset
                             ? rsdp_addr - hhdm_offset
                             : rsdp_addr;
    const uint8_t *rsdp = acpi_ptr(rsdp_phys, 36);
    if (rsdp == NULL || memcmp(rsdp, "RSD PTR ", 8) != 0 ||
        !acpi_checksum(rsdp, 20)) {
        serial_write_line("[acpi] RSDP invalid");
        return -1;
    }
    uint64_t sdt_phys;
    int xsdt = 0;
    if (rsdp[15] >= 2) {
        sdt_phys = rd64(rsdp + 24);
        xsdt = 1;
    } else {
        sdt_phys = rd32(rsdp + 16);
    }
    const uint8_t *sdt = acpi_ptr(sdt_phys, 8);
    if (sdt == NULL) {
        return -1;
    }
    uint32_t sdt_len = rd32(sdt + 4);
    sdt = acpi_ptr(sdt_phys, sdt_len);
    if (sdt == NULL || sdt_len < 36 ||
        !acpi_checksum(sdt, sdt_len)) {
        serial_write_line("[acpi] SDT invalid");
        return -1;
    }
    uint32_t entry_size = xsdt ? 8 : 4;
    uint32_t count = (sdt_len - 36) / entry_size;
    const uint8_t *fadt = NULL;
    uint32_t fadt_len = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t phys = xsdt ? rd64(sdt + 36 + i * 8)
                             : rd32(sdt + 36 + i * 4);
        const uint8_t *t = acpi_ptr(phys, 8);
        if (t == NULL || memcmp(t, "FACP", 4) != 0) {
            continue;
        }
        fadt_len = rd32(t + 4);
        fadt = acpi_ptr(phys, fadt_len);
        break;
    }
    if (fadt == NULL || fadt_len < 100 ||
        !acpi_checksum(fadt, fadt_len)) {
        serial_write_line("[acpi] no FADT; power management off");
        return -1;
    }
    acpi_have_fadt = 1;
    pm1a_cnt_port = (uint16_t)rd32(fadt + 64);
    uint64_t dsdt_phys = rd32(fadt + 40);
    if (dsdt_phys == 0 && fadt_len >= 148) {
        dsdt_phys = rd64(fadt + 140);
    }
    if (dsdt_phys != 0) {
        const uint8_t *dsdt = acpi_ptr(dsdt_phys, 8);
        if (dsdt != NULL && memcmp(dsdt, "DSDT", 4) == 0) {
            uint32_t dlen = rd32(dsdt + 4);
            dsdt = acpi_ptr(dsdt_phys, dlen);
            if (dsdt != NULL && acpi_checksum(dsdt, dlen)) {
                s3_typ = dsdt_slp_typ(dsdt, dlen, "_S3_");
                s5_typ = dsdt_slp_typ(dsdt, dlen, "_S5_");
                battery_present = dsdt_has(dsdt, dlen, "PNP0C0A");
            }
        }
    }
    /* FACS: the firmware waking vector is where the
     * firmware jumps on S3 resume. Point it at the wake
     * stub copied to physical 0x9000. */
    facs_ptr = NULL;
    facs_len = 0;
    wake_ready = 0;
    {
        uint64_t facs_phys = rd32(fadt + 36);
        if (facs_phys == 0 && fadt_len >= 140) {
            facs_phys = rd64(fadt + 132);
        }
        const uint8_t *facs = NULL;
        if (facs_phys != 0) {
            facs = acpi_ptr(facs_phys, 8);
        }
        if (facs != NULL && memcmp(facs, "FACS", 4) == 0) {
            facs_len = rd32(facs + 4);
            facs = acpi_ptr(facs_phys, facs_len < 64 ? facs_len : 64);
            facs_ptr = (uint8_t *)facs;
        }
        if (facs_ptr != NULL &&
            paging_map_page_identity(ACPI_WAKE_PHYS,
                                     acpi_hhdm) == 0) {
            uint8_t *blob = (uint8_t *)(uintptr_t)(
                ACPI_WAKE_PHYS + acpi_hhdm);
            size_t blob_size =
                (size_t)(acpi_wake_blob_end - acpi_wake_blob);
            memcpy(blob, acpi_wake_blob, blob_size);
            uint64_t *entry_slot = (uint64_t *)(blob +
                (uint64_t)((const uint8_t *)&acpi_wake_entry_slot -
                           acpi_wake_blob));
            *entry_slot = (uint64_t)(uintptr_t)acpi_s3_resume;
            struct desc_ptr {
                uint16_t limit;
                uint64_t base;
            } __attribute__((packed));
            struct desc_ptr dp;
            __asm__ volatile("sgdt %0" : "=m"(dp));
            uint8_t *gp = blob +
                (uint64_t)(acpi_wake_gdt_ptr - acpi_wake_blob);
            gp[0] = (uint8_t)(dp.limit & 0xFF);
            gp[1] = (uint8_t)(dp.limit >> 8);
            for (int i = 0; i < 8; i++) {
                gp[2 + i] = (uint8_t)(dp.base >> (8 * i));
            }
            __asm__ volatile("sidt %0" : "=m"(dp));
            uint8_t *ip = blob +
                (uint64_t)(acpi_wake_idt_ptr - acpi_wake_blob);
            ip[0] = (uint8_t)(dp.limit & 0xFF);
            ip[1] = (uint8_t)(dp.limit >> 8);
            for (int i = 0; i < 8; i++) {
                ip[2 + i] = (uint8_t)(dp.base >> (8 * i));
            }
            /* Firmware waking vector (32-bit) and the
             * 64-bit X_ vector when the FACS is long
             * enough. */
            facs_ptr[12] = (uint8_t)(ACPI_WAKE_PHYS & 0xFF);
            facs_ptr[13] = (uint8_t)((ACPI_WAKE_PHYS >> 8) & 0xFF);
            facs_ptr[14] = (uint8_t)((ACPI_WAKE_PHYS >> 16) & 0xFF);
            facs_ptr[15] = (uint8_t)((ACPI_WAKE_PHYS >> 24) & 0xFF);
            if (facs_len >= 32) {
                for (int i = 0; i < 8; i++) {
                    facs_ptr[24 + i] =
                        (uint8_t)(ACPI_WAKE_PHYS >> (8 * i));
                }
            }
            wake_ready = 1;
        }
    }
    serial_write("[acpi] FADT pm1a_cnt=0x");
    {
        char hex[8];
        int n = 0;
        uint32_t v = pm1a_cnt_port;
        char tmp[8];
        int ti = 0;
        if (v == 0) {
            tmp[ti++] = '0';
        }
        while (v > 0) {
            int d = (int)(v & 0xF);
            tmp[ti++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            v >>= 4;
        }
        while (ti > 0) {
            hex[n++] = tmp[--ti];
        }
        hex[n] = '\0';
        serial_write(hex);
    }
    serial_write(" s3_typ=");
    serial_write(s3_typ >= 0 ? "yes" : "no");
    serial_write(" s5_typ=");
    serial_write(s5_typ >= 0 ? "yes" : "no");
    serial_write(" battery=");
    serial_write_line(battery_present ? "present" : "none");
    return 0;
}

int acpi_s3_available(void) {
    return acpi_have_fadt && pm1a_cnt_port != 0 && s3_typ >= 0 &&
           wake_ready;
}

int acpi_s3_slp_typ(void) {
    return s3_typ;
}

int acpi_s5_slp_typ(void) {
    return s5_typ;
}

uint16_t acpi_pm1_cnt_port(void) {
    return pm1a_cnt_port;
}

int acpi_battery_present(void) {
    return battery_present;
}

int acpi_enter_s3(void) {
    if (!acpi_s3_available()) {
        return -1;
    }
    /* PCI 設定 (BAR/コマンド) は復帰時にプラットフォームが
     * 初期状態へ戻すため、入る前に全デバイス分を保存する。 */
    pci_save_all();
    /* The context slots live inside the wake blob at
     * physical 0x9000; the switch writes CR3/RSP there and
     * the wake stub reads them back on resume. */
    uint64_t *ctx = (uint64_t *)(uintptr_t)(
        ACPI_WAKE_PHYS + acpi_hhdm +
        (uint64_t)((const uint8_t *)&acpi_wake_cr3_slot -
                   acpi_wake_blob));
    int rc = acpi_s3_switch(
        pm1a_cnt_port,
        (uint16_t)(((uint16_t)s3_typ << 10) | (1u << 13)), ctx);
    if (rc == 1) {
        /* Resumed: the wake stub came back with interrupts
         * off; the pre-sleep context had them on. */
        /* S3 復帰でファームウェアは 8259 PIC を既定状態
         * (ベクタ基点 0x08/0x70、マスク全閉じ側) に戻す
         * ため、起動時と同じリマップとマスクをやり直す。
         * これをしないと IRQ12 (マウス) がベクタ 44 に
         * 届かなくなる。 */
        pic_init();
        /* PCI 設定を書き戻す (VGA の BAR とメモリデコード、
         * NIC やストレージの BAR も含む)。これがないと復帰
         * 後にフレームバッファへの書き込みがどこにも届かず
         * 画面が凍り、解像度変更も失敗する。 */
        pci_restore_all();
        /* VBE モードも入れ直す (DISPI 側の状態が戻って
         * いても安全な冪等操作)。 */
        if (vbe_present()) {
            (void)vbe_set_mode(fb_width(), fb_height());
        }
        /* キーボードも初期化し直す。復帰後の i8042 は変換
         * (set 2 -> set 1) が切れており、キーが別の文字に
         * 化ける (e が j、F1 が '4' 等)。設定バイトの変換
         * ビットと走査有効化を起動時と同じに戻す。 */
        keyboard_init();
        /* PS/2 マウスも初期化し直す。順序が重要: 割り込みを
         * 有効化した後だと、マウスの ACK バイトを IRQ12
         * ハンドラがパケットとして横取りし、ポーリングで
         * 待つ mouse_init が応答なしと誤判定する (復帰後に
         * マウスが無応答になる既知の問題の実体)。起動時と
         * 同じく、割り込みオフのまま初期化してから sti
         * する。 */
        mouse_init();
        prism_sti();
    }
    return 0;
}

int acpi_poweroff(void) {
    if (acpi_have_fadt && pm1a_cnt_port != 0 && s5_typ >= 0) {
        prism_outw(pm1a_cnt_port,
                   (uint16_t)(((uint16_t)s5_typ << 10) |
                              (1u << 13)));
        for (volatile int i = 0; i < 1000000; i++) {
            __asm__ volatile("pause");
        }
    }
    /* Fallback for QEMU/Bochs-style test rigs without a
     * usable S5: the debug shutdown port. */
    prism_outw(0x604, 0x2000);
    return -1;
}
