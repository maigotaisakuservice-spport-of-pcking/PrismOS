/* PrismOS — ARM 用 ramfb フレームバッファ (QEMU virt の fw_cfg 経由)
 *
 * 原因と修正 (2026-11-21、仕様と実例を読んで特定):
 *   以前の版は設定を fw_cfg のデータレジスタへ 32 ビット単位で
 *   書いていたが、QEMU の fw_cfg_write() は QEMU 2.4 以降は
 *   空関数で、データレジスタへの書き込みはすべて捨てられる
 *   (hw/nvram/fw_cfg.c)。データレジスタは読む専用。
 *   設定を書く唯一の方法は fw_cfg DMA である。OSDev の Ramfb
 *   と動く実例 (arm-v8a-virt-ramfb) と同じ手順で書く。
 *   なお設定は 28 バイト (addr 8 / fourcc 4 / flags 4 /
 *   width 4 / height 4 / stride 4、すべてビッグエンディアン)。
 *   以前のヘッダにあった「56 バイト・予約 28 バイト」は誤記。
 *   QEMU 側の RAMFBCfg (hw/display/ramfb.c) は 28 バイトで、
 *   fw_cfg の "etc/ramfb" ファイルも 28 バイトである。
 *   fourcc は XR24 (XRGB8888 = 0x34325258)。
 *   書き込み後は DMA で読み返して設定が QEMU に届いたことを
 *   確かめる。 */
#include "ramfb.h"

#include "pmm.h"
#include "string.h"
#include "uart.h"

/* fw_cfg の MMIO (QEMU virt、0x09020000)。
 * 並びは IO 版と逆で、データが先、選択子が後:
 *   データ (0x00、8 バイト、64 ビットで読む)
 *   選択子 (0x08、2 バイト、16 ビットで書く)
 *   DMA (0x10、8 バイト、64 ビットで書く)
 * データはビッグエンディアン: 64 ビット値の最上位バイトが
 * ファイルの先頭バイト。選択子もビッグエンディアン。 */
#define FWCFG_BASE ((volatile uint8_t *)0x09020000u)
#define FWCFG_DATA_OFF 0x00
#define FWCFG_SELECTOR_OFF 0x08
#define FWCFG_DMA_OFF 0x10
#define FWCFG_FILE_DIR 0x19 /* ファイル一覧の選択子 */

/* fw_cfg DMA の制御ビット (QEMU の fw_cfg 仕様)。 */
#define FWCFG_DMA_CTL_ERROR 0x01u
#define FWCFG_DMA_CTL_READ 0x02u
#define FWCFG_DMA_CTL_SELECT 0x08u
#define FWCFG_DMA_CTL_WRITE 0x10u

#define RAMFB_WIDTH 1024u
#define RAMFB_HEIGHT 768u
#define RAMFB_BPP 4u /* XR24 は 1 ピクセル 4 バイト */
#define RAMFB_CFG_LEN 28u

static uint32_t be32(uint32_t v) {
    return __builtin_bswap32(v);
}

static uint64_t be64(uint64_t v) {
    return __builtin_bswap64(v);
}

/* D キャッシュを PoC まで書き戻す (実機の DMA 用。QEMU では
 * キャッシュが無いので何もしない)。 */
static void dcache_clean_range(uint64_t addr, uint64_t len) {
    uint64_t start = addr & ~63ULL;
    uint64_t end = (addr + len + 63) & ~63ULL;
    for (uint64_t a = start; a < end; a += 64) {
        __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static volatile uint64_t *fwcfg_data_reg(void) {
    return (volatile uint64_t *)(FWCFG_BASE + FWCFG_DATA_OFF);
}

static volatile uint16_t *fwcfg_selector_reg(void) {
    return (volatile uint16_t *)(FWCFG_BASE + FWCFG_SELECTOR_OFF);
}

static volatile uint64_t *fwcfg_dma_reg(void) {
    return (volatile uint64_t *)(FWCFG_BASE + FWCFG_DMA_OFF);
}

static void fwcfg_select(uint16_t sel) {
    /* 選択子レジスタはビッグエンディアン。CPU はリトル
     * エンディアンなので、バイトを入れ替えて書く。 */
    *fwcfg_selector_reg() = (uint16_t)(((sel & 0xFFu) << 8) |
                                       ((sel >> 8) & 0xFFu));
}

static uint64_t fwcfg_read64(void) {
    return *fwcfg_data_reg();
}

/* 8 バイトずつ読んで 1 バイトずつ渡す読み手。 */
static uint8_t fwcfg_read_buf[8];
static int fwcfg_read_pos;
static int fwcfg_read_len;

static void fwcfg_read_reset(void) {
    fwcfg_read_pos = 0;
    fwcfg_read_len = 0;
}

static uint8_t fwcfg_read_byte(void) {
    if (fwcfg_read_pos >= fwcfg_read_len) {
        uint64_t v = fwcfg_read64();
        /* ビッグエンディアンの機器をリトルエンディアンの
         * CPU が読むとバイトが逆になるため、リトルエンディアン
         * 順 (先頭バイトが最下位) で取り出す。 */
        for (int i = 0; i < 8; i++) {
            fwcfg_read_buf[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
        }
        fwcfg_read_pos = 0;
        fwcfg_read_len = 8;
    }
    return fwcfg_read_buf[fwcfg_read_pos++];
}

/* ビッグエンディアンで読む。 */
static uint32_t fwcfg_read_be32(void) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) | fwcfg_read_byte();
    }
    return v;
}

static uint16_t fwcfg_read_be16(void) {
    uint16_t v = 0;
    for (int i = 0; i < 2; i++) {
        v = (uint16_t)((v << 8) | fwcfg_read_byte());
    }
    return v;
}

/* ファイル一覧から "etc/ramfb" の選択子を探す。
 * 見つかれば選択子、無ければ 0。 */
static uint16_t ramfb_find_selector(void) {
    fwcfg_select(FWCFG_FILE_DIR);
    fwcfg_read_reset();
    uint32_t count = fwcfg_read_be32();
    if (count == 0 || count > 512) {
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = fwcfg_read_be32();
        uint16_t sel = fwcfg_read_be16();
        (void)fwcfg_read_be16(); /* 予約 */
        char name[56];
        for (int j = 0; j < 56; j++) {
            name[j] = (char)fwcfg_read_byte();
        }
        (void)size;
        if (strcmp(name, "etc/ramfb") == 0) {
            return sel;
        }
    }
    return 0;
}

static void write_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

static void write_be32(uint8_t *p, uint32_t v) {
    for (int i = 3; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

/* fw_cfg DMA の転送記述子 (QEMU の FWCfgDmaAccess)。
 * すべてビッグエンディアンで置く。 */
struct fwcfg_dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
} __attribute__((packed));

static void uart_write_hex32(uint32_t v) {
    static const char hexd[] = "0123456789abcdef";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hexd[(v >> (28 - i * 4)) & 0xF];
    }
    buf[10] = '\0';
    arm_uart_write(buf);
}

static void uart_write_hex64(uint64_t v) {
    static const char hexd[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hexd[(v >> (60 - i * 4)) & 0xF];
    }
    buf[18] = '\0';
    arm_uart_write(buf);
}

/* DMA 転送を 1 回行う。op は READ (0x02) か WRITE (0x10)。
 * control には (選択子 << 16) | SELECT | op を置く。
 * 成功したら 0、失敗したら -1 を返す。 */
static int fwcfg_dma(uint16_t sel, uint32_t op, void *buf, uint32_t len) {
    /* QEMU が control に完了を書き戻すため、volatile が要る。
     * volatile が無いとコンパイラが読み取りをループの外に
     * 持ち出し、完了を一生読まない (2026-11-21 に実際に起きた)。 */
    static volatile struct fwcfg_dma_access dma;
    dma.control = be32(((uint32_t)sel << 16) | FWCFG_DMA_CTL_SELECT | op);
    dma.length = be32(len);
    dma.address = be64((uint64_t)(uintptr_t)buf);
    /* 記述子と転送する内容を RAM へ出してから DMA を始める。 */
    dcache_clean_range((uint64_t)(uintptr_t)&dma, sizeof(dma));
    dcache_clean_range((uint64_t)(uintptr_t)buf, len);
    __asm__ volatile("dmb sy" ::: "memory");
    /* DMA レジスタに記述子のアドレスをビッグエンディアンで書く。
     * QEMU はこの書き込みの中で同期的に転送し、終わったら
     * control を 0 (成功) か 1 (失敗) に書き戻す。 */
    *fwcfg_dma_reg() = be64((uint64_t)(uintptr_t)&dma);
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint32_t c = be32(dma.control);
        if ((c & ~FWCFG_DMA_CTL_ERROR) == 0) {
            return (c & FWCFG_DMA_CTL_ERROR) ? -1 : 0;
        }
    }
    arm_uart_write("[boot] ramfb: DMA no reply, control=");
    uart_write_hex32(be32(dma.control));
    arm_uart_write(" dma_at=");
    uart_write_hex64((uint64_t)(uintptr_t)&dma);
    arm_uart_write_line("");
    return -1; /* 応答が無い */
}

uint64_t arm_ramfb_init(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return 0;
    }
    uint16_t sel = ramfb_find_selector();
    if (sel == 0) {
        return 0; /* ramfb が無い */
    }
    uint64_t bytes = (uint64_t)width * height * RAMFB_BPP;
    uint64_t pages = (bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t fb_phys = pmm_alloc_pages((size_t)pages);
    if (fb_phys == 0) {
        return 0;
    }
    memset((void *)(uintptr_t)fb_phys, 0, (size_t)(pages * PMM_PAGE_SIZE));

    /* 28 バイトの設定を作る。QEMU の ramfb は設定を
     * ビッグエンディアンで読む (fw_cfg の慣習)。 */
    static uint8_t cfg[RAMFB_CFG_LEN];
    for (int i = 0; i < (int)RAMFB_CFG_LEN; i++) {
        cfg[i] = 0;
    }
    write_be64(cfg + 0, fb_phys);
    write_be32(cfg + 8, 0x34325258u); /* "XR24" (XRGB8888) */
    write_be32(cfg + 12, 0);         /* flags */
    write_be32(cfg + 16, width);
    write_be32(cfg + 20, height);
    write_be32(cfg + 24, width * RAMFB_BPP);
    /* データレジスタへの書き込みは QEMU 2.4 以降は捨てられる
     * ため、DMA で書く。 */
    if (fwcfg_dma(sel, FWCFG_DMA_CTL_WRITE, cfg, RAMFB_CFG_LEN) != 0) {
        arm_uart_write_line("[boot] ramfb: DMA write failed");
        return 0;
    }
    /* 読み返して設定が QEMU に届いたことを確かめる。 */
    static uint8_t back[RAMFB_CFG_LEN];
    for (int i = 0; i < (int)RAMFB_CFG_LEN; i++) {
        back[i] = 0;
    }
    if (fwcfg_dma(sel, FWCFG_DMA_CTL_READ, back, RAMFB_CFG_LEN) != 0) {
        arm_uart_write_line("[boot] ramfb: DMA read-back failed");
        return 0;
    }
    for (int i = 0; i < (int)RAMFB_CFG_LEN; i++) {
        if (back[i] != cfg[i]) {
            arm_uart_write_line("[boot] ramfb: read-back mismatch");
            return 0;
        }
    }
    (void)RAMFB_WIDTH;
    (void)RAMFB_HEIGHT;
    return fb_phys;
}
