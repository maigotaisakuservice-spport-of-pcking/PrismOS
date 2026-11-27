/* PrismOS — virtio-gpu ドライバ (モダン virtio PCI、2D のみ)。
 *
 * 従来型 (レガシ) ドライバ群 (virtio_blk / virtio_net) と違い、
 * GPU は virtio 1.0 のモダンインタフェースしか持たないため、
 * PCI 機能リストから共通設定/通知/機器設定の各領域を探して
 * MMIO で操作する。描画は fb 層のシャドウ経由でバッキング
 * メモリへ入り、fb_flush の present フックで機器へ転送する。
 * 割り込みは使わずポーリングで完了を待つ (起動時・描画時とも
 * シェル文脈からのみ呼ばれる)。
 */
#include "virtio_gpu.h"

#include "fb.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "virtio_blk.h" /* virtio_queue_layout / virtio_desc を再利用 */

/* PCI 機器 ID (virtio GPU)。 */
#define VG_VENDOR 0x1AF4
#define VG_DEVICE 0x1050

/* PCI 機能レジスタの種別 (virtio)。 */
#define VG_CAP_COMMON 1
#define VG_CAP_NOTIFY 2
#define VG_CAP_ISR 3
#define VG_CAP_DEVICE 4

/* 共通設定のオフセット。 */
#define VG_C_DFSELECT 0
#define VG_C_DF 4
#define VG_C_GFSELECT 8
#define VG_C_GF 12
#define VG_C_NUMQ 18
#define VG_C_STATUS 20
#define VG_C_Q_SELECT 22
#define VG_C_Q_SIZE 24
#define VG_C_Q_ENABLE 28
#define VG_C_Q_NOFF 30
#define VG_C_Q_DESCLO 32
#define VG_C_Q_DESCHI 36
#define VG_C_Q_AVAILLO 40
#define VG_C_Q_AVAILHI 44
#define VG_C_Q_USEDLO 48
#define VG_C_Q_USEDHI 52

/* 機器状態ビット。 */
#define VG_ST_ACK 0x01
#define VG_ST_DRIVER 0x02
#define VG_ST_DRIVER_OK 0x04
#define VG_ST_FEATURES_OK 0x08
#define VG_ST_FAILED 0x80

/* 記述子フラグ。 */
#define VG_DESC_F_NEXT 1
#define VG_DESC_F_WRITE 2

/* GPU 制御コマンドと応答。 */
#define VG_CMD_GET_DISPLAY_INFO 0x0100
#define VG_CMD_RESOURCE_CREATE_2D 0x0101
#define VG_CMD_RESOURCE_UNREF 0x0102
#define VG_CMD_SET_SCANOUT 0x0103
#define VG_CMD_RESOURCE_FLUSH 0x0104
#define VG_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VG_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VG_RESP_OK_NODATA 0x1100
#define VG_RESP_OK_DISPLAY_INFO 0x1101

/* ピクセル形式: B8G8R8A8 (メモリ順 B,G,R,A)。 */
#define VG_FORMAT_B8G8R8A8 1

#define VG_RESOURCE_ID 1
#define VG_SCANOUT_ID 0
#define VG_HDR_LEN 24 /* 制御ヘッダの大きさ */

static uint64_t vg_hhdm;
static int vg_active;
static uint8_t vg_bus, vg_slot, vg_func;

/* MMIO 領域 (仮想アドレス)。 */
static uint8_t *vg_common;
static uint8_t *vg_notify_base;
static uint32_t vg_notify_mult;
static uint16_t vg_notify_off; /* 制御キューの通知オフセット */

/* 制御キュー (キュー 0)。 */
static uint64_t vg_q_phys;
static uint64_t vg_q_pages;
static uint16_t vg_qsize;
static uint32_t vg_avail_off;
static uint32_t vg_used_off;
static uint16_t vg_avail_idx;
static uint16_t vg_used_idx;

/* コマンド/応答バッファ (各 1 ページ)。 */
static uint64_t vg_cmd_phys;
static uint64_t vg_resp_phys;

/* 表示状態。 */
static uint32_t vg_w;
static uint32_t vg_h;
static uint64_t vg_backing_phys; /* リソースの裏付けメモリ */
static uint64_t vg_backing_pages;

/* ---- 小物 ---- */

static void *vg_phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(phys + vg_hhdm);
}

static void vg_barrier(void) {
    __asm__ volatile("" ::: "memory");
}

static uint32_t vg_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void vg_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void vg_wr64(uint8_t *p, uint64_t v) {
    vg_wr32(p, (uint32_t)(v & 0xFFFFFFFFu));
    vg_wr32(p + 4, (uint32_t)(v >> 32));
}

/* 共通設定への型付きアクセス (MMIO、揮発)。 */
static uint8_t vg_c8(uint32_t off) {
    return *(volatile uint8_t *)(vg_common + off);
}
static void vg_c8w(uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(vg_common + off) = v;
}
static uint16_t vg_c16(uint32_t off) {
    return *(volatile uint16_t *)(vg_common + off);
}
static void vg_c16w(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(vg_common + off) = v;
}
static uint32_t vg_c32(uint32_t off) {
    return *(volatile uint32_t *)(vg_common + off);
}
static void vg_c32w(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(vg_common + off) = v;
}

/* ---- PCI 機能リスト ---- */

/* BAR のメモリ基底 (64 ビット BAR は上位も読む)。I/O BAR や
 * 無効なものは 0。 */
static uint64_t vg_bar_base(int bar) {
    uint32_t lo = pci_config_read32(vg_bus, vg_slot, vg_func,
                                    (uint8_t)(0x10 + 4 * bar));
    if ((lo & 1u) != 0) {
        return 0;
    }
    uint64_t base = (uint64_t)(lo & ~0xFu);
    if ((lo & 0x6u) == 0x4u && bar < 5) {
        uint32_t hi = pci_config_read32(
            vg_bus, vg_slot, vg_func, (uint8_t)(0x10 + 4 * (bar + 1)));
        base |= (uint64_t)hi << 32;
    }
    return base;
}

/* 設定空間の 1 バイトを読む (32 ビット読みから切り出す)。 */
static uint8_t vg_cfg8(uint8_t off) {
    uint32_t v = pci_config_read32(vg_bus, vg_slot, vg_func,
                                   (uint8_t)(off & ~3u));
    return (uint8_t)(v >> ((off & 3u) * 8));
}

/* BAR 内の領域を HHDM へ載せて仮想アドレスを返す。 */
static uint8_t *vg_map_region(uint64_t bar_base, uint32_t offset,
                              uint32_t length) {
    uint64_t phys = bar_base + offset;
    if (paging_map_mmio(phys, length, vg_hhdm) != 0) {
        return NULL;
    }
    return (uint8_t *)(uintptr_t)(phys + vg_hhdm);
}

/* 機能リストを歩いて共通設定と通知領域を確定する。
 * 成功したら 0。 */
static int vg_find_caps(void) {
    uint16_t status = pci_config_read16(vg_bus, vg_slot, vg_func, 0x06);
    if ((status & 0x10u) == 0) {
        return -1; /* 機能リスト無し */
    }
    uint8_t ptr = vg_cfg8(0x34) & ~3u;
    int have_common = 0;
    int have_notify = 0;
    while (ptr >= 0x40) {
        uint8_t id = vg_cfg8(ptr);
        uint8_t next = vg_cfg8((uint8_t)(ptr + 1));
        if (id == 0x09) { /* ベンダ固有 = virtio 機能 */
            uint8_t type = vg_cfg8((uint8_t)(ptr + 3));
            uint8_t bar = vg_cfg8((uint8_t)(ptr + 4));
            uint32_t off = pci_config_read32(vg_bus, vg_slot, vg_func,
                                             (uint8_t)(ptr + 8));
            uint32_t len = pci_config_read32(vg_bus, vg_slot, vg_func,
                                             (uint8_t)(ptr + 12));
            uint64_t base = (bar < 6) ? vg_bar_base(bar) : 0;
            if (base != 0 && len > 0) {
                if (type == VG_CAP_COMMON && !have_common) {
                    vg_common = vg_map_region(base, off, len);
                    have_common = vg_common != NULL;
                } else if (type == VG_CAP_NOTIFY && !have_notify) {
                    vg_notify_base = vg_map_region(base, off, len);
                    vg_notify_mult = pci_config_read32(
                        vg_bus, vg_slot, vg_func, (uint8_t)(ptr + 16));
                    have_notify = vg_notify_base != NULL;
                }
            }
        }
        if (next == 0) {
            break;
        }
        ptr = next & ~3u;
    }
    return (have_common && have_notify) ? 0 : -1;
}

/* ---- 制御キュー ---- */

/* キュー 0 (制御) を用意する。キューメモリは初回だけ確保し、
 * 復帰時は同じメモリを初期化し直して機器へ教え直す。 */
static int vg_queue_setup(void) {
    vg_c16w(VG_C_Q_SELECT, 0);
    vg_qsize = vg_c16(VG_C_Q_SIZE);
    if (vg_qsize == 0) {
        return -1;
    }
    uint32_t desc_bytes, total;
    virtio_queue_layout(vg_qsize, &desc_bytes, &vg_avail_off,
                        &vg_used_off, &total);
    if (vg_q_phys == 0) {
        vg_q_pages = (total + 4095) / 4096;
        vg_q_phys = pmm_alloc_pages((size_t)vg_q_pages);
        if (vg_q_phys == 0) {
            return -1;
        }
    }
    memset(vg_phys_to_virt(vg_q_phys), 0, (size_t)vg_q_pages * 4096);
    vg_avail_idx = 0;
    vg_used_idx = 0;
    vg_c32w(VG_C_Q_DESCLO, (uint32_t)(vg_q_phys & 0xFFFFFFFFu));
    vg_c32w(VG_C_Q_DESCHI, (uint32_t)(vg_q_phys >> 32));
    uint64_t avail_phys = vg_q_phys + vg_avail_off;
    vg_c32w(VG_C_Q_AVAILLO, (uint32_t)(avail_phys & 0xFFFFFFFFu));
    vg_c32w(VG_C_Q_AVAILHI, (uint32_t)(avail_phys >> 32));
    uint64_t used_phys = vg_q_phys + vg_used_off;
    vg_c32w(VG_C_Q_USEDLO, (uint32_t)(used_phys & 0xFFFFFFFFu));
    vg_c32w(VG_C_Q_USEDHI, (uint32_t)(used_phys >> 32));
    vg_notify_off = vg_c16(VG_C_Q_NOFF);
    vg_c16w(VG_C_Q_ENABLE, 1);
    return 0;
}

/* コマンドを 1 つ実行する。cmd は vg_cmd バッファに用意済みの
 * 前提。応答ヘッダの種別が expect と一致したら 0。 */
static int vg_exec(uint32_t cmd_len, uint32_t resp_len,
                   uint32_t expect) {
    uint8_t *q = (uint8_t *)vg_phys_to_virt(vg_q_phys);
    uint8_t *resp = (uint8_t *)vg_phys_to_virt(vg_resp_phys);
    memset(resp, 0, resp_len);

    /* 記述子 0: コマンド (機器が読む)、1: 応答 (機器が書く)。 */
    virtio_desc(q + 0 * 16, vg_cmd_phys, cmd_len, VG_DESC_F_NEXT, 1);
    virtio_desc(q + 1 * 16, vg_resp_phys, resp_len, VG_DESC_F_WRITE,
                0);

    /* 利用可能リングへ先頭記述子を出す。 */
    uint16_t slot = (uint16_t)(vg_avail_idx % vg_qsize);
    uint32_t ring_at = vg_avail_off + 4 + (uint32_t)slot * 2;
    q[ring_at] = 0;
    q[ring_at + 1] = 0;
    vg_barrier();
    vg_avail_idx = (uint16_t)(vg_avail_idx + 1);
    q[vg_avail_off + 2] = (uint8_t)(vg_avail_idx & 0xFF);
    q[vg_avail_off + 3] = (uint8_t)(vg_avail_idx >> 8);
    vg_barrier();

    /* 通知して完了 (使用済み idx の進行) を待つ。 */
    uint16_t used_before = (uint16_t)(q[vg_used_off + 2] |
                                      (q[vg_used_off + 3] << 8));
    *(volatile uint16_t *)(vg_notify_base +
                           (uint32_t)vg_notify_off * vg_notify_mult) =
        0;
    for (int i = 0; i < 100000000; i++) {
        uint16_t used_now = (uint16_t)(q[vg_used_off + 2] |
                                       (q[vg_used_off + 3] << 8));
        if (used_now != used_before) {
            vg_used_idx = used_now;
            return vg_rd32(resp) == expect ? 0 : -1;
        }
        __asm__ volatile("pause");
    }
    return -1; /* 時間切れ */
}

/* コマンドヘッダだけ置く (残りは呼び出し側が続ける)。 */
static uint8_t *vg_cmd_begin(uint32_t type) {
    uint8_t *cmd = (uint8_t *)vg_phys_to_virt(vg_cmd_phys);
    memset(cmd, 0, 4096);
    vg_wr32(cmd + 0, type);
    return cmd;
}

/* ---- 機器セットアップ ---- */

/* 状態遷移 (リセット→機能交渉→キュー→DRIVER_OK) を通す。
 * 初期化と S3 復帰の共通処理。 */
static int vg_device_setup(void) {
    vg_c8w(VG_C_STATUS, 0);
    for (int i = 0; i < 1000000; i++) {
        if (vg_c8(VG_C_STATUS) == 0) {
            break;
        }
        __asm__ volatile("pause");
    }
    if (vg_c8(VG_C_STATUS) != 0) {
        return -1;
    }
    vg_c8w(VG_C_STATUS, VG_ST_ACK);
    vg_c8w(VG_C_STATUS, VG_ST_ACK | VG_ST_DRIVER);

    /* 機能: VERSION_1 (ビット 32) だけを必須とし、GPU の
     * 任意機能 (3D 等) は使わない。 */
    vg_c32w(VG_C_DFSELECT, 1);
    uint32_t feats_hi = vg_c32(VG_C_DF);
    if ((feats_hi & 1u) == 0) {
        vg_c8w(VG_C_STATUS, VG_ST_FAILED);
        return -1;
    }
    vg_c32w(VG_C_GFSELECT, 0);
    vg_c32w(VG_C_GF, 0);
    vg_c32w(VG_C_GFSELECT, 1);
    vg_c32w(VG_C_GF, 1);
    vg_c8w(VG_C_STATUS, VG_ST_ACK | VG_ST_DRIVER | VG_ST_FEATURES_OK);
    if ((vg_c8(VG_C_STATUS) & VG_ST_FEATURES_OK) == 0) {
        return -1; /* 機器が機能を受け付けなかった */
    }

    if (vg_queue_setup() != 0) {
        vg_c8w(VG_C_STATUS, VG_ST_FAILED);
        return -1;
    }
    vg_c8w(VG_C_STATUS, VG_ST_ACK | VG_ST_DRIVER | VG_ST_FEATURES_OK |
                            VG_ST_DRIVER_OK);
    return 0;
}

/* 矩形フィールド (x, y, w, h) を書く。 */
static void vg_rect(uint8_t *p, uint32_t x, uint32_t y, uint32_t w,
                    uint32_t h) {
    vg_wr32(p + 0, x);
    vg_wr32(p + 4, y);
    vg_wr32(p + 8, w);
    vg_wr32(p + 12, h);
}

/* リソース 1 を作り、バッキングを付けてスキャンアウト 0 へ
 * 繋ぐ。vg_w / vg_h と vg_backing_phys が前提。 */
static int vg_scanout_setup(void) {
    uint8_t *cmd = vg_cmd_begin(VG_CMD_RESOURCE_CREATE_2D);
    vg_wr32(cmd + 24, VG_RESOURCE_ID);
    vg_wr32(cmd + 28, VG_FORMAT_B8G8R8A8);
    vg_wr32(cmd + 32, vg_w);
    vg_wr32(cmd + 36, vg_h);
    if (vg_exec(40, VG_HDR_LEN, VG_RESP_OK_NODATA) != 0) {
        return -1;
    }

    cmd = vg_cmd_begin(VG_CMD_RESOURCE_ATTACH_BACKING);
    vg_wr32(cmd + 24, VG_RESOURCE_ID);
    vg_wr32(cmd + 28, 1); /* 項目数 */
    vg_wr64(cmd + 32, vg_backing_phys);
    vg_wr32(cmd + 40, vg_w * vg_h * 4);
    vg_wr32(cmd + 44, 0);
    if (vg_exec(48, VG_HDR_LEN, VG_RESP_OK_NODATA) != 0) {
        return -1;
    }

    cmd = vg_cmd_begin(VG_CMD_SET_SCANOUT);
    vg_rect(cmd + 24, 0, 0, vg_w, vg_h);
    vg_wr32(cmd + 40, VG_SCANOUT_ID);
    vg_wr32(cmd + 44, VG_RESOURCE_ID);
    if (vg_exec(48, VG_HDR_LEN, VG_RESP_OK_NODATA) != 0) {
        return -1;
    }
    return 0;
}

/* バッキングメモリを大きさに合わせて確保する (既存は解放)。 */
static int vg_backing_alloc(uint32_t w, uint32_t h) {
    uint64_t bytes = (uint64_t)w * h * 4;
    uint64_t pages = (bytes + 4095) / 4096;
    if (vg_backing_phys != 0) {
        pmm_free_pages(vg_backing_phys, (size_t)vg_backing_pages);
        vg_backing_phys = 0;
        vg_backing_pages = 0;
    }
    uint64_t phys = pmm_alloc_pages((size_t)pages);
    if (phys == 0) {
        return -1;
    }
    memset(vg_phys_to_virt(phys), 0, (size_t)bytes);
    vg_backing_phys = phys;
    vg_backing_pages = pages;
    return 0;
}

/* ---- 公開 API ---- */

int virtio_gpu_init(uint64_t hhdm_offset) {
    vg_hhdm = hhdm_offset;
    vg_active = 0;
    if (!pci_find_device(VG_VENDOR, VG_DEVICE, &vg_bus, &vg_slot,
                         &vg_func, NULL)) {
        return 0; /* virtio-gpu が無い */
    }
    if (vg_find_caps() != 0) {
        return 0;
    }
    pci_enable_bus_master(vg_bus, vg_slot, vg_func);

    vg_cmd_phys = pmm_alloc_page();
    vg_resp_phys = pmm_alloc_page();
    if (vg_cmd_phys == 0 || vg_resp_phys == 0) {
        return 0;
    }
    if (vg_device_setup() != 0) {
        return 0;
    }

    /* 表示情報を読み、スキャンアウト 0 のモードを採る。
     * 無効な場合は既定の 1280x800 を要求モードにする。 */
    vg_cmd_begin(VG_CMD_GET_DISPLAY_INFO);
    uint32_t w = 1280;
    uint32_t h = 800;
    if (vg_exec(VG_HDR_LEN, 408, VG_RESP_OK_DISPLAY_INFO) == 0) {
        uint8_t *resp = (uint8_t *)vg_phys_to_virt(vg_resp_phys);
        uint32_t pw = vg_rd32(resp + 24 + 8);
        uint32_t ph = vg_rd32(resp + 24 + 12);
        uint32_t enabled = vg_rd32(resp + 24 + 16);
        if (enabled != 0 && pw >= 320 && pw <= 4096 && ph >= 200 &&
            ph <= 4096) {
            w = pw;
            h = ph;
        }
    }
    vg_w = w;
    vg_h = h;
    if (vg_backing_alloc(w, h) != 0) {
        return 0;
    }
    if (vg_scanout_setup() != 0) {
        return 0;
    }
    vg_active = 1;
    serial_write_line("[boot] virtio-gpu: display ready");
    virtio_gpu_present();
    return 1;
}

int virtio_gpu_active(void) {
    return vg_active;
}

void *virtio_gpu_framebuffer(void) {
    return vg_phys_to_virt(vg_backing_phys);
}

uint32_t virtio_gpu_width(void) {
    return vg_w;
}

uint32_t virtio_gpu_height(void) {
    return vg_h;
}

uint64_t virtio_gpu_pitch(void) {
    return (uint64_t)vg_w * 4;
}

void virtio_gpu_present(void) {
    if (!vg_active) {
        return;
    }
    uint8_t *cmd = vg_cmd_begin(VG_CMD_TRANSFER_TO_HOST_2D);
    vg_rect(cmd + 24, 0, 0, vg_w, vg_h);
    vg_wr64(cmd + 40, 0); /* 転送オフセット */
    vg_wr32(cmd + 48, VG_RESOURCE_ID);
    if (vg_exec(56, VG_HDR_LEN, VG_RESP_OK_NODATA) != 0) {
        return;
    }
    cmd = vg_cmd_begin(VG_CMD_RESOURCE_FLUSH);
    vg_rect(cmd + 24, 0, 0, vg_w, vg_h);
    vg_wr32(cmd + 40, VG_RESOURCE_ID);
    (void)vg_exec(48, VG_HDR_LEN, VG_RESP_OK_NODATA);
}

int virtio_gpu_set_mode(uint32_t width, uint32_t height) {
    if (!vg_active) {
        return -1;
    }
    if (width == vg_w && height == vg_h) {
        virtio_gpu_present();
        return 0;
    }
    if (width < 320 || width > 4096 || height < 200 ||
        height > 4096) {
        return -1;
    }
    /* 先に新しいバッキングを確保する (失敗時は旧表示を保つ)。 */
    uint64_t old_phys = vg_backing_phys;
    uint64_t old_pages = vg_backing_pages;
    uint64_t bytes = (uint64_t)width * height * 4;
    uint64_t pages = (bytes + 4095) / 4096;
    uint64_t phys = pmm_alloc_pages((size_t)pages);
    if (phys == 0) {
        return -1;
    }
    memset(vg_phys_to_virt(phys), 0, (size_t)bytes);

    /* 旧スキャンアウトを止めて旧リソースを捨てる。 */
    uint8_t *cmd = vg_cmd_begin(VG_CMD_SET_SCANOUT);
    vg_rect(cmd + 24, 0, 0, 0, 0);
    vg_wr32(cmd + 40, VG_SCANOUT_ID);
    vg_wr32(cmd + 44, 0);
    (void)vg_exec(48, VG_HDR_LEN, VG_RESP_OK_NODATA);
    cmd = vg_cmd_begin(VG_CMD_RESOURCE_UNREF);
    vg_wr32(cmd + 24, VG_RESOURCE_ID);
    (void)vg_exec(32, VG_HDR_LEN, VG_RESP_OK_NODATA);

    vg_backing_phys = phys;
    vg_backing_pages = pages;
    vg_w = width;
    vg_h = height;
    if (old_phys != 0) {
        pmm_free_pages(old_phys, (size_t)old_pages);
    }
    if (vg_scanout_setup() != 0) {
        return -1;
    }
    /* fb 層を新しい形状で初期化し直す (32bpp BGRA)。 */
    fb_init(virtio_gpu_framebuffer(), vg_w, vg_h, virtio_gpu_pitch(),
            32, 8, 16, 8, 8, 8, 0);
    virtio_gpu_present();
    return 0;
}

int virtio_gpu_resume(void) {
    if (!vg_active) {
        return -1;
    }
    /* PCI 設定は呼び出し側 (acpi) が書き戻し済みの前提。
     * 機器はリセットから作り直し、同じバッキング・寸法で
     * リソースとスキャンアウトを張り直す。 */
    if (vg_device_setup() != 0) {
        return -1;
    }
    if (vg_scanout_setup() != 0) {
        return -1;
    }
    virtio_gpu_present();
    return 0;
}
