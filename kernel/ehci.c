/* PrismOS — EHCI USB 2.0 ホストコントローラ */
#include "ehci.h"

#include "io.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "speaker.h"
#include "string.h"
#include "uhci.h" /* uhci_build_setup / uhci_build_cbw を再利用 */

/* 能力レジスタ (BAR0 からのずれ)。 */
#define EHCI_CAPLENGTH 0x00
#define EHCI_HCSPARAMS 0x04
#define EHCI_HCCPARAMS 0x08

/* 運用レジスタ (能力長からのずれ)。 */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_USBINTR 0x08
#define OP_PERIODICLISTBASE 0x14
#define OP_ASYNCLISTADDR 0x18
#define OP_CONFIGFLAG 0x40
#define OP_PORTSC 0x44

#define USBCMD_RS (1u << 0)      /* 実行/停止 */
#define USBCMD_HCRESET (1u << 1) /* ホストコントローラリセット */
#define USBCMD_IAAD (1u << 6)    /* 非同期前進ドアベル */
#define USBCMD_ASE (1u << 5)     /* 非同期スケジュール有効 */

#define USBSTS_HCHALTED (1u << 12)

#define PORTSC_CCS (1u << 0)  /* 現在の接続状態 */
#define PORTSC_CSC (1u << 1)  /* 接続状態の変化 (書くと消去) */
#define PORTSC_PE (1u << 2)   /* ポート有効 */
#define PORTSC_PEC (1u << 3)  /* 有効状態の変化 (書くと消去) */
#define PORTSC_OCC (1u << 5)  /* 過電流変化 (書くと消去) */
#define PORTSC_PR (1u << 8)   /* ポートリセット */
#define PORTSC_CHANGES (PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)

/* リンクポインタの種別 (QH) と終了ビット。 */
#define LINK_T (1u << 0)
#define LINK_QH (1u << 1)

/* qTD トークンの状態ビット。 */
#define QTD_ACTIVE (1u << 7)
#define QTD_ERRORS 0x7Cu /* 欠落/転送誤り/バブル/バッファ誤り/停止 */

/* DMA 構造の配置 (構造ページ内)。 */
#define EHCI_QTD_COUNT 16
#define OFF_HEAD_QH 0
#define OFF_XFER_QH 64
#define OFF_QTD0 128 /* qTD は 64 バイト枠で並べる */

/* バッファページ内の配置 (すべて 1 ページ内に収める)。 */
#define BOFF_SETUP 0     /* 8 バイト */
#define BOFF_DATA 64     /* 制御データ、最大 1024 バイト */
#define BOFF_CBW 2048    /* 31 バイト */
#define BOFF_CSW 2176    /* 13 バイト */
#define BOFF_SECTOR 2560 /* 512 バイト */
#define CTRL_DATA_MAX 1024

static volatile uint32_t *ehci_cap; /* 能力レジスタ (MMIO) */
static volatile uint32_t *ehci_op;  /* 運用レジスタ (MMIO) */
static uint64_t ehci_hhdm;
static int ehci_found;
static int ehci_n_ports;
static ehci_device ehci_devices[EHCI_MAX_DEVICES];
static int ehci_dev_count;
static int ehci_storage_idx = -1;
static uint32_t ehci_cbw_tag = 1;

/* DMA ページ (物理)。 */
static uint64_t ehci_struct_phys;
static uint64_t ehci_buf_phys;

/* 機器ごとのバルクトグル。 */
static int ehci_tgl_in[EHCI_MAX_DEVICES];
static int ehci_tgl_out[EHCI_MAX_DEVICES];

static void ehci_dec(uint64_t v);
static void print_hex16(uint16_t v);

/* ---- 純粋関数 ---- */

uint32_t ehci_qtd_token(int pid, uint32_t len, int toggle, int ioc) {
    uint32_t t = QTD_ACTIVE;
    t |= (uint32_t)(pid & 3) << 8;   /* PID */
    t |= 3u << 10;                   /* 許容誤り回数 (Cerr) */
    if (ioc) {
        t |= (1u << 15);
    }
    t |= (len & 0x7FFFu) << 16;      /* 転送バイト数 */
    if (toggle) {
        t |= (1u << 31);
    }
    return t;
}

uint32_t ehci_qh_epchar(uint8_t addr, uint8_t ep, int speed,
                        uint16_t max_packet, int head, int control) {
    uint32_t v = (uint32_t)(addr & 0x7F);
    v |= (uint32_t)(ep & 0x0F) << 8;
    v |= (uint32_t)(speed & 3) << 12; /* 0=全速 1=低速 2=高速 */
    v |= (1u << 14);                  /* DTC: トグルは qTD から */
    if (head) {
        v |= (1u << 15);
    }
    v |= (uint32_t)max_packet << 16;
    if (control) {
        v |= (1u << 27);
    }
    return v;
}

int ehci_parse_config(const uint8_t *cfg, size_t len,
                      ehci_device *info) {
    size_t total = len;
    if (len >= 4) {
        size_t want = (size_t)(cfg[2] | (cfg[3] << 8));
        if (want > 0 && want < total) {
            total = want;
        }
    }
    int in_storage_if = 0;
    size_t off = 0;
    while (off + 2 <= total) {
        uint8_t dlen = cfg[off];
        uint8_t dtype = cfg[off + 1];
        if (dlen < 2) {
            break;
        }
        if (dtype == 4 && dlen >= 9) { /* 接続面 */
            uint8_t cls = cfg[off + 5];
            uint8_t sub = cfg[off + 6];
            uint8_t proto = cfg[off + 7];
            if (info->if_class == 0) {
                info->if_class = cls;
                info->if_num = cfg[off + 2];
            }
            in_storage_if =
                (cls == 8 && sub == 6 && proto == 0x50);
            if (in_storage_if) {
                info->if_num = cfg[off + 2];
            }
        } else if (dtype == 5 && dlen >= 7 && in_storage_if) {
            /* 終端: バルクのみ拾う。 */
            uint8_t eaddr = cfg[off + 2];
            uint8_t attr = cfg[off + 3];
            uint16_t mp = (uint16_t)(cfg[off + 4] |
                                     (cfg[off + 5] << 8));
            if ((attr & 3) == 2) {
                if (eaddr & 0x80) {
                    info->bulk_in_ep = (uint8_t)(eaddr & 0x0F);
                } else {
                    info->bulk_out_ep = (uint8_t)(eaddr & 0x0F);
                }
                if (mp > 0) {
                    info->bulk_max_packet = mp;
                }
            }
        }
        off += dlen;
    }
    return (info->bulk_in_ep != 0 && info->bulk_out_ep != 0) ? 0 : -1;
}

/* ---- MMIO / DMA アクセス ---- */

static uint32_t op_read(uint32_t off) { return ehci_op[off / 4]; }

static void op_write(uint32_t off, uint32_t v) { ehci_op[off / 4] = v; }

static volatile uint32_t *dma_ptr(uint64_t phys) {
    return (volatile uint32_t *)(uintptr_t)(phys + ehci_hhdm);
}

static uint8_t *buf_at(uint32_t off) {
    return (uint8_t *)(uintptr_t)(ehci_buf_phys + ehci_hhdm) + off;
}

static uint64_t buf_phys_at(uint32_t off) {
    return ehci_buf_phys + off;
}

/* PIT カウンタで最大 chunks 回 (1 回 ≒ 50ms) 待つ。
 * cond が真になったら 1 を返す。 */
static int wait_chunks(int (*cond)(void *), void *ctx, int chunks) {
    for (int c = 0; c < chunks; c++) {
        uint16_t start = speaker_pit0_count();
        uint32_t waited = 0;
        while (waited < 60000) {
            if (cond(ctx)) {
                return 1;
            }
            uint16_t now = speaker_pit0_count();
            waited = (uint32_t)(uint16_t)(start - now);
            __asm__ volatile("pause");
        }
    }
    return cond(ctx);
}

struct token_wait {
    volatile uint32_t *token;
};

static int token_done(void *p) {
    struct token_wait *w = (struct token_wait *)p;
    /* 非同期前進の完了表示 (IAA) が立っていたら消す。
     * これをしないとコントローラが前進を止める実装がある。 */
    if (op_read(OP_USBSTS) & (1u << 5)) {
        op_write(OP_USBSTS, (1u << 5));
    }
    return (*w->token & QTD_ACTIVE) == 0;
}

static void ehci_dump_chain(int n_qtd, const char *what);

/* qTD チェーンを実行し、最後の qTD の完了を待つ。
 * 成功したら 0。転送 QH は初期化時に先頭 QH の後ろへ
 * 常時リンクしておき、ここではオーバーレイだけを
 * 差し替える (リンクの抜き差しを繰り返すと、進捗管理の
 * 状態機械が止まる実装があるため)。 */
static int ehci_run_chain(int n_qtd) {
    volatile uint32_t *qh = dma_ptr(ehci_struct_phys + OFF_XFER_QH);
    /* 実行 QH のオーバーレイに先頭 qTD を差す。 */
    qh[3] = 0; /* 現在 qTD ポインタ */
    qh[4] = (uint32_t)(ehci_struct_phys + OFF_QTD0); /* 次 qTD */
    qh[5] = LINK_T;                                  /* 代替次 */
    qh[6] = 0;                                       /* トークン */
    for (int i = 0; i < 5; i++) {
        qh[7 + i] = 0;
    }
    /* ドアベルを鳴らす。 */
    op_write(OP_USBCMD, op_read(OP_USBCMD) | USBCMD_IAAD);
    volatile uint32_t *last =
        dma_ptr(ehci_struct_phys + OFF_QTD0 +
                (uint64_t)(n_qtd - 1) * 64 + 8);
    struct token_wait w = {last};
    int ok = wait_chunks(token_done, &w, 40); /* 最大 ≒ 2 秒 */
    if (!ok) {
        ehci_dump_chain(n_qtd, "timeout");
        return -1;
    }
    /* 全 qTD の誤りビットを見る。 */
    for (int i = 0; i < n_qtd; i++) {
        volatile uint32_t *t =
            dma_ptr(ehci_struct_phys + OFF_QTD0 + (uint64_t)i * 64);
        if (t[2] & (QTD_ACTIVE | QTD_ERRORS)) {
            ehci_dump_chain(n_qtd, "error bits");
            return -1;
        }
    }
    return 0;
}

/* 失敗時の診断: チェーン内 qTD のトークンをシリアルへ。 */
static void ehci_dump_chain(int n_qtd, const char *what) {
    serial_write("[ehci] xfer failed: ");
    serial_write(what);
    for (int i = 0; i < n_qtd; i++) {
        volatile uint32_t *t =
            dma_ptr(ehci_struct_phys + OFF_QTD0 + (uint64_t)i * 64);
        serial_write(" qtd");
        ehci_dec((uint64_t)i);
        serial_write("=");
        print_hex16((uint16_t)(t[2] >> 16));
        print_hex16((uint16_t)(t[2] & 0xFFFF));
    }
    serial_write_line("");
}

/* qTD を 1 個組み立てる。buf_phys はデータの物理アドレス
 * (ページ境界でなくてよい)。 */
static void make_qtd(int idx, uint32_t next_phys, int pid,
                     uint64_t buf_phys, uint32_t len, int toggle,
                     int ioc) {
    volatile uint32_t *t =
        dma_ptr(ehci_struct_phys + OFF_QTD0 + (uint64_t)idx * 64);
    t[0] = next_phys;
    t[1] = LINK_T; /* 代替次は使わない */
    t[2] = ehci_qtd_token(pid, len, toggle, ioc);
    uint32_t base = (uint32_t)(buf_phys & 0xFFFFFFFFu);
    t[3] = base;
    uint32_t page = base & ~0xFFFu;
    for (int i = 1; i < 5; i++) {
        page += 0x1000u;
        t[3 + i] = page;
    }
}

/* 制御転送。UHCI 版と同じ引数並び。成功 0。 */
static int ehci_control(ehci_device *dev, const uint8_t *setup,
                        uint8_t *data, uint16_t data_len, int data_in) {
    uint8_t *sbuf = buf_at(BOFF_SETUP);
    for (int i = 0; i < 8; i++) {
        sbuf[i] = setup[i];
    }
    if (data_len > CTRL_DATA_MAX) {
        return -1;
    }
    if (data_len > 0 && !data_in) {
        uint8_t *dbuf = buf_at(BOFF_DATA);
        for (int i = 0; i < data_len; i++) {
            dbuf[i] = data[i];
        }
    }
    /* 実行 QH の接続点特性 (終端 0)。 */
    volatile uint32_t *qh = dma_ptr(ehci_struct_phys + OFF_XFER_QH);
    qh[1] = ehci_qh_epchar(dev->address, 0, dev->speed,
                           dev->max_packet, 0,
                           dev->speed != 2);
    qh[2] = (1u << 30); /* Mult=1 */
    int n = 0;
    uint32_t qtd_phys = (uint32_t)ehci_struct_phys + OFF_QTD0;
    /* 設定段階 (トグル 0)。 */
    make_qtd(n, qtd_phys + 64, 2, buf_phys_at(BOFF_SETUP), 8, 0, 0);
    n++;
    if (data_len > 0) {
        make_qtd(n, qtd_phys + (uint32_t)(n + 1) * 64,
                 data_in ? 1 : 0, buf_phys_at(BOFF_DATA), data_len,
                 1, 0);
        n++;
    }
    /* 状態段階 (方向は逆、トグル 1、長さ 0)。 */
    make_qtd(n, LINK_T, data_in ? 0 : 1, 0, 0, 1, 1);
    n++;
    if (ehci_run_chain(n) != 0) {
        return -1;
    }
    if (data_len > 0 && data_in) {
        /* 実際に届いた長さを確認する。 */
        volatile uint32_t *t =
            dma_ptr(ehci_struct_phys + OFF_QTD0 + 64);
        uint32_t remain = (t[2] >> 16) & 0x7FFF;
        if (remain > 0 && data_len - (int)remain < 8) {
            /* 短すぎる応答は失敗扱い (記述子の先頭読み等)。 */
            if (data_len > 8) {
                return -1;
            }
        }
        uint8_t *dbuf = buf_at(BOFF_DATA);
        uint32_t got = data_len - remain;
        for (uint32_t i = 0; i < got && i < data_len; i++) {
            data[i] = dbuf[i];
        }
    }
    return 0;
}

/* バルク転送 (1 回最大 1 ページ分)。成功 0。 */
static int ehci_bulk(ehci_device *dev, int dev_idx, uint8_t ep,
                     uint64_t data_phys, uint32_t len, int in) {
    volatile uint32_t *qh = dma_ptr(ehci_struct_phys + OFF_XFER_QH);
    qh[1] = ehci_qh_epchar(dev->address, ep, dev->speed,
                           dev->bulk_max_packet, 0, 0);
    qh[2] = (1u << 30);
    int toggle = in ? ehci_tgl_in[dev_idx] : ehci_tgl_out[dev_idx];
    make_qtd(0, LINK_T, in ? 1 : 0, data_phys, len, toggle, 1);
    if (ehci_run_chain(1) != 0) {
        return -1;
    }
    if (in) {
        ehci_tgl_in[dev_idx] = toggle ^ 1;
    } else {
        ehci_tgl_out[dev_idx] = toggle ^ 1;
    }
    return 0;
}

/* 終端の停止を解除する (バルク回復用)。 */
static int ehci_clear_halt(ehci_device *dev, uint8_t ep, int in) {
    uint8_t setup[8];
    uhci_build_setup(setup, 0x02, 0x01, 0,
                     (uint16_t)(ep | (in ? 0x80 : 0)), 0);
    return ehci_control(dev, setup, NULL, 0, 0);
}

/* Bulk-Only Transport で SCSI コマンドを 1 回実行する。 */
static int ehci_bot(ehci_device *dev, int dev_idx,
                    const uint8_t *cdb, size_t cdb_len,
                    uint64_t data_phys, uint32_t data_len, int data_in) {
    uint8_t *cbw = buf_at(BOFF_CBW);
    uhci_build_cbw(cbw, ehci_cbw_tag++, data_len, data_in, cdb,
                   cdb_len);
    if (ehci_bulk(dev, dev_idx, dev->bulk_out_ep,
                  buf_phys_at(BOFF_CBW), 31, 0) != 0) {
        return -1;
    }
    if (data_len > 0) {
        if (ehci_bulk(dev, dev_idx,
                      data_in ? dev->bulk_in_ep : dev->bulk_out_ep,
                      data_phys, data_len, data_in) != 0) {
            /* 停止したら解除して状態包だけ読む。 */
            (void)ehci_clear_halt(
                dev, data_in ? dev->bulk_in_ep : dev->bulk_out_ep,
                data_in);
            if (data_in) {
                ehci_tgl_in[dev_idx] = 0;
            } else {
                ehci_tgl_out[dev_idx] = 0;
            }
        }
    }
    uint8_t *csw = buf_at(BOFF_CSW);
    for (int i = 0; i < 13; i++) {
        csw[i] = 0;
    }
    if (ehci_bulk(dev, dev_idx, dev->bulk_in_ep,
                  buf_phys_at(BOFF_CSW), 13, 1) != 0) {
        return -1;
    }
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 ||
        csw[3] != 0x53) {
        return -1; /* 署名 "USBS" でない */
    }
    return csw[12] == 0 ? 0 : -2; /* 状態バイト */
}

/* ---- 列挙 ---- */

static void ehci_dec(uint64_t v) {
    char buf[24];
    int n = 0;
    if (v == 0) {
        serial_write("0");
        return;
    }
    while (v > 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        char c[2];
        c[0] = buf[--n];
        c[1] = '\0';
        serial_write(c);
    }
}

static void print_hex16(uint16_t v) {
    const char *hex = "0123456789abcdef";
    char buf[5];
    buf[0] = hex[(v >> 12) & 0xF];
    buf[1] = hex[(v >> 8) & 0xF];
    buf[2] = hex[(v >> 4) & 0xF];
    buf[3] = hex[v & 0xF];
    buf[4] = '\0';
    serial_write(buf);
}

static int port_connected(int port) {
    return (op_read(OP_PORTSC + 4u * (uint32_t)(port - 1)) &
            PORTSC_CCS) != 0;
}

/* ポートをリセットする。有効化されたら速度 (0/1/2) を、
 * だめなら -1 を返す。 */
static int ehci_port_reset(int port) {
    uint32_t off = OP_PORTSC + 4u * (uint32_t)(port - 1);
    uint32_t v = op_read(off);
    v &= ~PORTSC_CHANGES;
    v |= PORTSC_PR;
    v &= ~PORTSC_PE;
    op_write(off, v);
    speaker_wait_ticks(60000); /* ≒ 50ms */
    v = op_read(off);
    v &= ~PORTSC_CHANGES;
    v &= ~PORTSC_PR;
    op_write(off, v);
    /* リセット解除と有効化を待つ (最大 ≒ 500ms)。 */
    for (int i = 0; i < 10; i++) {
        speaker_wait_ticks(60000);
        v = op_read(off);
        if ((v & PORTSC_PR) == 0 && (v & PORTSC_PE) != 0) {
            break;
        }
    }
    v = op_read(off);
    /* 変化ビットを消す。 */
    op_write(off, (v & ~PORTSC_PR) | PORTSC_CHANGES);
    if ((v & PORTSC_PE) == 0) {
        return -1;
    }
    int pspd = (int)((v >> 26) & 3);
    if (pspd == 2) {
        return 2; /* 高速 */
    }
    if (pspd == 1) {
        return 1; /* 低速 */
    }
    /* PSPD を返さない実装 (QEMU) ではライン状態で判定する:
     * 有効化直後に K 状態 (2) なら高速、J 状態 (1) なら全速。 */
    if (((v >> 10) & 3) == 2) {
        return 2;
    }
    return 0; /* 全速 */
}

static void ehci_enumerate_port(int port) {
    int speed = ehci_port_reset(port);
    if (speed < 0) {
        serial_write_line("[ehci] port reset did not enable");
        return;
    }
    if (ehci_dev_count >= EHCI_MAX_DEVICES) {
        return;
    }
    ehci_device *dev = &ehci_devices[ehci_dev_count];
    memset(dev, 0, sizeof(*dev));
    dev->port = (uint8_t)port;
    dev->speed = speed;
    dev->address = 0;
    dev->max_packet = speed == 2 ? 64 : 8;
    uint8_t setup[8];
    uint8_t desc[18];
    /* 機器記述子の先頭 8 バイトで最大パケットを知る。 */
    uhci_build_setup(setup, 0x80, 0x06, 0x0100, 0, 8);
    if (ehci_control(dev, setup, desc, 8, 1) != 0) {
        serial_write_line("[ehci] get descriptor (8) failed");
        return;
    }
    if (desc[7] != 0) {
        dev->max_packet = desc[7];
    }
    /* アドレスを割り当てる。 */
    uint8_t addr = (uint8_t)(ehci_dev_count + 1);
    uhci_build_setup(setup, 0x00, 0x05, addr, 0, 0);
    if (ehci_control(dev, setup, NULL, 0, 0) != 0) {
        serial_write_line("[ehci] set address failed");
        return;
    }
    speaker_wait_ticks(3000); /* アドレス確定の回復時間 */
    dev->address = addr;
    /* 機器記述子の全体。 */
    uhci_build_setup(setup, 0x80, 0x06, 0x0100, 0, 18);
    if (ehci_control(dev, setup, desc, 18, 1) != 0) {
        serial_write_line("[ehci] get descriptor failed");
        return;
    }
    dev->vendor = (uint16_t)(desc[8] | (desc[9] << 8));
    dev->product = (uint16_t)(desc[10] | (desc[11] << 8));
    dev->dev_class = desc[4];
    if (desc[7] != 0) {
        dev->max_packet = desc[7];
    }
    /* 構成記述子: まず 9 バイト、次に全体。 */
    uint8_t cfgbuf[CTRL_DATA_MAX];
    uhci_build_setup(setup, 0x80, 0x06, 0x0200, 0, 9);
    if (ehci_control(dev, setup, cfgbuf, 9, 1) != 0) {
        serial_write_line("[ehci] get config header failed");
        return;
    }
    uint16_t total = (uint16_t)(cfgbuf[2] | (cfgbuf[3] << 8));
    if (total < 9 || total > CTRL_DATA_MAX) {
        total = 9;
    }
    uhci_build_setup(setup, 0x80, 0x06, 0x0200, 0, total);
    if (ehci_control(dev, setup, cfgbuf, total, 1) != 0) {
        serial_write_line("[ehci] get config failed");
        return;
    }
    uint8_t cfg_value = cfgbuf[5];
    (void)ehci_parse_config(cfgbuf, total, dev);
    /* 速度を PORTSC で報告しない実装 (QEMU) への対処:
     * バルク終端の最大パケットが 64 を超える構成は高速で
     * 動いているときしか現れない (全速バルクは最大 64)。
     * 今読んだ構成は現在の速度のものなので、ここから
     * 高速と確定できる。 */
    if (dev->speed != 2 && dev->bulk_max_packet > 64) {
        dev->speed = 2;
    }
    /* 構成を設定する。 */
    uhci_build_setup(setup, 0x00, 0x09, cfg_value, 0, 0);
    if (ehci_control(dev, setup, NULL, 0, 0) != 0) {
        serial_write_line("[ehci] set configuration failed");
        return;
    }
    int idx = ehci_dev_count;
    ehci_dev_count++;
    serial_write("[ehci] device port ");
    ehci_dec((uint64_t)port);
    serial_write(": vendor=");
    print_hex16(dev->vendor);
    serial_write(" product=");
    print_hex16(dev->product);
    serial_write(" class=");
    ehci_dec(dev->if_class);
    serial_write(dev->speed == 2 ? " speed=hs"
                                 : (dev->speed == 1 ? " speed=ls"
                                                    : " speed=fs"));
    serial_write_line("");
    /* マスストレージなら SCSI の初期確認まで行う。 */
    if (dev->bulk_in_ep != 0 && dev->bulk_out_ep != 0) {
        uint8_t cdb[16];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x12; /* INQUIRY */
        cdb[4] = 36;
        if (ehci_bot(dev, idx, cdb, 6, buf_phys_at(BOFF_SECTOR), 36,
                     1) == 0) {
            memset(cdb, 0, sizeof(cdb));
            cdb[0] = 0x25; /* READ CAPACITY(10) */
            if (ehci_bot(dev, idx, cdb, 10,
                         buf_phys_at(BOFF_SECTOR), 8, 1) == 0) {
                uint8_t *cap = buf_at(BOFF_SECTOR);
                uint32_t last = ((uint32_t)cap[0] << 24) |
                                ((uint32_t)cap[1] << 16) |
                                ((uint32_t)cap[2] << 8) | cap[3];
                uint32_t blksz = ((uint32_t)cap[4] << 24) |
                                 ((uint32_t)cap[5] << 16) |
                                 ((uint32_t)cap[6] << 8) | cap[7];
                if (blksz == 512 && last != 0xFFFFFFFFu) {
                    dev->storage_sectors = last + 1;
                    ehci_storage_idx = idx;
                    serial_write("[ehci] storage: ");
                    ehci_dec(dev->storage_sectors);
                    serial_write_line(" sectors");
                }
            }
        }
    }
}

/* ---- 初期化 ---- */

int ehci_init(uint64_t hhdm_offset) {
    if (ehci_found) {
        return 1;
    }
    ehci_hhdm = hhdm_offset;
    uint8_t bus, slot, func;
    if (!pci_find_class(0x0C, 0x03, 0x20, &bus, &slot, &func)) {
        return 0;
    }
    uint32_t bar0 = pci_bar32_mem(bus, slot, func, 0);
    if (bar0 == 0) {
        return 0;
    }
    uint64_t bar_phys = bar0 & 0xFFFFFFF0u;
    if ((bar0 & 0x6) == 0x4) { /* 64 ビット BAR */
        uint32_t hi = pci_bar32_mem(bus, slot, func, 1);
        bar_phys |= (uint64_t)hi << 32;
    }
    pci_enable_bus_master(bus, slot, func);
    if (paging_map_mmio(bar_phys, 0x1000, ehci_hhdm) != 0) {
        return 0;
    }
    ehci_cap = (volatile uint32_t *)(uintptr_t)(bar_phys + ehci_hhdm);
    uint32_t caplen = *(volatile uint8_t *)(uintptr_t)(bar_phys +
                                                       ehci_hhdm);
    ehci_op = (volatile uint32_t *)(uintptr_t)(bar_phys + ehci_hhdm +
                                               caplen);
    uint32_t hcs = ehci_cap[EHCI_HCSPARAMS / 4];
    ehci_n_ports = (int)(hcs & 0xF);
    uint32_t hcc = ehci_cap[EHCI_HCCPARAMS / 4];
    int addr64 = (int)(hcc & 1);
    uint32_t eecp = (hcc >> 8) & 0xFF;
    /* レガシー引き継ぎ (BIOS 所有セマフォ)。 */
    if (eecp >= 0x40) {
        uint32_t legsup = pci_config_read32(bus, slot, func,
                                            (uint8_t)eecp);
        if ((legsup & 0xFF) == 1) {
            uint16_t hi = pci_config_read16(bus, slot, func,
                                            (uint8_t)(eecp + 2));
            hi |= (1u << 8); /* OS 所有セマフォ (全体では bit24) */
            pci_config_write16(bus, slot, func, (uint8_t)(eecp + 2),
                               hi);
            for (int i = 0; i < 100; i++) {
                uint32_t v = pci_config_read32(bus, slot, func,
                                               (uint8_t)eecp);
                if ((v & (1u << 16)) == 0) {
                    break; /* BIOS 所有が外れた */
                }
                speaker_wait_ticks(12000);
            }
        }
    }
    /* 止めてからリセットする。 */
    op_write(OP_USBCMD, op_read(OP_USBCMD) & ~USBCMD_RS);
    for (int i = 0; i < 20; i++) {
        if (op_read(OP_USBSTS) & USBSTS_HCHALTED) {
            break;
        }
        speaker_wait_ticks(12000);
    }
    op_write(OP_USBCMD, op_read(OP_USBCMD) | USBCMD_HCRESET);
    for (int i = 0; i < 20; i++) {
        if ((op_read(OP_USBCMD) & USBCMD_HCRESET) == 0) {
            break;
        }
        speaker_wait_ticks(12000);
    }
    /* DMA ページ。32 ビットアドレス必須 (64 ビット対応でも
     * 下位に置けば共通に使える)。 */
    ehci_struct_phys = pmm_alloc_pages(1);
    ehci_buf_phys = pmm_alloc_pages(1);
    if (ehci_struct_phys == 0 || ehci_buf_phys == 0) {
        return 0;
    }
    if (!addr64 && (ehci_struct_phys >> 32) != 0) {
        return 0; /* この構成では届かない */
    }
    volatile uint32_t *s =
        (volatile uint32_t *)(uintptr_t)(ehci_struct_phys + ehci_hhdm);
    for (int i = 0; i < 1024; i++) {
        s[i] = 0;
    }
    uint8_t *b = buf_at(0);
    for (int i = 0; i < 4096; i++) {
        b[i] = 0;
    }
    /* 非同期先頭 QH (回収先頭ビット) と、その後ろに常時
     * リンクする転送 QH。 */
    volatile uint32_t *head = dma_ptr(ehci_struct_phys + OFF_HEAD_QH);
    head[0] = (uint32_t)(ehci_struct_phys + OFF_XFER_QH) | LINK_QH;
    head[1] = ehci_qh_epchar(0, 0, 2, 64, 1, 0);
    head[2] = (1u << 30);
    head[4] = LINK_T;
    head[5] = LINK_T;
    volatile uint32_t *xqh = dma_ptr(ehci_struct_phys + OFF_XFER_QH);
    xqh[0] = (uint32_t)ehci_struct_phys | LINK_QH;
    xqh[1] = ehci_qh_epchar(0, 0, 2, 64, 0, 0);
    xqh[2] = (1u << 30);
    xqh[4] = LINK_T;
    xqh[5] = LINK_T;
    op_write(OP_PERIODICLISTBASE, (uint32_t)ehci_struct_phys);
    op_write(OP_ASYNCLISTADDR, (uint32_t)ehci_struct_phys);
    op_write(OP_USBINTR, 0);
    op_write(OP_CONFIGFLAG, 1); /* ポートを EHCI へ回す */
    op_write(OP_USBCMD, USBCMD_RS | USBCMD_ASE | (8u << 16));
    ehci_found = 1;
    serial_write("[ehci] controller ready, ports=");
    ehci_dec((uint64_t)ehci_n_ports);
    serial_write_line("");
    /* 電源投入後の接続安定待ち。 */
    speaker_wait_ticks(120000);
    for (int p = 1; p <= ehci_n_ports; p++) {
        if (port_connected(p)) {
            ehci_enumerate_port(p);
        }
    }
    return 1;
}

int ehci_present(void) { return ehci_found; }

int ehci_device_count(void) { return ehci_dev_count; }

const ehci_device *ehci_device_at(int i) {
    if (i < 0 || i >= ehci_dev_count) {
        return NULL;
    }
    return &ehci_devices[i];
}

int ehci_storage_present(void) { return ehci_storage_idx >= 0; }

uint32_t ehci_storage_sectors(void) {
    if (ehci_storage_idx < 0) {
        return 0;
    }
    return ehci_devices[ehci_storage_idx].storage_sectors;
}

static int ehci_sector_xfer(uint32_t lba, int write) {
    if (ehci_storage_idx < 0) {
        return -1;
    }
    ehci_device *dev = &ehci_devices[ehci_storage_idx];
    if (lba >= dev->storage_sectors) {
        return -1;
    }
    uint8_t cdb[16];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? 0x2A : 0x28; /* WRITE(10) / READ(10) */
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[8] = 1; /* 1 ブロック */
    return ehci_bot(dev, ehci_storage_idx, cdb, 10,
                    buf_phys_at(BOFF_SECTOR), 512, !write) == 0
               ? 0
               : -1;
}

int ehci_storage_read_sector(uint32_t lba, uint8_t *buf) {
    if (ehci_sector_xfer(lba, 0) != 0) {
        return -1;
    }
    uint8_t *s = buf_at(BOFF_SECTOR);
    for (int i = 0; i < 512; i++) {
        buf[i] = s[i];
    }
    return 0;
}

int ehci_storage_write_sector(uint32_t lba, const uint8_t *buf) {
    uint8_t *s = buf_at(BOFF_SECTOR);
    for (int i = 0; i < 512; i++) {
        s[i] = buf[i];
    }
    return ehci_sector_xfer(lba, 1);
}
