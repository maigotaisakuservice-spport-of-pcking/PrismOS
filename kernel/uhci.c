/* PrismOS — UHCI USB ホストコントローラ (列挙まで) */
#include "uhci.h"

#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "speaker.h"
#include "string.h"

/* UHCI レジスタ (I/O ベースからのずれ)。 */
#define UHCI_USBCMD 0x00
#define UHCI_USBSTS 0x02
#define UHCI_FRNUM 0x06
#define UHCI_FLBASEADD 0x08
#define UHCI_PORTSC1 0x10
#define UHCI_PORTSC2 0x12

#define UHCI_CMD_RS (1u << 0)   /* スケジュール実行 */
#define UHCI_CMD_HCRESET (1u << 1)
#define UHCI_CMD_MAXP (1u << 6) /* 最大パケット 64 */

#define UHCI_PORT_CCS (1u << 0)   /* 機器が繋がっている */
#define UHCI_PORT_PE (1u << 2)    /* ポート有効 */
#define UHCI_PORT_LSDA (1u << 8)  /* 低速機器 */
#define UHCI_PORT_RESET (1u << 9) /* リセット */

#define UHCI_TD_ACTIVE (1u << 23)
#define UHCI_TD_STALLED (1u << 22)
#define UHCI_TD_ERRORS ((1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) | \
                        (1u << 21))
#define UHCI_TD_CERR ((3u) << 27)
#define UHCI_TD_IOC (1u << 30)

#define UHCI_PID_SETUP 0x2D
#define UHCI_PID_IN 0x69
#define UHCI_PID_OUT 0xE1

#define UHCI_LINK_TERMINATE 1u
#define UHCI_LINK_QH 2u

#define UHCI_MAX_TDS 40

/* TD 状態の低速ビット (低速機器向けの転送に立てる)。 */
#define UHCI_TD_LS (1u << 26)

/* 割り込み TD の番号 (制御用 TD の後ろ)。 */
#define UHCI_INT_TD_BASE 48

static uint16_t uhci_iobase;
static uint64_t uhci_hhdm;
static int uhci_found;
static uhci_device uhci_devices[UHCI_MAX_DEVICES];
static int uhci_dev_count;

/* DMA 用の物理ページ: フレーム一覧 (1 ページ) と、
 * QH + TD + バッファ (1 ページ)。 */
static uint64_t uhci_frame_phys;
static uint64_t uhci_work_phys;
static uint64_t uhci_td_phys;

/* 機器ごとの実行時状態 (公開情報とは別に持つ)。 */
typedef struct {
    int hid_toggle;
    uint8_t prev_report[8];
    int bulk_in_toggle;
    int bulk_out_toggle;
    int hid_slot; /* 割り込み QH のスロット (0..1、-1=なし) */
} uhci_rt;

/* 次に空いている割り込み QH スロット。 */
static int uhci_next_hid_slot;
static uhci_rt uhci_rts[UHCI_MAX_DEVICES];
static uint32_t uhci_cbw_tag = 1;
static int uhci_storage_idx = -1;

/* ---- 純粋関数 ---- */

void uhci_build_setup(uint8_t *pkt, uint8_t req_type, uint8_t request,
                      uint16_t value, uint16_t index, uint16_t length) {
    pkt[0] = req_type;
    pkt[1] = request;
    pkt[2] = (uint8_t)(value & 0xFF);
    pkt[3] = (uint8_t)((value >> 8) & 0xFF);
    pkt[4] = (uint8_t)(index & 0xFF);
    pkt[5] = (uint8_t)((index >> 8) & 0xFF);
    pkt[6] = (uint8_t)(length & 0xFF);
    pkt[7] = (uint8_t)((length >> 8) & 0xFF);
}

uint32_t uhci_td_token(uint32_t pid, uint8_t addr, uint8_t ep, int toggle,
                       uint32_t maxlen) {
    uint32_t token = pid;
    token |= (uint32_t)(addr & 0x7F) << 8;
    token |= (uint32_t)(ep & 0x0F) << 15;
    if (toggle) {
        token |= (1u << 19);
    }
    token |= (uint32_t)((maxlen - 1) & 0x7FF) << 21;
    return token;
}

void uhci_parse_device_desc(const uint8_t *d, uhci_device *info) {
    info->max_packet = d[7];
    info->vendor = (uint16_t)(d[8] | (d[9] << 8));
    info->product = (uint16_t)(d[10] | (d[11] << 8));
    info->dev_class = d[4];
}

int uhci_td_ok(uint32_t status) {
    if (status & UHCI_TD_ACTIVE) {
        return 0;
    }
    if (status & (UHCI_TD_STALLED | UHCI_TD_ERRORS)) {
        return 0;
    }
    return 1;
}

int uhci_parse_config(const uint8_t *cfg, size_t len, uhci_device *info) {
    if (cfg == NULL || len < 9 || cfg[1] != 2) {
        return -1;
    }
    info->config_value = cfg[5];
    size_t pos = 0;
    int matched = 0;
    uint8_t cur_num = 0;
    while (pos + 2 <= len) {
        uint8_t blen = cfg[pos];
        uint8_t btype = cfg[pos + 1];
        if (blen < 2 || pos + blen > len) {
            break;
        }
        if (btype == 4 && blen >= 9) {
            /* 接続面: 番号・種別・手順。代替設定 0 のみ。 */
            cur_num = cfg[pos + 2];
            if (cfg[pos + 3] == 0 && !matched &&
                (cfg[pos + 5] == 3 || cfg[pos + 5] == 8)) {
                matched = 1;
                info->if_class = cfg[pos + 5];
                info->if_num = cur_num;
                if (cfg[pos + 5] == 8) {
                    info->kind = UHCI_KIND_STORAGE;
                } else if (cfg[pos + 7] == 1) {
                    info->kind = UHCI_KIND_HID_KEYBOARD;
                } else if (cfg[pos + 7] == 2) {
                    info->kind = UHCI_KIND_HID_MOUSE;
                } else {
                    info->kind = UHCI_KIND_UNKNOWN;
                }
            }
        } else if (btype == 5 && blen >= 7 && matched &&
                   cur_num == info->if_num) {
            /* 終端: 向き・転送種別・最大パケット。 */
            uint8_t ep = cfg[pos + 2];
            uint8_t attr = cfg[pos + 3];
            uint16_t mp = (uint16_t)(cfg[pos + 4] |
                                     ((uint16_t)cfg[pos + 5] << 8));
            if (mp > 255) {
                mp = 255;
            }
            if ((attr & 3) == 3 && (ep & 0x80) != 0) {
                info->hid_ep = (uint8_t)(ep & 0x0F);
                info->hid_max_packet = (uint8_t)mp;
            } else if ((attr & 3) == 2) {
                if ((ep & 0x80) != 0) {
                    info->bulk_in_ep = (uint8_t)(ep & 0x0F);
                } else {
                    info->bulk_out_ep = (uint8_t)(ep & 0x0F);
                }
                info->bulk_max_packet = (uint8_t)mp;
            }
        }
        pos += blen;
    }
    return matched ? 0 : -1;
}

/* HID 使用コード → キーコード (PS/2 側と同じ値)。
 * 英字は大文字小文字を shift で、Ctrl+英字は
 * keyboard.c と同じレガシー / BASE コードで返す。 */
int uhci_hid_key_code(uint8_t usage, int shift, int ctrl) {
    if (usage >= 0x04 && usage <= 0x1D) {
        int letter = usage - 0x04; /* a=0 */
        if (ctrl) {
            switch (letter) {
                case 'o' - 'a':
                    return PRISM_KEY_CTRL_O;
                case 'v' - 'a':
                    return PRISM_KEY_CTRL_V;
                case 'i' - 'a':
                    return PRISM_KEY_CTRL_I;
                case 'm' - 'a':
                    return PRISM_KEY_CTRL_M;
                case 'd' - 'a':
                    return PRISM_KEY_CTRL_D;
                case 'p' - 'a':
                    return PRISM_KEY_CTRL_P;
                case 'n' - 'a':
                    return PRISM_KEY_CTRL_N;
                case 'e' - 'a':
                    return PRISM_KEY_CTRL_E;
                case 'r' - 'a':
                    return PRISM_KEY_CTRL_R;
                default:
                    return PRISM_KEY_CTRL_LETTER_BASE + letter;
            }
        }
        return shift ? 'A' + letter : 'a' + letter;
    }
    if (usage >= 0x1E && usage <= 0x27) {
        static const char digits[10] = {'1', '2', '3', '4', '5',
                                        '6', '7', '8', '9', '0'};
        static const char digits_shift[10] = {'!', '@', '#', '$', '%',
                                              '^', '&', '*', '(', ')'};
        int d = usage - 0x1E;
        return shift ? digits_shift[d] : digits[d];
    }
    switch (usage) {
        case 0x28:
            return '\n';
        case 0x29:
            return 0x1B; /* Esc */
        case 0x2A:
            return '\b';
        case 0x2B:
            return '\t';
        case 0x2C:
            return ' ';
        case 0x2D:
            return shift ? '_' : '-';
        case 0x2E:
            return shift ? '+' : '=';
        case 0x2F:
            return shift ? '{' : '[';
        case 0x30:
            return shift ? '}' : ']';
        case 0x31:
            return shift ? '|' : '\\';
        case 0x33:
            return shift ? ':' : ';';
        case 0x34:
            return shift ? '"' : '\'';
        case 0x35:
            return shift ? '~' : '`';
        case 0x36:
            return shift ? '<' : ',';
        case 0x37:
            return shift ? '>' : '.';
        case 0x38:
            return shift ? '?' : '/';
        case 0x4F:
            return PRISM_KEY_RIGHT;
        case 0x50:
            return PRISM_KEY_LEFT;
        case 0x51:
            return PRISM_KEY_DOWN;
        case 0x52:
            return PRISM_KEY_UP;
        default:
            break;
    }
    if (usage >= 0x3A && usage <= 0x45) {
        return PRISM_KEY_F1 + (usage - 0x3A);
    }
    return -1;
}

void uhci_build_cbw(uint8_t *cbw, uint32_t tag, uint32_t data_len,
                    int data_in, const uint8_t *cdb, size_t cdb_len) {
    for (int i = 0; i < 31; i++) {
        cbw[i] = 0;
    }
    cbw[0] = 0x55; /* "USBC" (0x43425355) */
    cbw[1] = 0x53;
    cbw[2] = 0x42;
    cbw[3] = 0x43;
    cbw[4] = (uint8_t)(tag & 0xFF);
    cbw[5] = (uint8_t)((tag >> 8) & 0xFF);
    cbw[6] = (uint8_t)((tag >> 16) & 0xFF);
    cbw[7] = (uint8_t)((tag >> 24) & 0xFF);
    cbw[8] = (uint8_t)(data_len & 0xFF);
    cbw[9] = (uint8_t)((data_len >> 8) & 0xFF);
    cbw[10] = (uint8_t)((data_len >> 16) & 0xFF);
    cbw[11] = (uint8_t)((data_len >> 24) & 0xFF);
    cbw[12] = data_in ? 0x80 : 0x00;
    cbw[13] = 0; /* LUN 0 */
    cbw[14] = (uint8_t)cdb_len;
    for (size_t i = 0; i < cdb_len && i < 16; i++) {
        cbw[15 + i] = cdb[i];
    }
}

/* ---- ドライバ ---- */

static void *uhci_phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(phys + uhci_hhdm);
}

static uint16_t uhci_in16(uint16_t off) {
    return prism_inw((uint16_t)(uhci_iobase + off));
}

static void uhci_out16(uint16_t off, uint16_t value) {
    prism_outw((uint16_t)(uhci_iobase + off), value);
}

static void uhci_out32(uint16_t off, uint32_t value) {
    prism_outl((uint16_t)(uhci_iobase + off), value);
}

/* QH は作業ページの先頭 (32 バイト)。
 * TD はその後ろに 8 個まで (各 32 バイト)。
 * 設定パケットは 512 バイト目、データは 1024 バイト目に置く。 */
static volatile uint32_t *uhci_qh(void) {
    return (volatile uint32_t *)uhci_phys_to_virt(uhci_work_phys);
}

static volatile uint32_t *uhci_td(int i) {
    return (volatile uint32_t *)((uint8_t *)uhci_phys_to_virt(
                                     uhci_td_phys) +
                                 (size_t)i * 32);
}

static uint64_t uhci_td_addr(int i) {
    return uhci_td_phys + (uint64_t)i * 32;
}

/* 割り込み用 QH は作業ページの 32 バイト目から (制御用
 * QH は先頭)。機器の番号ごとに 1 つ使う。 */
static volatile uint32_t *uhci_int_qh(int idx) {
    return (volatile uint32_t *)((uint8_t *)uhci_phys_to_virt(
                                     uhci_work_phys) +
                                 32 + (size_t)idx * 32);
}

/* HID レポートの置き場 (TD ページの後ろ、機器ごとに
 * 16 バイト)。 */
static uint8_t *uhci_report_buf(int idx) {
    return (uint8_t *)uhci_phys_to_virt(uhci_td_phys) + 3072 +
           (size_t)idx * 16;
}

static uint64_t uhci_report_phys(int idx) {
    return uhci_td_phys + 3072 + (uint64_t)idx * 16;
}

static uint8_t *uhci_setup_buf(void) {
    return (uint8_t *)uhci_phys_to_virt(uhci_work_phys) + 512;
}

static uint8_t *uhci_data_buf(void) {
    return (uint8_t *)uhci_phys_to_virt(uhci_work_phys) + 1024;
}

static uint64_t uhci_setup_phys(void) {
    return uhci_work_phys + 512;
}

static uint64_t uhci_data_phys(void) {
    return uhci_work_phys + 1024;
}

static uint32_t uhci_link_ptr(uint64_t phys, int is_qh) {
    uint32_t v = (uint32_t)(phys & 0xFFFFFFF0u);
    if (is_qh) {
        v |= UHCI_LINK_QH;
    }
    return v;
}

/* TD を 1 個作る。next は次の TD の番号、-1 なら終端。 */
/* ハブ経由の低速機器宛ての TD には LS ビットが要る
 * (アドレスごとの表。ルート直結の低速機器には付けない)。 */
static int uhci_ls_td[128];

static void uhci_make_td(int i, uint32_t token, uint64_t buf_phys,
                         int next) {
    volatile uint32_t *td = uhci_td(i);
    uint32_t link = UHCI_LINK_TERMINATE;
    if (next >= 0) {
        link = uhci_link_ptr(uhci_td_addr(next), 0);
    }
    td[0] = link;
    td[1] = UHCI_TD_ACTIVE | UHCI_TD_CERR | UHCI_TD_IOC;
    {
        uint32_t addr = (token >> 8) & 0x7F;
        if (uhci_ls_td[addr]) {
            td[1] |= UHCI_TD_LS;
        }
    }
    td[2] = token;
    td[3] = (uint32_t)(buf_phys & 0xFFFFFFFFu);
}

/* 制御転送を 1 回行う。setup は 8 バイトの設定パケット、
 * data_len はデータ段階の長さ (0 ならデータ段階なし)。
 * data は転送先 (IN) または転送元 (OUT) のバッファ。
 * max_packet は制御転送の最大パケット (データ段階を
 * その大きさに分けて送る)。成功したら 0。TD は 0 番から
 * 使う。 */
static int uhci_control(uint8_t addr, const uint8_t *setup,
                        uint8_t *data, uint16_t data_len, int data_in,
                        uint8_t max_packet) {
    uint8_t *sbuf = uhci_setup_buf();
    uint8_t *dbuf = uhci_data_buf();
    int ntd = 0;
    /* 設定段階。 */
    for (int i = 0; i < 8; i++) {
        sbuf[i] = setup[i];
    }
    uhci_make_td(ntd, uhci_td_token(UHCI_PID_SETUP, addr, 0, 0, 8),
                 uhci_setup_phys(), 1); /* 次はデータ段階か状態段階 */
    ntd++;
    /* データ段階 (最大パケットごとに TD を分ける)。 */
    if (data_len > 0) {
        if (max_packet == 0) {
            max_packet = 8;
        }
        if (!data_in) {
            for (int i = 0; i < data_len; i++) {
                dbuf[i] = data[i];
            }
        }
        int done = 0;
        int toggle = 1;
        while (done < data_len) {
            int chunk = data_len - done;
            if (chunk > max_packet) {
                chunk = max_packet;
            }
            uhci_make_td(ntd,
                         uhci_td_token(data_in ? UHCI_PID_IN : UHCI_PID_OUT,
                                       addr, 0, toggle, (uint32_t)chunk),
                         uhci_data_phys() + (uint64_t)done, ntd + 1);
            ntd++;
            done += chunk;
            toggle = toggle ? 0 : 1;
            if (ntd >= UHCI_MAX_TDS - 1) {
                break;
            }
        }
    }
    /* 状態段階 (長さ 0、方向はデータ段階と逆)。 */
    uhci_make_td(ntd,
                 uhci_td_token(data_in ? UHCI_PID_OUT : UHCI_PID_IN,
                               addr, 0, 1, 0),
                 0, -1);
    int status_td = ntd;
    ntd++;

    /* QH の要素に先頭 TD を繋ぎ、完了を待つ。
     * UHCI は 1 フレーム (1ms) に 1 TD しか進まないため、
     * TD の数だけ時間がかかる。PIT で 200ms まで待つ。 */
    volatile uint32_t *qh = uhci_qh();
    qh[1] = uhci_link_ptr(uhci_td_addr(0), 0);
    volatile uint32_t *td = uhci_td(status_td);
    int done = 0;
    for (int chunk = 0; chunk < 4 && !done; chunk++) {
        uint16_t start = speaker_pit0_count();
        uint32_t waited = 0;
        while (waited < 60000) { /* 60000 ティック ≒ 50ms */
            if ((td[1] & UHCI_TD_ACTIVE) == 0) {
                done = 1;
                break;
            }
            uint16_t now = speaker_pit0_count();
            waited = (uint32_t)(uint16_t)(start - now);
            __asm__ volatile("pause");
        }
    }
    qh[1] = UHCI_LINK_TERMINATE;
    if (!done) {
        return -1;
    }
    /* 全 TD を検査する (以前は最後の TD だけを見ていた
     * ため、途中の失敗を取りこぼし得た)。IN のデータ
     * 段階は、実際に届いた長さの合計が要求と一致する
     * ことも確かめる (短い転送の黙った切り詰め防止)。 */
    for (int i = 0; i <= status_td; i++) {
        volatile uint32_t *t = uhci_td(i);
        if ((t[1] & UHCI_TD_ACTIVE) != 0) {
            return -1;
        }
        if (!uhci_td_ok(t[1])) {
            return -1;
        }
    }
    if (data_len > 0 && data_in) {
        uint32_t got = 0;
        for (int i = 1; i < status_td; i++) {
            uint32_t al = uhci_td(i)[1] & 0x7FF;
            got += al == 0x7FF ? 0 : al + 1;
        }
        if (got < (uint32_t)data_len) {
            return -1;
        }
        for (int i = 0; i < data_len; i++) {
            data[i] = dbuf[i];
        }
    }
    return 0;
}

/* CBW / CSW の置き場 (TD ページ内、レポート置き場の後)。
 * データバッファとは別にして、コマンド包とデータが
 * 互いを上書きしないようにする。 */
static uint8_t *uhci_cbw_buf(void) {
    return (uint8_t *)uhci_phys_to_virt(uhci_td_phys) + 3200;
}

static uint64_t uhci_cbw_phys(void) {
    return uhci_td_phys + 3200;
}

/* バルク転送。len バイトを ep へ送るか、ep から受ける。
 * buf_phys がデータの物理アドレス。トグルは機器ごとに
 * 保持する。成功したら 0。 */
static int uhci_bulk(uhci_device *dev, uhci_rt *rt, uint8_t ep,
                     uint64_t buf_phys, uint32_t len, int in) {
    uint8_t mp = dev->bulk_max_packet;
    if (mp == 0) {
        mp = 64;
    }
    int *toggle = in ? &rt->bulk_in_toggle : &rt->bulk_out_toggle;
    int ntd = 0;
    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > mp) {
            chunk = mp;
        }
        if (ntd >= UHCI_MAX_TDS) {
            return -1;
        }
        volatile uint32_t *td = uhci_td(ntd);
        uint32_t link = UHCI_LINK_TERMINATE;
        if (done + chunk < len) {
            link = uhci_link_ptr(uhci_td_addr(ntd + 1), 0);
        }
        td[0] = link;
        td[1] = UHCI_TD_ACTIVE | UHCI_TD_CERR | UHCI_TD_IOC;
        td[2] = uhci_td_token(in ? UHCI_PID_IN : UHCI_PID_OUT,
                              dev->address, ep, *toggle, chunk);
        td[3] = (uint32_t)((buf_phys + done) & 0xFFFFFFFFu);
        *toggle = *toggle ? 0 : 1;
        done += chunk;
        ntd++;
    }
    if (ntd == 0) {
        return 0;
    }
    volatile uint32_t *qh = uhci_qh();
    qh[1] = uhci_link_ptr(uhci_td_addr(0), 0);
    volatile uint32_t *last = uhci_td(ntd - 1);
    int finished = 0;
    for (int chunk = 0; chunk < 8 && !finished; chunk++) {
        uint16_t start = speaker_pit0_count();
        uint32_t waited = 0;
        while (waited < 60000) {
            if ((last[1] & UHCI_TD_ACTIVE) == 0) {
                finished = 1;
                break;
            }
            uint16_t now = speaker_pit0_count();
            waited = (uint32_t)(uint16_t)(start - now);
            __asm__ volatile("pause");
        }
    }
    qh[1] = UHCI_LINK_TERMINATE;
    if (!finished) {
        return -1;
    }
    uint32_t got = 0;
    for (int i = 0; i < ntd; i++) {
        volatile uint32_t *t = uhci_td(i);
        if ((t[1] & UHCI_TD_ACTIVE) != 0 || !uhci_td_ok(t[1])) {
            return -1;
        }
        uint32_t al = t[1] & 0x7FF;
        got += al == 0x7FF ? 0 : al + 1;
    }
    if (in && got < len) {
        return -1; /* 短い受信を黙って切り詰めない */
    }
    return 0;
}

/* SCSI コマンドを Bulk-Only Transport で 1 回実行する。
 * データ段階は uhci_data_buf 経由 (呼び出し側が出し入れ
 * する)。CBW と CSW は専用バッファを使う。成功 0。 */
static int uhci_bot(uhci_device *dev, uhci_rt *rt,
                    const uint8_t *cdb, size_t cdb_len,
                    uint32_t data_len, int data_in) {
    uint8_t *cbw = uhci_cbw_buf();
    uhci_build_cbw(cbw, uhci_cbw_tag++, data_len, data_in, cdb,
                   cdb_len);
    if (uhci_bulk(dev, rt, dev->bulk_out_ep, uhci_cbw_phys(), 31,
                  0) != 0) {
        return -1;
    }
    if (data_len > 0) {
        if (uhci_bulk(dev, rt,
                      data_in ? dev->bulk_in_ep : dev->bulk_out_ep,
                      uhci_data_phys(), data_len, data_in) != 0) {
            return -1;
        }
    }
    if (uhci_bulk(dev, rt, dev->bulk_in_ep, uhci_cbw_phys(), 13,
                  1) != 0) {
        return -1;
    }
    if (!(cbw[0] == 0x55 && cbw[1] == 0x53 && cbw[2] == 0x42 &&
          cbw[3] == 0x53)) {
        return -1; /* CSW の署名 "USBS" でない */
    }
    if (cbw[12] != 0) {
        return -1; /* コマンド失敗 */
    }
    return 0;
}

/* ストレージの初期確認: INQUIRY と READ CAPACITY。
 * 成功したら dev->storage_sectors が入る。 */
static int uhci_storage_probe(uhci_device *dev, uhci_rt *rt) {
    uint8_t *dbuf = uhci_data_buf();
    static const uint8_t cdb_inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    if (uhci_bot(dev, rt, cdb_inquiry, 6, 36, 1) != 0) {
        return -1;
    }
    if (dbuf[0] != 0x00 && dbuf[0] != 0x05) {
        /* 直接アクセス機器 (0x00) か光学ドライブ (0x05)
         * 以外はディスクとして扱わない。 */
        return -1;
    }
    static const uint8_t cdb_tur[6] = {0x00, 0, 0, 0, 0, 0};
    for (int i = 0; i < 3; i++) {
        if (uhci_bot(dev, rt, cdb_tur, 6, 0, 0) == 0) {
            break;
        }
        speaker_wait_ticks(60000);
    }
    static const uint8_t cdb_cap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (uhci_bot(dev, rt, cdb_cap, 10, 8, 1) != 0) {
        return -1;
    }
    uint32_t last = ((uint32_t)dbuf[0] << 24) |
                    ((uint32_t)dbuf[1] << 16) |
                    ((uint32_t)dbuf[2] << 8) | dbuf[3];
    uint32_t block = ((uint32_t)dbuf[4] << 24) |
                     ((uint32_t)dbuf[5] << 16) |
                     ((uint32_t)dbuf[6] << 8) | dbuf[7];
    if (block != 512 || last == 0xFFFFFFFFu) {
        return -1; /* 512 バイトブロック以外は未対応 */
    }
    dev->storage_sectors = last + 1;
    return 0;
}

int uhci_storage_present(void) {
    return uhci_storage_idx >= 0;
}

uint32_t uhci_storage_sectors(void) {
    if (uhci_storage_idx < 0) {
        return 0;
    }
    return uhci_devices[uhci_storage_idx].storage_sectors;
}

static int uhci_storage_rw(uint32_t lba, uint8_t *buf, int write) {
    if (uhci_storage_idx < 0) {
        return -1;
    }
    uhci_device *dev = &uhci_devices[uhci_storage_idx];
    uhci_rt *rt = &uhci_rts[uhci_storage_idx];
    if (lba >= dev->storage_sectors) {
        return -1;
    }
    uint8_t *dbuf = uhci_data_buf();
    if (write) {
        for (int i = 0; i < 512; i++) {
            dbuf[i] = buf[i];
        }
    }
    uint8_t cdb[10];
    cdb[0] = write ? 0x2A : 0x28; /* WRITE(10) / READ(10) */
    cdb[1] = 0;
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 1; /* 1 ブロック */
    cdb[9] = 0;
    if (uhci_bot(dev, rt, cdb, 10, 512, write ? 0 : 1) != 0) {
        return -1;
    }
    if (!write) {
        for (int i = 0; i < 512; i++) {
            buf[i] = dbuf[i];
        }
    }
    return 0;
}

int uhci_storage_read_sector(uint32_t lba, uint8_t *buf) {
    return uhci_storage_rw(lba, buf, 0);
}

int uhci_storage_write_sector(uint32_t lba, const uint8_t *buf) {
    return uhci_storage_rw(lba, (uint8_t *)buf, 1);
}

/* HID の割り込み TD を用意する。 */
static void uhci_hid_arm(int idx) {
    uhci_device *dev = &uhci_devices[idx];
    volatile uint32_t *td = uhci_td(UHCI_INT_TD_BASE + idx);
    td[0] = UHCI_LINK_TERMINATE;
    td[1] = UHCI_TD_ACTIVE | UHCI_TD_CERR |
            (dev->low_speed ? UHCI_TD_LS : 0);
    td[2] = uhci_td_token(UHCI_PID_IN, dev->address, dev->hid_ep,
                          uhci_rts[idx].hid_toggle,
                          dev->hid_max_packet != 0
                              ? dev->hid_max_packet
                              : 8);
    td[3] = (uint32_t)(uhci_report_phys(idx) & 0xFFFFFFFFu);
    volatile uint32_t *qh =
        uhci_int_qh(uhci_rts[idx].hid_slot);
    qh[1] = uhci_link_ptr(uhci_td_addr(UHCI_INT_TD_BASE + idx), 0);
}

/* HID の割り込み TD を張り直す (トグルを進める)。 */
static void uhci_hid_rearm(int idx) {
    uhci_rts[idx].hid_toggle = uhci_rts[idx].hid_toggle ? 0 : 1;
    uhci_hid_arm(idx);
}

void uhci_input_poll(void) {
    if (!uhci_found) {
        return;
    }
    for (int idx = 0; idx < uhci_dev_count; idx++) {
        uhci_device *dev = &uhci_devices[idx];
        if (dev->kind != UHCI_KIND_HID_KEYBOARD &&
            dev->kind != UHCI_KIND_HID_MOUSE) {
            continue;
        }
        volatile uint32_t *td = uhci_td(UHCI_INT_TD_BASE + idx);
        if ((td[1] & UHCI_TD_ACTIVE) != 0) {
            continue; /* まだレポートが来ていない */
        }
        if (!uhci_td_ok(td[1])) {
            uhci_hid_rearm(idx);
            continue;
        }
        uint32_t al = td[1] & 0x7FF;
        int len = al == 0x7FF ? 0 : (int)al + 1;
        uint8_t *rep = uhci_report_buf(idx);
        if (dev->kind == UHCI_KIND_HID_MOUSE) {
            if (len >= 3) {
                mouse_inject((int)(int8_t)rep[1],
                             (int)(int8_t)rep[2], rep[0] & 0x07);
                serial_write_line("[usb] hid mouse report");
            }
        } else {
            /* キーボード: 前回レポートに無い使用コードが
             * 新しい押下。修飾は現在のレポートから取る。 */
            int shift = (rep[0] & 0x22) != 0;
            int ctrl = (rep[0] & 0x11) != 0;
            for (int k = 2; k < 8; k++) {
                uint8_t usage = rep[k];
                if (usage == 0) {
                    continue;
                }
                int was = 0;
                for (int j = 2; j < 8; j++) {
                    if (uhci_rts[idx].prev_report[j] == usage) {
                        was = 1;
                    }
                }
                if (was) {
                    continue;
                }
                int code = uhci_hid_key_code(usage, shift, ctrl);
                if (code >= 0) {
                    keyboard_inject(code);
                    serial_write_line("[usb] hid keyboard key");
                }
            }
            for (int k = 0; k < 8; k++) {
                uhci_rts[idx].prev_report[k] = rep[k];
            }
        }
        uhci_hid_rearm(idx);
    }
}

/* 1 ポートを列挙する。成功したら機器を表に追加して 0、
 * 機器が無ければ 1、失敗したら -1。 */
static int uhci_enumerate_device(int port, int low_speed,
                                 int via_hub);

/* ハブの下流ポートを列挙する (1 段のみ)。 */
static void uhci_hub_enumerate(int hub_idx) {
    uhci_device *hub = &uhci_devices[hub_idx];
    uint8_t setup[8];
    uint8_t buf[64];
    /* ハブ記述子: バイト 2 が下流ポート数。 */
    uhci_build_setup(setup, 0xA0, 0x06, 0x2900, 0, 8);
    if (uhci_control(hub->address, setup, buf, 8, 1,
                     hub->max_packet) != 0) {
        serial_write_line("[usb] hub descriptor failed");
        return;
    }
    int nports = buf[2];
    if (nports > 4) {
        nports = 4;
    }
    for (int p = 1; p <= nports; p++) {
        /* ポート電源を入れて安定を待つ。 */
        uhci_build_setup(setup, 0x23, 0x03, 8, (uint16_t)p, 0);
        if (uhci_control(hub->address, setup, NULL, 0, 0,
                         hub->max_packet) != 0) {
            continue;
        }
        speaker_wait_ticks(120000);
        uhci_build_setup(setup, 0xA3, 0x00, 0, (uint16_t)p, 4);
        if (uhci_control(hub->address, setup, buf, 4, 1,
                         hub->max_packet) != 0) {
            continue;
        }
        uint16_t status = (uint16_t)(buf[0] | (buf[1] << 8));
        if ((status & 1) == 0) {
            continue; /* 何も繋がっていない */
        }
        /* 接続変化を消してからポートリセット。 */
        uhci_build_setup(setup, 0x23, 0x01, 16, (uint16_t)p, 0);
        (void)uhci_control(hub->address, setup, NULL, 0, 0,
                           hub->max_packet);
        uhci_build_setup(setup, 0x23, 0x03, 4, (uint16_t)p, 0);
        if (uhci_control(hub->address, setup, NULL, 0, 0,
                         hub->max_packet) != 0) {
            continue;
        }
        speaker_wait_ticks(60000);
        uhci_build_setup(setup, 0xA3, 0x00, 0, (uint16_t)p, 4);
        if (uhci_control(hub->address, setup, buf, 4, 1,
                         hub->max_packet) != 0) {
            continue;
        }
        status = (uint16_t)(buf[0] | (buf[1] << 8));
        uhci_build_setup(setup, 0x23, 0x01, 20, (uint16_t)p, 0);
        (void)uhci_control(hub->address, setup, NULL, 0, 0,
                           hub->max_packet);
        if ((status & 2) == 0) {
            continue; /* ポートが有効にならなかった */
        }
        int ls = (status & (1 << 9)) != 0;
        (void)uhci_enumerate_device(hub->port, ls, 1);
    }
}

static int uhci_enumerate_port(int port) {
    uint16_t portsc_off = port == 1 ? UHCI_PORTSC1 : UHCI_PORTSC2;
    uint16_t sc = uhci_in16(portsc_off);
    if ((sc & UHCI_PORT_CCS) == 0) {
        return 1; /* 機器が繋がっていない */
    }
    int low_speed = (sc & UHCI_PORT_LSDA) != 0;
    /* リセットを 50ms かける。 */
    uhci_out16(portsc_off, UHCI_PORT_RESET);
    speaker_wait_ticks(60000);
    uhci_out16(portsc_off, 0);
    speaker_wait_ticks(12000);
    /* ポートを有効にする。 */
    uhci_out16(portsc_off, UHCI_PORT_PE);
    speaker_wait_ticks(12000);
    sc = uhci_in16(portsc_off);
    if ((sc & UHCI_PORT_PE) == 0) {
        return -1;
    }
    int idx = uhci_enumerate_device(port, low_speed, 0);
    if (idx >= 0 && uhci_devices[idx].dev_class == 0x09) {
        serial_write_line("[usb] hub found, enumerating "
                          "downstream");
        uhci_hub_enumerate(idx);
    }
    return idx >= 0 ? 0 : -1;
}

/* ポートのリセット済みの機器を 1 台列挙する。成功したら
 * 機器の添字、失敗したら -1 を返す。 */
static int uhci_enumerate_device(int port, int low_speed,
                                 int via_hub) {
    uint8_t setup[8];
    uint8_t buf[256];
    if (via_hub && low_speed) {
        uhci_ls_td[0] = 1; /* アドレス 0 の間も LS で送る */
    }
    /* まず 8 バイトだけ読んで最大パケットを知る。 */
    uhci_build_setup(setup, 0x80, 0x06, 0x0100, 0, 8);
    if (uhci_control(0, setup, buf, 8, 1, 8) != 0) {
        uhci_ls_td[0] = 0;
        return -1;
    }
    uint8_t max_packet = buf[7];
    if (max_packet == 0) {
        uhci_ls_td[0] = 0;
        return -1;
    }
    /* アドレスを割り当てる。 */
    uint8_t addr = (uint8_t)(uhci_dev_count + 1);
    uhci_build_setup(setup, 0x00, 0x05, addr, 0, 0);
    if (uhci_control(0, setup, NULL, 0, 0, max_packet) != 0) {
        uhci_ls_td[0] = 0;
        return -1;
    }
    if (via_hub && low_speed) {
        uhci_ls_td[addr] = 1;
    }
    uhci_ls_td[0] = 0;
    speaker_wait_ticks(12000);
    /* 機器記述子を全部読む。 */
    uhci_build_setup(setup, 0x80, 0x06, 0x0100, 0, 18);
    if (uhci_control(addr, setup, buf, 18, 1, max_packet) != 0) {
        return -1;
    }
    int idx = uhci_dev_count;
    uhci_device *dev = &uhci_devices[idx];
    for (size_t i = 0; i < sizeof(uhci_device); i++) {
        ((uint8_t *)dev)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(uhci_rt); i++) {
        ((uint8_t *)&uhci_rts[idx])[i] = 0;
    }
    uhci_parse_device_desc(buf, dev);
    dev->address = addr;
    dev->port = (uint8_t)port;
    dev->low_speed = low_speed;
    dev->via_hub = (uint8_t)(via_hub ? 1 : 0);
    /* 構成記述子を全部読み、終端まで解析する。 */
    uhci_build_setup(setup, 0x80, 0x06, 0x0200, 0, 9);
    if (uhci_control(addr, setup, buf, 9, 1, max_packet) == 0) {
        uint16_t total = (uint16_t)(buf[2] | (buf[3] << 8));
        if (total > 9 && total <= 255) {
            uhci_build_setup(setup, 0x80, 0x06, 0x0200, 0, total);
            if (uhci_control(addr, setup, buf, total, 1,
                             max_packet) == 0) {
                (void)uhci_parse_config(buf, total, dev);
            }
        }
        /* total <= 9 のときは接続面まで読めていない
         * (if_class は 0 = 不明のまま)。 */
    }
    /* 構成を設定する (機器はこれで初めて使える)。 */
    if (dev->config_value != 0) {
        uhci_build_setup(setup, 0x00, 0x09, dev->config_value, 0, 0);
        (void)uhci_control(addr, setup, NULL, 0, 0, max_packet);
        speaker_wait_ticks(12000);
    }
    if ((dev->kind == UHCI_KIND_HID_KEYBOARD ||
         dev->kind == UHCI_KIND_HID_MOUSE) &&
        dev->hid_ep != 0) {
        /* HID: ブートプロトコル + アイドル通知なしにし、
         * 割り込み転送を張る。 */
        uhci_build_setup(setup, 0x21, 0x0B, 0, dev->if_num, 0);
        (void)uhci_control(addr, setup, NULL, 0, 0, max_packet);
        uhci_build_setup(setup, 0x21, 0x0A, 0, dev->if_num, 0);
        (void)uhci_control(addr, setup, NULL, 0, 0, max_packet);
        if (uhci_next_hid_slot < 2) {
            uhci_rts[idx].hid_slot = uhci_next_hid_slot++;
            uhci_hid_arm(idx);
        } else {
            serial_write_line("[usb] no interrupt QH slot "
                              "for HID device");
        }
    } else if (dev->kind == UHCI_KIND_STORAGE &&
               dev->bulk_in_ep != 0 && dev->bulk_out_ep != 0) {
        if (uhci_storage_probe(dev, &uhci_rts[idx]) == 0) {
            uhci_storage_idx = idx;
            serial_write("[usb] storage ready: ");
            char num[16];
            int ni = 0;
            uint32_t v = dev->storage_sectors;
            if (v == 0) {
                num[ni++] = '0';
            } else {
                char rev[12];
                int ri = 0;
                while (v > 0 && ri < 11) {
                    rev[ri++] = (char)('0' + (v % 10));
                    v /= 10;
                }
                while (ri > 0) {
                    num[ni++] = rev[--ri];
                }
            }
            num[ni] = '\0';
            serial_write(num);
            serial_write_line(" sectors");
        }
    }
    uhci_dev_count++;
    return idx;
}

int uhci_init(uint64_t hhdm_offset) {
    uint8_t bus, slot, func;
    uhci_found = 0;
    uhci_dev_count = 0;
    uhci_next_hid_slot = 0;
    for (int i = 0; i < UHCI_MAX_DEVICES; i++) {
        uhci_rts[i].hid_slot = -1;
    }
    uhci_hhdm = hhdm_offset;

    if (!pci_find_class(0x0C, 0x03, 0x00, &bus, &slot, &func)) {
        return 0; /* UHCI が無い */
    }
    uhci_iobase = (uint16_t)pci_bar32_io(bus, slot, func, 4);
    if (uhci_iobase == 0) {
        return 0;
    }
    pci_enable_bus_master(bus, slot, func);

    uhci_frame_phys = pmm_alloc_page();
    uhci_work_phys = pmm_alloc_page();
    uhci_td_phys = pmm_alloc_page();
    if (uhci_frame_phys == 0 || uhci_work_phys == 0 ||
        uhci_td_phys == 0) {
        return 0;
    }
    /* フレーム一覧の全項目を QH に向ける。 */
    uint32_t *fl = (uint32_t *)uhci_phys_to_virt(uhci_frame_phys);
    uint32_t qh_link = uhci_link_ptr(uhci_work_phys, 1);
    for (int i = 0; i < 1024; i++) {
        fl[i] = qh_link;
    }
    /* QH: 横は終端、要素は空。 */
    volatile uint32_t *qh = uhci_qh();
    qh[1] = UHCI_LINK_TERMINATE;
    /* 割り込み用 QH を横に鎖状に繋ぐ: 制御 QH →
     * 割り込み QH 0 → 割り込み QH 1 → 終端。割り込み
     * QH は機器が HID のときだけ要素を持つ。 */
    qh[0] = uhci_link_ptr(uhci_work_phys + 32, 1);
    for (int i = 0; i < 2; i++) {
        volatile uint32_t *iq = uhci_int_qh(i);
        iq[0] = i + 1 < 2 ? uhci_link_ptr(uhci_work_phys + 64, 1)
                          : UHCI_LINK_TERMINATE;
        iq[1] = UHCI_LINK_TERMINATE;
    }
    /* フレーム番号を 0 にし、一覧の場所を教える。 */
    uhci_out16(UHCI_FRNUM, 0);
    uhci_out32(UHCI_FLBASEADD, (uint32_t)(uhci_frame_phys & 0xFFFFFFFFu));
    /* スケジュールを開始する。 */
    uhci_out16(UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP);
    uhci_found = 1;

    /* 2 ポートを列挙する。 */
    (void)uhci_enumerate_port(1);
    (void)uhci_enumerate_port(2);
    return 1;
}

int uhci_present(void) {
    return uhci_found;
}

int uhci_device_count(void) {
    return uhci_dev_count;
}

const uhci_device *uhci_device_at(int i) {
    if (i < 0 || i >= uhci_dev_count) {
        return NULL;
    }
    return &uhci_devices[i];
}
