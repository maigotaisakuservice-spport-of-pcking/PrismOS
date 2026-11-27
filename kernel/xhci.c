/* PrismOS — xHCI USB 3.x ホストコントローラ (本体)
 *
 * 設計方針は EHCI 版と同じ: ポーリング駆動、DMA 構造は
 * PMM ページに置き、物理アドレスでハードウェアに渡す。
 * xHCI 固有の要点:
 *   - 制御は全て TRB リング経由 (コマンド / イベント /
 *     終端ごとの転送リング)。サイクルビットで所有権を示す。
 *   - 機器の有効化は Enable Slot → Address Device →
 *     (記述子取得) → Configure Endpoint の順。
 *   - イベントは割り込みを使わずイベントリングをポーリング
 *     して消費し、ERDP を進めて EHB を落とす。
 * 対応はマスストレージ 1 台 (最初に見つけたもの) の
 * ブロック登録まで。他ポートの機器は列挙とログまで。
 */
#include "xhci.h"

#include "io.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "speaker.h"
#include "string.h"
#include "uhci.h" /* uhci_build_setup / uhci_build_cbw を再利用 */
#include "uvc.h"

/* ---- レジスタ配置 ---- */

/* 能力レジスタ (MMIO 先頭から) */
#define CAP_CAPLEN_VER 0x00 /* 下位 8 ビット: 能力長 */
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18

/* 運用レジスタ (能力長の先) */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_PAGESIZE 0x08
#define OP_DNCTRL 0x14
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC0 0x400

#define USBCMD_RUN (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBCMD_INTE (1u << 2)
#define USBSTS_HCH (1u << 0)
#define USBSTS_CNR (1u << 11)

/* ポート状態 (PORTSC) */
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR (1u << 4)
#define PORT_PP (1u << 9)
#define PORT_SPEED_SHIFT 10
#define PORT_CSC (1u << 17)
#define PORT_PEC (1u << 18)
#define PORT_WRC (1u << 19)
#define PORT_RC (1u << 21)
#define PORT_WPR (1u << 31)
#define PORT_CHANGE_MASK \
    (PORT_CSC | PORT_PEC | PORT_WRC | (1u << 20) | PORT_RC | \
     (1u << 22) | (1u << 23))

/* ランタイム: インタラプタ 0 */
#define RT_IMAN 0x20
#define RT_IMOD 0x24
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP 0x38
#define ERDP_EHB (1ull << 3)

/* TRB 種別 */
#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDR_DEV 11
#define TRB_CONFIG_EP 12
#define TRB_EVAL_CTX 13
#define TRB_CMD_NOOP 23
#define EV_TRANSFER 32
#define EV_CMD_COMPLETE 33

/* TRB 制御ビット */
#define TRB_CYCLE (1u << 0)
#define TRB_TC (1u << 1)    /* リンク TRB のサイクル切替 */
#define TRB_ISP (1u << 2)
#define TRB_CHAIN (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)

/* 完了コード */
#define CC_SUCCESS 1
#define CC_SHORT_PACKET 13

#define RING_TRBS 64 /* 末尾 1 つはリンク TRB (転送/コマンド) */

/* DMA バッファ内の配置 (EHCI と同じ割り付け) */
#define BOFF_DATA 64     /* 制御データ、最大 1024 バイト */
#define BOFF_CBW 2048    /* 31 バイト */
#define BOFF_CSW 2176    /* 13 バイト */
#define BOFF_SECTOR 2560 /* 512 バイト */
#define CTRL_DATA_MAX 1024

static int xhci_found;
static uint64_t xhci_hhdm;
static volatile uint32_t *xhci_cap; /* MMIO 先頭 */
static uint32_t xhci_oplen;         /* 能力長 = 運用開始位置 */
static uint32_t xhci_dboff;
static uint32_t xhci_rtsoff;
static uint32_t xhci_ctx_size; /* 文脈の大きさ: 32 か 64 */
static int xhci_num_ports;
static int xhci_num_slots;

static xhci_device xhci_devices[XHCI_MAX_DEVICES];
static int xhci_dev_count;
static int xhci_storage_idx = -1;

/* ---- MMIO ---- */

static uint32_t cap_read(uint32_t off) {
    return xhci_cap[off / 4];
}

static uint32_t op_read(uint32_t off) {
    return xhci_cap[(xhci_oplen + off) / 4];
}

static void op_write(uint32_t off, uint32_t v) {
    xhci_cap[(xhci_oplen + off) / 4] = v;
}

static void op_write64(uint32_t off, uint64_t v) {
    op_write(off, (uint32_t)(v & 0xFFFFFFFFu));
    op_write(off + 4, (uint32_t)(v >> 32));
}

static void rt_write(uint32_t off, uint32_t v) {
    xhci_cap[(xhci_rtsoff + off) / 4] = v;
}

static void rt_write64(uint32_t off, uint64_t v) {
    rt_write(off, (uint32_t)(v & 0xFFFFFFFFu));
    rt_write(off + 4, (uint32_t)(v >> 32));
}

static void db_write(uint32_t slot, uint32_t v) {
    xhci_cap[(xhci_dboff + slot * 4) / 4] = v;
}

static uint32_t port_read(int port) {
    return op_read(OP_PORTSC0 + (uint32_t)(port - 1) * 0x10);
}

static void port_write(int port, uint32_t v) {
    op_write(OP_PORTSC0 + (uint32_t)(port - 1) * 0x10, v);
}

/* ---- 小物 ---- */

static void xhci_dec(uint64_t v) {
    char buf[24];
    int i = 0;
    if (v == 0) {
        serial_write("0");
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        char s[2] = {buf[--i], '\0'};
        serial_write(s);
    }
}

static void print_hex16(uint16_t v) {
    static const char hexd[] = "0123456789abcdef";
    char out[5];
    out[0] = hexd[(v >> 12) & 0xF];
    out[1] = hexd[(v >> 8) & 0xF];
    out[2] = hexd[(v >> 4) & 0xF];
    out[3] = hexd[v & 0xF];
    out[4] = '\0';
    serial_write(out);
}

/* PIT カウンタで最大 chunks 回 (1 回 ≒ 50ms) 待つ。
 * cond が真になったら 1 を返す (EHCI と同じ作法)。 */
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

static int cond_always_false(void *ctx) {
    (void)ctx;
    return 0;
}

/* chunks 回分 (≒50ms 単位) ただ待つ。 */
static void xhci_delay(int chunks) {
    (void)wait_chunks(cond_always_false, NULL, chunks);
}

/* ---- 純粋関数 ---- */

uint32_t xhci_ep_info2(int ep_type, uint16_t max_packet,
                       uint8_t max_burst, int cerr) {
    return ((uint32_t)(cerr & 0x3) << 1) |
           ((uint32_t)(ep_type & 0x7) << 3) |
           ((uint32_t)max_burst << 8) |
           ((uint32_t)max_packet << 16);
}

uint32_t xhci_slot_info(int speed, int ctx_entries) {
    return ((uint32_t)(speed & 0xF) << 20) |
           ((uint32_t)(ctx_entries & 0x1F) << 27);
}

uint32_t xhci_setup_trt(int dir_in, int has_data) {
    if (!has_data) {
        return 0;
    }
    return dir_in ? 3u : 2u;
}

uint32_t xhci_trb_control(int type, int cycle, uint32_t flags) {
    return (cycle ? TRB_CYCLE : 0) | flags |
           ((uint32_t)(type & 0x3F) << 10);
}

/* 構成記述子を歩く (EHCI 版と同じ歩き方: ストレージ
 * 接続面の中のバルク終端だけを拾う)。 */
int xhci_parse_config(const uint8_t *cfg, size_t len,
                      xhci_device *info) {
    info->if_class = 0;
    info->if_num = 0;
    info->bulk_in_ep = 0;
    info->bulk_out_ep = 0;
    info->bulk_max_packet = 0;
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

/* ---- DMA 構造 ---- */

static uint64_t pg_dcbaa;   /* DCBAA + スクラッチパッド表 */
static uint64_t pg_cmdring; /* コマンドリング */
static uint64_t pg_evring;  /* イベントリング */
static uint64_t pg_erst;    /* イベントリングセグメント表 */
static uint64_t pg_buf;     /* 転送バッファ */

static volatile uint32_t *dma_ptr(uint64_t phys) {
    return (volatile uint32_t *)(uintptr_t)(phys + xhci_hhdm);
}

static uint8_t *buf_at(uint32_t off) {
    return (uint8_t *)(uintptr_t)(pg_buf + xhci_hhdm) + off;
}

static uint64_t buf_phys_at(uint32_t off) {
    return pg_buf + off;
}

static void zero_page(uint64_t phys) {
    volatile uint32_t *p = dma_ptr(phys);
    for (int i = 0; i < 1024; i++) {
        p[i] = 0;
    }
}

/* TRB リング (生産者)。末尾 TRB はリンク (輪の先頭へ、
 * サイクル切替あり)。イベントリングは消費者なので別扱い。 */
typedef struct {
    uint64_t phys;
    int enqueue;
    uint32_t cycle;
} xring;

static void ring_init(xring *r, uint64_t phys) {
    zero_page(phys);
    r->phys = phys;
    r->enqueue = 0;
    r->cycle = 1;
    /* リンク TRB を末尾に置く。サイクルビットは生産者が
     * 書き込むときに付けるので、ここでは 0 にしておく。 */
    volatile uint32_t *link = dma_ptr(phys + (RING_TRBS - 1) * 16);
    link[0] = (uint32_t)(phys & 0xFFFFFFFFu);
    link[1] = (uint32_t)(phys >> 32);
    link[2] = 0;
    link[3] = ((uint32_t)TRB_LINK << 10) | TRB_TC;
}

/* TRB を 1 つ積む。制御ワードのサイクルビットはリングの
 * 現在のサイクルで上書きする。積んだ TRB の物理番地を返す。 */
static uint64_t ring_push(xring *r, uint64_t param, uint32_t status,
                          uint32_t control) {
    if (r->enqueue == RING_TRBS - 1) {
        /* リンク TRB に現在のサイクルを付けて輪を閉じ、
         * サイクルを反転して先頭へ戻る。 */
        volatile uint32_t *link =
            dma_ptr(r->phys + (RING_TRBS - 1) * 16);
        link[3] = ((uint32_t)TRB_LINK << 10) | TRB_TC |
                  (r->cycle ? TRB_CYCLE : 0);
        r->enqueue = 0;
        r->cycle ^= 1u;
    }
    volatile uint32_t *t = dma_ptr(r->phys + (uint64_t)r->enqueue * 16);
    t[0] = (uint32_t)(param & 0xFFFFFFFFu);
    t[1] = (uint32_t)(param >> 32);
    t[2] = status;
    t[3] = (control & ~TRB_CYCLE) | (r->cycle ? TRB_CYCLE : 0);
    uint64_t at = r->phys + (uint64_t)r->enqueue * 16;
    r->enqueue++;
    return at;
}

/* ---- イベントリング (消費者) ---- */

static int ev_dequeue;       /* 次に読む TRB の番号 */
static uint32_t ev_cycle = 1;

typedef struct {
    uint64_t param;  /* TRB ポインタ / コマンド TRB ポインタ */
    uint32_t status; /* 完了コードは上位 8 ビット */
    uint32_t control;
    int type;
    int code;
    int slot;
    int ep; /* 転送イベントの終端番号 (DCI) */
} xevent;

static int cond_event_ready(void *ctx) {
    (void)ctx;
    volatile uint32_t *t = dma_ptr(pg_evring + (uint64_t)ev_dequeue * 16);
    return (int)((t[3] & TRB_CYCLE) == ev_cycle);
}

/* 次のイベントを 1 つ取る。chunks 回 (≒50ms 単位) 待って
 * 来なければ 0。取れたら ERDP を進める。 */
static int next_event(xevent *ev, int chunks) {
    if (!wait_chunks(cond_event_ready, NULL, chunks)) {
        return 0;
    }
    volatile uint32_t *t = dma_ptr(pg_evring + (uint64_t)ev_dequeue * 16);
    ev->param = (uint64_t)t[0] | ((uint64_t)t[1] << 32);
    ev->status = t[2];
    ev->control = t[3];
    ev->type = (int)((t[3] >> 10) & 0x3F);
    ev->code = (int)(t[2] >> 24);
    ev->slot = (int)(t[3] >> 24);
    ev->ep = (int)((t[3] >> 16) & 0x1F);
    ev_dequeue++;
    if (ev_dequeue >= RING_TRBS) {
        ev_dequeue = 0;
        ev_cycle ^= 1u;
    }
    /* ERDP: 次の位置 + EHB クリア。 */
    rt_write64(RT_ERDP,
               (pg_evring + (uint64_t)ev_dequeue * 16) | ERDP_EHB);
    return 1;
}

/* ---- コマンド ---- */

static xring cmd_ring;

/* コマンドを 1 つ実行し、完了コードを返す (失敗は負数)。
 * slot_out があれば Enable Slot のスロット ID を入れる。 */
static int xhci_command(uint64_t param, uint32_t control,
                        int *slot_out) {
    uint64_t at = ring_push(&cmd_ring, param, 0, control);
    db_write(0, 0);
    /* 対応する完了イベントを探す (ポート変化などの
     * イベントは読み飛ばす)。最大 ≒5 秒。 */
    for (int i = 0; i < 100; i++) {
        xevent ev;
        if (!next_event(&ev, 1)) {
            continue;
        }
        if (ev.type != EV_CMD_COMPLETE || ev.param != at) {
            continue;
        }
        if (slot_out != NULL) {
            *slot_out = ev.slot;
        }
        return ev.code;
    }
    serial_write_line("[xhci] command timeout");
    return -1;
}

/* ---- 機器ごとの状態 ---- */

typedef struct {
    uint64_t devctx; /* 出力文脈ページ */
    uint64_t inctx;  /* 入力文脈ページ */
    xring ep0;
    xring bulk_in;
    xring bulk_out;
    int bulk_ready;
} xdev_state;

static xdev_state xhci_state[XHCI_MAX_DEVICES];

/* UVC 検出 (後方で定義)。 */
static void uvc_detect(int di, const uint8_t *cfg, size_t len);

/* 入力文脈の各文脈へのポインタ (文脈サイズで割る)。 */
static volatile uint32_t *in_ctx_at(xdev_state *st, int idx) {
    return dma_ptr(st->inctx + (uint64_t)idx * xhci_ctx_size);
}

static volatile uint32_t *out_ctx_at(xdev_state *st, int idx) {
    return dma_ptr(st->devctx + (uint64_t)idx * xhci_ctx_size);
}

static void ctx_write_ep(volatile uint32_t *ctx, uint32_t info2,
                         uint64_t ring_phys, uint32_t avg_len) {
    ctx[0] = 0; /* 状態・間隔: 制御/バルクは 0 */
    ctx[1] = info2;
    ctx[2] = (uint32_t)(ring_phys & 0xFFFFFFFFu) | 1u; /* DCS=1 */
    ctx[3] = (uint32_t)(ring_phys >> 32);
    ctx[4] = avg_len;
    ctx[5] = 0;
    ctx[6] = 0;
    ctx[7] = 0;
}

/* ---- 転送 ---- */

/* 指定スロット + 終端 (DCI) の転送イベントを待つ。
 * 完了コードを返す (失敗は負数)。残量は residual に。 */
static int wait_transfer(int slot, int dci, uint32_t *residual) {
    for (int i = 0; i < 100; i++) {
        xevent ev;
        if (!next_event(&ev, 1)) {
            continue;
        }
        if (ev.type != EV_TRANSFER || ev.slot != slot ||
            ev.ep != dci) {
            continue;
        }
        if (residual != NULL) {
            *residual = ev.status & 0xFFFFFFu;
        }
        return ev.code;
    }
    return -1;
}

/* 制御転送 (EP0)。setup は 8 バイト、data は DMA バッファ
 * の物理番地 (無ければ 0)、len はデータ段の長さ。
 * 成功 0。受け取ったバイト数は呼び出し側がバッファを見る。 */
static int xhci_control(xdev_state *st, int slot,
                        const uint8_t setup[8], uint64_t data_phys,
                        uint32_t len, int dir_in) {
    uint64_t param = 0;
    for (int i = 0; i < 8; i++) {
        param |= (uint64_t)setup[i] << (8 * i);
    }
    uint32_t trt = xhci_setup_trt(dir_in, len > 0);
    ring_push(&st->ep0, param, 8,
              xhci_trb_control(TRB_SETUP, 0, TRB_IDT | (trt << 16)));
    if (len > 0) {
        ring_push(&st->ep0, data_phys, len,
                  xhci_trb_control(TRB_DATA, 0,
                                   (dir_in ? TRB_DIR_IN : 0) |
                                       (dir_in ? TRB_ISP : 0)));
    }
    uint32_t status_dir = (len == 0 || !dir_in) ? TRB_DIR_IN : 0;
    ring_push(&st->ep0, 0, 0,
              xhci_trb_control(TRB_STATUS, 0, TRB_IOC | status_dir));
    db_write((uint32_t)slot, 1); /* EP0 の DCI は 1 */
    int code = wait_transfer(slot, 1, NULL);
    if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
        return 0;
    }
    serial_write("[xhci] control failed: code ");
    xhci_dec((uint64_t)(code < 0 ? 0 : code));
    serial_write_line(code < 0 ? " (timeout)" : "");
    return -1;
}

/* バルク転送 1 本 (Normal TRB 1 つ)。成功 0。 */
static int xhci_bulk(int slot, xring *ring, int dci,
                     uint64_t buf_phys, uint32_t len) {
    ring_push(ring, buf_phys, len,
              xhci_trb_control(TRB_NORMAL, 0, TRB_IOC | TRB_ISP));
    db_write((uint32_t)slot, (uint32_t)dci);
    int code = wait_transfer(slot, dci, NULL);
    if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
        return 0;
    }
    serial_write("[xhci] bulk failed: code ");
    xhci_dec((uint64_t)(code < 0 ? 0 : code));
    serial_write_line(code < 0 ? " (timeout)" : "");
    return -1;
}

/* Bulk-Only Transport (EHCI 版と同じ流れ)。cdb を送り、
 * data_phys との間で data_len バイトを運び、CSW を確認
 * する。成功 0。 */
static uint32_t bot_tag = 1;

static int xhci_bot(xhci_device *dev, xdev_state *st,
                    const uint8_t *cdb, int cdb_len,
                    uint64_t data_phys, uint32_t data_len,
                    int dir_in) {
    uint8_t *cbw = buf_at(BOFF_CBW);
    uhci_build_cbw(cbw, bot_tag++, data_len, dir_in, cdb,
                   (size_t)cdb_len);
    if (xhci_bulk(dev->slot, &st->bulk_out, dev->bulk_out_dci,
                  buf_phys_at(BOFF_CBW), 31) != 0) {
        return -1;
    }
    if (data_len > 0) {
        xring *r = dir_in ? &st->bulk_in : &st->bulk_out;
        int dci = dir_in ? dev->bulk_in_dci : dev->bulk_out_dci;
        if (xhci_bulk(dev->slot, r, dci, data_phys, data_len) != 0) {
            return -1;
        }
    }
    if (xhci_bulk(dev->slot, &st->bulk_in, dev->bulk_in_dci,
                  buf_phys_at(BOFF_CSW), 13) != 0) {
        return -1;
    }
    const uint8_t *csw = buf_at(BOFF_CSW);
    uint32_t sig = (uint32_t)csw[0] | ((uint32_t)csw[1] << 8) |
                   ((uint32_t)csw[2] << 16) |
                   ((uint32_t)csw[3] << 24);
    if (sig != 0x53425355u || csw[12] != 0) {
        return -1;
    }
    return 0;
}

int xhci_storage_present(void) {
    return xhci_found && xhci_storage_idx >= 0;
}

uint32_t xhci_storage_sectors(void) {
    if (xhci_storage_idx < 0) {
        return 0;
    }
    return xhci_devices[xhci_storage_idx].storage_sectors;
}

static int xhci_scsi_rw(int write, uint32_t lba, uint8_t *buf) {
    if (xhci_storage_idx < 0) {
        return -1;
    }
    xhci_device *dev = &xhci_devices[xhci_storage_idx];
    xdev_state *st = &xhci_state[xhci_storage_idx];
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? 0x2A : 0x28; /* WRITE(10) / READ(10) */
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[8] = 1; /* 1 ブロック */
    if (!write) {
        if (xhci_bot(dev, st, cdb, 10, buf_phys_at(BOFF_SECTOR),
                     512, 1) != 0) {
            return -1;
        }
        const uint8_t *s = buf_at(BOFF_SECTOR);
        for (int i = 0; i < 512; i++) {
            buf[i] = s[i];
        }
        return 0;
    }
    uint8_t *s = buf_at(BOFF_SECTOR);
    for (int i = 0; i < 512; i++) {
        s[i] = buf[i];
    }
    return xhci_bot(dev, st, cdb, 10, buf_phys_at(BOFF_SECTOR),
                    512, 0);
}

int xhci_storage_read_sector(uint32_t lba, uint8_t *buf) {
    return xhci_scsi_rw(0, lba, buf);
}

int xhci_storage_write_sector(uint32_t lba, const uint8_t *buf) {
    return xhci_scsi_rw(1, lba, (uint8_t *)buf);
}

/* ---- 列挙 ---- */

static int cond_halted(void *ctx) {
    (void)ctx;
    return (op_read(OP_USBSTS) & USBSTS_HCH) != 0;
}

static int cond_not_halted(void *ctx) {
    (void)ctx;
    return (op_read(OP_USBSTS) & USBSTS_HCH) == 0;
}

static int cond_reset_done(void *ctx) {
    (void)ctx;
    return (op_read(OP_USBCMD) & USBCMD_HCRST) == 0;
}

static int cond_cnr_clear(void *ctx) {
    (void)ctx;
    return (op_read(OP_USBSTS) & USBSTS_CNR) == 0;
}

static int cond_port_enabled(void *ctx) {
    int port = *(const int *)ctx;
    return (port_read(port) & PORT_PED) != 0;
}

/* ポートをリセットして有効化を待つ。成功 0。 */
static int xhci_port_reset(int port) {
    uint32_t v = port_read(port);
    if ((v & PORT_PP) == 0) {
        port_write(port, v | PORT_PP);
        xhci_delay(1);
        v = port_read(port);
    }
    int speed = (int)((v >> PORT_SPEED_SHIFT) & 0xF);
    if (speed == 4) {
        /* 超速ポートはウォームリセット。 */
        port_write(port, (v & ~PORT_CHANGE_MASK) | PORT_WPR);
    } else {
        port_write(port, (v & ~PORT_CHANGE_MASK) | PORT_PR);
    }
    if (!wait_chunks(cond_port_enabled, &port, 40)) {
        serial_write("[xhci] port ");
        xhci_dec((uint64_t)port);
        serial_write_line(" reset did not enable");
        return -1;
    }
    /* 変化ビットを落としておく。 */
    v = port_read(port);
    port_write(port, v | PORT_CHANGE_MASK);
    xhci_delay(1);
    return 0;
}

/* 1 ポート分の列挙。失敗してもログだけ出して戻る。 */
static void xhci_enumerate_port(int port) {
    if (xhci_dev_count >= XHCI_MAX_DEVICES) {
        return;
    }
    int di = xhci_dev_count;
    xhci_device *dev = &xhci_devices[di];
    xdev_state *st = &xhci_state[di];
    memset(dev, 0, sizeof(*dev));
    memset(st, 0, sizeof(*st));
    dev->port = (uint8_t)port;

    if (xhci_port_reset(port) != 0) {
        return;
    }
    uint32_t pv = port_read(port);
    int speed_code = (int)((pv >> PORT_SPEED_SHIFT) & 0xF);
    dev->speed = speed_code == 1   ? 0
                 : speed_code == 2 ? 1
                 : speed_code == 4 ? 3
                                   : 2;
    uint16_t ep0_max = dev->speed == 1   ? 8
                       : dev->speed == 0 ? 64
                                         : 512;

    /* スロットを取る。 */
    int slot = -1;
    int code = xhci_command(0, xhci_trb_control(TRB_ENABLE_SLOT, 0, 0),
                            &slot);
    if (code != CC_SUCCESS || slot <= 0 || slot > xhci_num_slots) {
        serial_write_line("[xhci] enable slot failed");
        return;
    }
    dev->slot = (uint8_t)slot;

    /* 機器文脈と入力文脈、EP0 リングを用意する。 */
    st->devctx = pmm_alloc_pages(1);
    st->inctx = pmm_alloc_pages(1);
    uint64_t ep0_pg = pmm_alloc_pages(1);
    if (st->devctx == 0 || st->inctx == 0 || ep0_pg == 0) {
        serial_write_line("[xhci] no memory for device");
        return;
    }
    zero_page(st->devctx);
    ring_init(&st->ep0, ep0_pg);
    volatile uint32_t *dcbaa = dma_ptr(pg_dcbaa);
    dcbaa[slot * 2] = (uint32_t)(st->devctx & 0xFFFFFFFFu);
    dcbaa[slot * 2 + 1] = (uint32_t)(st->devctx >> 32);

    /* Address Device: スロット文脈 + EP0 文脈を入れる。 */
    zero_page(st->inctx);
    volatile uint32_t *ctrl = in_ctx_at(st, 0);
    ctrl[1] = 0x3; /* Add: スロット + EP0 */
    volatile uint32_t *sctx = in_ctx_at(st, 1);
    sctx[0] = xhci_slot_info(speed_code, 1);
    sctx[1] = (uint32_t)port << 16;
    sctx[2] = 0;
    sctx[3] = 0;
    ctx_write_ep(in_ctx_at(st, 2),
                 xhci_ep_info2(4, ep0_max, 0, 3), st->ep0.phys, 8);
    code = xhci_command(st->inctx,
                        xhci_trb_control(TRB_ADDR_DEV, 0,
                                         (uint32_t)slot << 24),
                        NULL);
    if (code != CC_SUCCESS) {
        serial_write("[xhci] address device failed: code ");
        xhci_dec((uint64_t)code);
        serial_write_line("");
        return;
    }
    dev->address =
        (uint8_t)(out_ctx_at(st, 0)[3] & 0xFF);

    /* 機器記述子を読む。 */
    uint8_t setup[8];
    uint8_t *data = buf_at(BOFF_DATA);
    uhci_build_setup(setup, 0x80, 0x06, 0x0100, 0, 18);
    if (xhci_control(st, slot, setup, buf_phys_at(BOFF_DATA), 18,
                     1) != 0) {
        serial_write_line("[xhci] get device descriptor failed");
        return;
    }
    dev->vendor = (uint16_t)(data[8] | (data[9] << 8));
    dev->product = (uint16_t)(data[10] | (data[11] << 8));
    dev->dev_class = data[4];
    dev->max_packet = data[7];
    if (data[7] != 0 && data[7] != (uint8_t)ep0_max) {
        /* 最大パケットが申告と違えば Evaluate Context で
         * 直す。失敗しても読み進める (多くの機器は既定値で
         * も記述子までは読める)。 */
        zero_page(st->inctx);
        in_ctx_at(st, 0)[1] = 0x2; /* Add: EP0 のみ */
        ctx_write_ep(in_ctx_at(st, 2),
                     xhci_ep_info2(4, data[7], 0, 3), st->ep0.phys,
                     8);
        (void)xhci_command(
            st->inctx,
            xhci_trb_control(TRB_EVAL_CTX, 0, (uint32_t)slot << 24),
            NULL);
        ep0_max = data[7];
    }

    /* 構成記述子: まず 9 バイト、次に全体。 */
    uhci_build_setup(setup, 0x80, 0x06, 0x0200, 0, 9);
    if (xhci_control(st, slot, setup, buf_phys_at(BOFF_DATA), 9,
                     1) != 0) {
        serial_write_line("[xhci] get config header failed");
        return;
    }
    uint16_t total = (uint16_t)(data[2] | (data[3] << 8));
    if (total < 9 || total > CTRL_DATA_MAX) {
        total = 9;
    }
    uhci_build_setup(setup, 0x80, 0x06, 0x0200, 0, total);
    if (xhci_control(st, slot, setup, buf_phys_at(BOFF_DATA), total,
                     1) != 0) {
        serial_write_line("[xhci] get config failed");
        return;
    }
    uint8_t cfg_value = data[5];
    (void)xhci_parse_config(data, total, dev);
    uhci_build_setup(setup, 0x00, 0x09, cfg_value, 0, 0);
    if (xhci_control(st, slot, setup, 0, 0, 0) != 0) {
        serial_write_line("[xhci] set configuration failed");
        return;
    }
    /* UVC カメラの検出 (構成記述子は data に残っている)。 */
    uvc_detect(di, data, total);
    xhci_dev_count++;
    serial_write("[xhci] device port ");
    xhci_dec((uint64_t)port);
    serial_write(": vendor=");
    print_hex16(dev->vendor);
    serial_write(" product=");
    print_hex16(dev->product);
    serial_write(" class=");
    xhci_dec(dev->if_class);
    serial_write(dev->speed == 3   ? " speed=ss"
                 : dev->speed == 2 ? " speed=hs"
                 : dev->speed == 1 ? " speed=ls"
                                   : " speed=fs");
    serial_write_line("");

    /* マスストレージならバルク終端を設定して SCSI の
     * 初期確認まで行う (登録は最初の 1 台だけ)。 */
    if (dev->bulk_in_ep == 0 || dev->bulk_out_ep == 0 ||
        xhci_storage_idx >= 0) {
        return;
    }
    uint64_t bin_pg = pmm_alloc_pages(1);
    uint64_t bout_pg = pmm_alloc_pages(1);
    if (bin_pg == 0 || bout_pg == 0) {
        return;
    }
    ring_init(&st->bulk_in, bin_pg);
    ring_init(&st->bulk_out, bout_pg);
    dev->bulk_in_dci = (uint8_t)(2 * dev->bulk_in_ep + 1);
    dev->bulk_out_dci = (uint8_t)(2 * dev->bulk_out_ep);
    int max_dci = dev->bulk_in_dci > dev->bulk_out_dci
                      ? dev->bulk_in_dci
                      : dev->bulk_out_dci;
    zero_page(st->inctx);
    in_ctx_at(st, 0)[1] = 1u | (1u << dev->bulk_in_dci) |
                          (1u << dev->bulk_out_dci);
    volatile uint32_t *osctx = out_ctx_at(st, 0);
    volatile uint32_t *isctx = in_ctx_at(st, 1);
    for (int i = 0; i < 8; i++) {
        isctx[i] = osctx[i];
    }
    isctx[0] = (osctx[0] & ~(0x1Fu << 27)) |
               ((uint32_t)max_dci << 27);
    ctx_write_ep(in_ctx_at(st, 1 + dev->bulk_out_dci),
                 xhci_ep_info2(2, dev->bulk_max_packet, 0, 3),
                 st->bulk_out.phys, 512);
    ctx_write_ep(in_ctx_at(st, 1 + dev->bulk_in_dci),
                 xhci_ep_info2(6, dev->bulk_max_packet, 0, 3),
                 st->bulk_in.phys, 512);
    code = xhci_command(st->inctx,
                        xhci_trb_control(TRB_CONFIG_EP, 0,
                                         (uint32_t)slot << 24),
                        NULL);
    if (code != CC_SUCCESS) {
        serial_write("[xhci] configure endpoint failed: code ");
        xhci_dec((uint64_t)code);
        serial_write_line("");
        return;
    }
    st->bulk_ready = 1;
    uint8_t cdb[16];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12; /* INQUIRY */
    cdb[4] = 36;
    if (xhci_bot(dev, st, cdb, 6, buf_phys_at(BOFF_SECTOR), 36,
                 1) != 0) {
        serial_write_line("[xhci] inquiry failed");
        return;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25; /* READ CAPACITY(10) */
    if (xhci_bot(dev, st, cdb, 10, buf_phys_at(BOFF_SECTOR), 8,
                 1) != 0) {
        serial_write_line("[xhci] read capacity failed");
        return;
    }
    const uint8_t *capb = buf_at(BOFF_SECTOR);
    uint32_t last = ((uint32_t)capb[0] << 24) |
                    ((uint32_t)capb[1] << 16) |
                    ((uint32_t)capb[2] << 8) | capb[3];
    uint32_t blksz = ((uint32_t)capb[4] << 24) |
                     ((uint32_t)capb[5] << 16) |
                     ((uint32_t)capb[6] << 8) | capb[7];
    if (blksz != 512 || last == 0xFFFFFFFFu) {
        serial_write_line("[xhci] unsupported capacity");
        return;
    }
    dev->storage_sectors = last + 1;
    xhci_storage_idx = di;
    serial_write("[xhci] storage: ");
    xhci_dec(dev->storage_sectors);
    serial_write_line(" sectors");
}

/* ---- UVC カメラ (クラスドライバ) ----
 * 列挙時に構成記述子を uvc_parse_config で読み、形式表を
 * 記録する (最初の 1 台だけ)。転送の開始・取り決めは
 * uvc_start、フレームの取り出しは uvc_poll_frame。
 * QEMU に UVC エミュレーションが無いため実転送は
 * 未検証 (uvc.h の注記参照)。 */

#define UVC_FMT_CAP 16
static int uvc_dev_idx = -1;
static uvc_format_frame uvc_formats[UVC_FMT_CAP];
static int uvc_fmt_count;
static int uvc_vc_iface = -1;
static int uvc_vs_iface = -1;
static uint8_t uvc_ep_addr;
static uint16_t uvc_ep_mps;
static uint8_t uvc_alt;
static uint8_t uvc_interval;
static xring uvc_ring;
static uint8_t uvc_dci;
static uint64_t uvc_chunk_pages;
static int uvc_streaming;
static uvc_assembler uvc_asm;
static uint8_t uvc_frame_buf[256 * 1024];

static void uvc_detect(int di, const uint8_t *cfg, size_t len) {
    if (uvc_dev_idx >= 0) {
        return;
    }
    int vc = -1, vs = -1;
    uint8_t ep = 0, alt = 0, interval = 0;
    uint16_t mps = 0;
    int n = uvc_parse_config(cfg, len, uvc_formats, UVC_FMT_CAP,
                             &vc, &vs, &ep, &mps, &alt, &interval);
    if (n <= 0 || vs < 0 || ep == 0) {
        return;
    }
    uvc_dev_idx = di;
    uvc_fmt_count = n;
    uvc_vc_iface = vc;
    uvc_vs_iface = vs;
    uvc_ep_addr = ep;
    uvc_ep_mps = mps;
    uvc_alt = alt;
    uvc_interval = interval;
    serial_write("[xhci] uvc camera: ");
    xhci_dec((uint64_t)n);
    serial_write_line(" formats");
}

int uvc_present(void) {
    return uvc_dev_idx >= 0;
}

int uvc_format_count(void) {
    return uvc_fmt_count;
}

const uvc_format_frame *uvc_format_at(int i) {
    if (i < 0 || i >= uvc_fmt_count) {
        return NULL;
    }
    return &uvc_formats[i];
}

int uvc_start(int format_idx) {
    if (uvc_dev_idx < 0 || format_idx < 0 ||
        format_idx >= uvc_fmt_count) {
        return -1;
    }
    if (uvc_streaming) {
        return 0;
    }
    xhci_device *dev = &xhci_devices[uvc_dev_idx];
    xdev_state *st = &xhci_state[uvc_dev_idx];
    int slot = dev->slot;
    uvc_format_frame *f = &uvc_formats[format_idx];
    if (uvc_ring.phys == 0) {
        uint64_t pg = pmm_alloc_pages(1);
        uint64_t cp = pmm_alloc_pages(8);
        if (pg == 0 || cp == 0) {
            return -1;
        }
        ring_init(&uvc_ring, pg);
        uvc_chunk_pages = cp;
    }
    uvc_dci = (uint8_t)(2 * (uvc_ep_addr & 0x0F) + 1);
    /* ストリーミング終端 (等時 IN) の文脈を設定する。 */
    zero_page(st->inctx);
    in_ctx_at(st, 0)[1] = 1u | (1u << uvc_dci);
    volatile uint32_t *osctx = out_ctx_at(st, 0);
    volatile uint32_t *isctx = in_ctx_at(st, 1);
    for (int i = 0; i < 8; i++) {
        isctx[i] = osctx[i];
    }
    isctx[0] = (osctx[0] & ~(0x1Fu << 27)) |
               ((uint32_t)uvc_dci << 27);
    int interval_exp;
    if (dev->speed >= 2) {
        /* HS/SS: bInterval は 2^(n-1) マイクロフレーム */
        interval_exp = uvc_interval > 0 ? uvc_interval - 1 : 0;
    } else {
        /* FS: bInterval はフレーム数。2 の冪の指数へ丸める */
        int iv = uvc_interval > 0 ? uvc_interval : 1;
        interval_exp = 0;
        while ((1 << interval_exp) < iv && interval_exp < 15) {
            interval_exp++;
        }
    }
    volatile uint32_t *epctx = in_ctx_at(st, 1 + uvc_dci);
    ctx_write_ep(epctx, xhci_ep_info2(5, uvc_ep_mps, 0, 3),
                 uvc_ring.phys, uvc_ep_mps);
    epctx[0] = (uint32_t)interval_exp << 16; /* Interval */
    /* 平均 TRB 長 + Max ESIT ペイロード (どちらも mps) */
    epctx[4] = (uint32_t)uvc_ep_mps | ((uint32_t)uvc_ep_mps << 16);
    int code = xhci_command(st->inctx,
                            xhci_trb_control(TRB_CONFIG_EP, 0,
                                             (uint32_t)slot << 24),
                            NULL);
    if (code != CC_SUCCESS) {
        serial_write_line("[xhci] uvc: configure endpoint failed");
        return -1;
    }
    uint8_t setup[8];
    uint8_t *data = buf_at(BOFF_DATA);
    /* 代替設定を選ぶ (帯域のある alt)。 */
    uhci_build_setup(setup, 0x01, 0x0B, uvc_alt, (uint16_t)uvc_vs_iface,
                     0);
    if (xhci_control(st, slot, setup, 0, 0, 0) != 0) {
        serial_write_line("[xhci] uvc: set interface failed");
        return -1;
    }
    /* VS Probe: SET_CUR → GET_CUR → Commit SET_CUR。 */
    uvc_build_probe(data, f->format_index, f->frame_index,
                    f->default_interval);
    uhci_build_setup(setup, 0x21, 0x01, 0x0100,
                     (uint16_t)uvc_vs_iface, 26);
    if (xhci_control(st, slot, setup, buf_phys_at(BOFF_DATA), 26,
                     0) != 0) {
        serial_write_line("[xhci] uvc: probe set failed");
        return -1;
    }
    uhci_build_setup(setup, 0xA1, 0x81, 0x0100,
                     (uint16_t)uvc_vs_iface, 26);
    if (xhci_control(st, slot, setup, buf_phys_at(BOFF_DATA), 26,
                     1) != 0) {
        serial_write_line("[xhci] uvc: probe get failed");
        return -1;
    }
    uint32_t neg_interval = 0;
    uvc_parse_probe(data, NULL, &neg_interval, NULL);
    if (neg_interval != 0) {
        uvc_build_probe(data, f->format_index, f->frame_index,
                        neg_interval);
    }
    uhci_build_setup(setup, 0x21, 0x01, 0x0200,
                     (uint16_t)uvc_vs_iface, 26);
    if (xhci_control(st, slot, setup, buf_phys_at(BOFF_DATA), 26,
                     0) != 0) {
        serial_write_line("[xhci] uvc: commit failed");
        return -1;
    }
    uvc_asm_init(&uvc_asm, uvc_frame_buf, sizeof(uvc_frame_buf));
    uvc_streaming = 1;
    serial_write_line("[xhci] uvc: streaming started");
    return 0;
}

int uvc_poll_frame(uint8_t *buf, uint32_t cap) {
    if (!uvc_streaming) {
        return -1;
    }
    xhci_device *dev = &xhci_devices[uvc_dev_idx];
    int slot = dev->slot;
    for (int batch = 0; batch < 16 && !uvc_asm.complete; batch++) {
        for (int t = 0; t < 8; t++) {
            ring_push(&uvc_ring,
                      uvc_chunk_pages + (uint64_t)t * 4096, 4096,
                      xhci_trb_control(TRB_NORMAL, 0,
                                       TRB_IOC | TRB_ISP));
        }
        db_write((uint32_t)slot, uvc_dci);
        for (int t = 0; t < 8; t++) {
            uint32_t residual = 0;
            int code = wait_transfer(slot, uvc_dci, &residual);
            if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
                return -1;
            }
            uint32_t got = 4096u - (residual & 0xFFFFFFu);
            if (got > 4096u) {
                got = 0;
            }
            const uint8_t *chunk =
                (const uint8_t *)dma_ptr(uvc_chunk_pages +
                                         (uint64_t)t * 4096);
            if (uvc_asm_feed(&uvc_asm, chunk, got)) {
                break;
            }
        }
    }
    if (!uvc_asm.complete) {
        return 0;
    }
    uint32_t n = uvc_asm.used;
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, uvc_frame_buf, n);
    uvc_asm_init(&uvc_asm, uvc_frame_buf, sizeof(uvc_frame_buf));
    return (int)n;
}

void uvc_stop(void) {
    if (uvc_dev_idx < 0 || !uvc_streaming) {
        return;
    }
    xhci_device *dev = &xhci_devices[uvc_dev_idx];
    xdev_state *st = &xhci_state[uvc_dev_idx];
    uint8_t setup[8];
    uhci_build_setup(setup, 0x01, 0x0B, 0, (uint16_t)uvc_vs_iface, 0);
    (void)xhci_control(st, dev->slot, setup, 0, 0, 0);
    uvc_streaming = 0;
}

/* ---- 初期化 ---- */

int xhci_present(void) {
    return xhci_found;
}

int xhci_device_count(void) {
    return xhci_dev_count;
}

const xhci_device *xhci_device_at(int i) {
    if (i < 0 || i >= xhci_dev_count) {
        return NULL;
    }
    return &xhci_devices[i];
}

int xhci_init(uint64_t hhdm_offset) {
    if (xhci_found) {
        return 1;
    }
    xhci_hhdm = hhdm_offset;
    uint8_t bus, slot, func;
    if (!pci_find_class(0x0C, 0x03, 0x30, &bus, &slot, &func)) {
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
    if (paging_map_mmio(bar_phys, 0x20000, xhci_hhdm) != 0) {
        return 0;
    }
    xhci_cap = (volatile uint32_t *)(uintptr_t)(bar_phys + xhci_hhdm);
    xhci_oplen = cap_read(CAP_CAPLEN_VER) & 0xFF;
    xhci_dboff = cap_read(CAP_DBOFF) & ~0x3u;
    xhci_rtsoff = cap_read(CAP_RTSOFF) & ~0x1Fu;
    uint32_t hcs1 = cap_read(CAP_HCSPARAMS1);
    uint32_t hcs2 = cap_read(CAP_HCSPARAMS2);
    uint32_t hcc1 = cap_read(CAP_HCCPARAMS1);
    xhci_num_slots = (int)(hcs1 & 0xFF);
    xhci_num_ports = (int)((hcs1 >> 24) & 0x7F);
    xhci_ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
    if (xhci_num_ports == 0 || xhci_num_slots == 0) {
        return 0;
    }

    /* レガシー所有権 (BIOS から引き継ぐ)。xECP の能力
     * リストを歩き、USB レガシーサポートがあれば OS
     * 所有セマフォを立てる。 */
    uint32_t xecp = (hcc1 >> 16) & 0xFFFF;
    if (xecp != 0) {
        uint32_t off = xecp * 4;
        for (int guard = 0; guard < 32; guard++) {
            uint32_t v = cap_read(off);
            uint32_t id = v & 0xFF;
            if (id == 1) {
                xhci_cap[off / 4] = v | (1u << 24);
                for (int i = 0; i < 40; i++) {
                    if ((cap_read(off) & (1u << 16)) == 0) {
                        break;
                    }
                    xhci_delay(1);
                }
            }
            uint32_t next = (v >> 8) & 0xFF;
            if (next == 0) {
                break;
            }
            off += next * 4;
        }
    }

    /* 停止 → リセット → 準備完了を待つ。 */
    if ((op_read(OP_USBSTS) & USBSTS_HCH) == 0) {
        op_write(OP_USBCMD, op_read(OP_USBCMD) & ~USBCMD_RUN);
        if (!wait_chunks(cond_halted, NULL, 40)) {
            return 0;
        }
    }
    op_write(OP_USBCMD, op_read(OP_USBCMD) | USBCMD_HCRST);
    if (!wait_chunks(cond_reset_done, NULL, 40)) {
        return 0;
    }
    if (!wait_chunks(cond_cnr_clear, NULL, 40)) {
        return 0;
    }
    if ((op_read(OP_PAGESIZE) & 1u) == 0) {
        return 0; /* 4K ページ必須 */
    }
    int slots_en = xhci_num_slots < 32 ? xhci_num_slots : 32;
    op_write(OP_CONFIG, (uint32_t)slots_en);

    /* DMA 構造を用意する。 */
    pg_dcbaa = pmm_alloc_pages(1);
    pg_cmdring = pmm_alloc_pages(1);
    pg_evring = pmm_alloc_pages(1);
    pg_erst = pmm_alloc_pages(1);
    pg_buf = pmm_alloc_pages(1);
    if (!pg_dcbaa || !pg_cmdring || !pg_evring || !pg_erst ||
        !pg_buf) {
        return 0;
    }
    zero_page(pg_dcbaa);
    zero_page(pg_erst);
    zero_page(pg_buf);
    /* スクラッチパッド (要求された場合のみ)。 */
    uint32_t scratch =
        (((hcs2 >> 27) & 0x1Fu) << 5) | ((hcs2 >> 21) & 0x1Fu);
    if (scratch > 0) {
        if (scratch > 64) {
            return 0;
        }
        volatile uint32_t *arr = dma_ptr(pg_dcbaa + 2048);
        for (uint32_t i = 0; i < scratch; i++) {
            uint64_t p = pmm_alloc_pages(1);
            if (p == 0) {
                return 0;
            }
            zero_page(p);
            arr[i * 2] = (uint32_t)(p & 0xFFFFFFFFu);
            arr[i * 2 + 1] = (uint32_t)(p >> 32);
        }
        volatile uint32_t *dcbaa = dma_ptr(pg_dcbaa);
        dcbaa[0] = (uint32_t)((pg_dcbaa + 2048) & 0xFFFFFFFFu);
        dcbaa[1] = (uint32_t)((pg_dcbaa + 2048) >> 32);
    }
    op_write64(OP_DCBAAP, pg_dcbaa);

    ring_init(&cmd_ring, pg_cmdring);
    ev_dequeue = 0;
    ev_cycle = 1;
    zero_page(pg_evring);
    volatile uint32_t *erst = dma_ptr(pg_erst);
    erst[0] = (uint32_t)(pg_evring & 0xFFFFFFFFu);
    erst[1] = (uint32_t)(pg_evring >> 32);
    erst[2] = RING_TRBS;
    erst[3] = 0;
    rt_write(RT_ERSTSZ, 1);
    rt_write64(RT_ERSTBA, pg_erst);
    rt_write64(RT_ERDP, pg_evring | ERDP_EHB);
    rt_write(RT_IMOD, 0);
    rt_write(RT_IMAN, 1); /* IP を落とす。IE は 0 (ポーリング) */
    op_write64(OP_CRCR, pg_cmdring | 1u);

    op_write(OP_USBCMD, USBCMD_RUN | USBCMD_INTE);
    if (!wait_chunks(cond_not_halted, NULL, 40)) {
        return 0;
    }
    xhci_found = 1;
    serial_write_line("[xhci] controller ready");

    /* 繋がっているポートを列挙する。 */
    for (int port = 1; port <= xhci_num_ports; port++) {
        uint32_t v = port_read(port);
        if ((v & PORT_CCS) == 0) {
            continue;
        }
        xhci_delay(2);
        xhci_enumerate_port(port);
    }
    return 1;
}
