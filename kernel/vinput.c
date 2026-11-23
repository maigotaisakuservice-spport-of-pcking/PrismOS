/* PrismOS — virtio-tablet (モダン virtio PCI) の実装。 */

#include "vinput.h"

#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

/* virtio PCI ケーパビリティの cfg_type */
#define VC_COMMON 1
#define VC_NOTIFY 2
#define VC_ISR 3
#define VC_DEVICE 4

/* 共通設定 (virtio_pci_common_cfg) のオフセット */
#define CC_DEV_FEAT_SEL 0x00
#define CC_DEV_FEAT 0x04
#define CC_DRV_FEAT_SEL 0x08
#define CC_DRV_FEAT 0x0C
#define CC_NUM_QUEUES 0x12
#define CC_STATUS 0x14
#define CC_Q_SELECT 0x16
#define CC_Q_SIZE 0x18
#define CC_Q_ENABLE 0x1C
#define CC_Q_NOTIFY_OFF 0x1E
#define CC_Q_DESC 0x20
#define CC_Q_DRIVER 0x28
#define CC_Q_DEVICE 0x30

#define VINPUT_BUFS 16
#define EV_KEY 1
#define EV_ABS 3
#define ABS_X 0
#define ABS_Y 1
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

static uint64_t vi_hhdm;
static int vi_found;
static volatile uint8_t *vi_common;
static volatile uint8_t *vi_device;
static volatile uint8_t *vi_notify_base;
static uint32_t vi_notify_mult;
static uint16_t vi_notify_off;
static uint16_t vi_qsize;
static uint64_t vi_desc_phys;
static uint64_t vi_avail_phys;
static uint64_t vi_used_phys;
static uint64_t vi_buf_phys;
static uint16_t vi_avail_idx;
static uint16_t vi_used_seen;
static int32_t vi_raw_x;
static int32_t vi_raw_y;
static int32_t vi_max_x = 32767;
static int32_t vi_max_y = 32767;
static int vi_buttons;
static int vi_last_px;
static int vi_last_py;
static int vi_last_buttons;
static int vi_have_pos;

static uint16_t cc16(uint32_t off) {
    return *(volatile uint16_t *)(vi_common + off);
}
static uint32_t cc32(uint32_t off) {
    return *(volatile uint32_t *)(vi_common + off);
}
static void cc8w(uint32_t off, uint8_t v) { vi_common[off] = v; }
static void cc16w(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(vi_common + off) = v;
}
static void cc32w(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(vi_common + off) = v;
}
static void cc64w(uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(vi_common + off) = v;
}

static void *phys_virt(uint64_t phys) {
    return (void *)(uintptr_t)(phys + vi_hhdm);
}

/* BAR を (必要なら) マップして仮想アドレスを返す。
 * 既にマップ済みの物理範囲は呼び出し側で使い回す。 */
static volatile uint8_t *bar_virt(uint8_t bus, uint8_t slot,
                                  uint8_t func, int bar) {
    uint32_t lo = pci_config_read32(bus, slot, func,
                                    0x10 + bar * 4);
    uint64_t phys = lo & 0xFFFFFFF0u;
    if ((lo & 0x6) == 0x4) {
        uint32_t hi = pci_config_read32(bus, slot, func,
                                        0x10 + (bar + 1) * 4);
        phys |= (uint64_t)hi << 32;
    }
    if (phys == 0) {
        return NULL;
    }
    if (paging_map_mmio(phys, 0x4000, vi_hhdm) != 0) {
        return NULL;
    }
    return (volatile uint8_t *)(uintptr_t)(phys + vi_hhdm);
}

/* virtio ケーパビリティを 1 つ探す。見つかれば BAR 内の
 * 仮想ポインタと (通知用に) 乗数を返す。 */
static volatile uint8_t *find_cap(uint8_t bus, uint8_t slot,
                                  uint8_t func, uint8_t want_type,
                                  uint32_t *mult_out) {
    uint8_t cap_ptr = (uint8_t)(pci_config_read32(bus, slot, func,
                                                  0x34) & 0xFF);
    while (cap_ptr != 0) {
        uint32_t w0 = pci_config_read32(bus, slot, func, cap_ptr);
        uint8_t id = (uint8_t)(w0 & 0xFF);
        uint8_t next = (uint8_t)((w0 >> 8) & 0xFF);
        if (id == 0x09) { /* ベンダ固有 = virtio */
            uint8_t type = (uint8_t)((w0 >> 24) & 0xFF);
            if (type == want_type) {
                uint32_t w1 = pci_config_read32(bus, slot, func,
                                                cap_ptr + 4);
                uint32_t w2 = pci_config_read32(bus, slot, func,
                                                cap_ptr + 8);
                uint8_t bar = (uint8_t)(w1 & 0xFF);
                volatile uint8_t *base = bar_virt(bus, slot, func,
                                                  (int)bar);
                if (base == NULL) {
                    return NULL;
                }
                if (mult_out != NULL && want_type == VC_NOTIFY) {
                    /* 乗数はケーパビリティの +16 にある
                     * (+12 は長さフィールド)。 */
                    *mult_out = pci_config_read32(bus, slot,
                                                  func,
                                                  cap_ptr + 16);
                }
                return base + w2;
            }
        }
        cap_ptr = next;
    }
    return NULL;
}

static int32_t config_abs_max(uint8_t axis) {
    /* virtio_input_config: select +0, subsel +1, size +2、
     * union は +8 から。ABS_INFO (select=0x12) の
     * abs.max は +12 (s32)。 */
    vi_device[0] = 0x12;
    vi_device[1] = axis;
    uint32_t v = *(volatile uint32_t *)(vi_device + 12);
    return (int32_t)v;
}

static void post_buffer(uint16_t desc_idx) {
    uint8_t *desc = (uint8_t *)phys_virt(vi_desc_phys) +
                    (size_t)desc_idx * 16;
    uint64_t addr = vi_buf_phys + (uint64_t)desc_idx * 8;
    for (int i = 0; i < 8; i++) {
        desc[i] = (uint8_t)(addr >> (8 * i));
    }
    desc[8] = 8;
    desc[9] = 0;
    desc[10] = 0;
    desc[11] = 0;
    desc[12] = 2; /* WRITE */
    desc[13] = 0;
    desc[14] = 0;
    desc[15] = 0;
    volatile uint16_t *avail =
        (volatile uint16_t *)phys_virt(vi_avail_phys);
    avail[2 + (vi_avail_idx % vi_qsize)] = desc_idx;
    __asm__ volatile("mfence" ::: "memory");
    vi_avail_idx++;
    avail[1] = vi_avail_idx;
    /* 通知: notify_base + notify_off * mult へキュー番号。 */
    *(volatile uint16_t *)(vi_notify_base +
                           (size_t)vi_notify_off *
                               vi_notify_mult) = 0;
}

int vinput_init(uint64_t hhdm_offset) {
    uint8_t bus, slot, func;
    vi_found = 0;
    vi_hhdm = hhdm_offset;
    if (!pci_find_device(0x1AF4, 0x1052, &bus, &slot, &func,
                         NULL)) {
        return 0;
    }
    pci_enable_bus_master(bus, slot, func);
    vi_common = find_cap(bus, slot, func, VC_COMMON, NULL);
    vi_device = find_cap(bus, slot, func, VC_DEVICE, NULL);
    vi_notify_base = find_cap(bus, slot, func, VC_NOTIFY,
                              &vi_notify_mult);
    if (vi_common == NULL || vi_device == NULL ||
        vi_notify_base == NULL) {
        serial_write_line("[vinput] missing virtio caps");
        return 0;
    }
    /* リセットしてから初期化シーケンスを進める。 */
    cc8w(CC_STATUS, 0);
    for (int i = 0; i < 100000; i++) {
        if (vi_common[CC_STATUS] == 0) {
            break;
        }
        __asm__ volatile("pause");
    }
    cc8w(CC_STATUS, 1); /* ACK */
    cc8w(CC_STATUS, 1 | 2); /* DRIVER */
    cc32w(CC_DEV_FEAT_SEL, 0);
    (void)cc32(CC_DEV_FEAT);
    cc32w(CC_DEV_FEAT_SEL, 1);
    (void)cc32(CC_DEV_FEAT);
    cc32w(CC_DRV_FEAT_SEL, 0);
    cc32w(CC_DRV_FEAT, 0);
    cc32w(CC_DRV_FEAT_SEL, 1);
    cc32w(CC_DRV_FEAT, 0);
    cc8w(CC_STATUS, 1 | 2 | 8); /* FEATURES_OK */
    if ((vi_common[CC_STATUS] & 8) == 0) {
        serial_write_line("[vinput] features not accepted");
        return 0;
    }

    int32_t mx = config_abs_max(ABS_X);
    int32_t my = config_abs_max(ABS_Y);
    if (mx > 0) {
        vi_max_x = mx;
    }
    if (my > 0) {
        vi_max_y = my;
    }

    /* イベントキュー (0) を用意する。 */
    cc16w(CC_Q_SELECT, 0);
    vi_qsize = cc16(CC_Q_SIZE);
    if (vi_qsize == 0 || vi_qsize > 256) {
        return 0;
    }
    vi_desc_phys = pmm_alloc_pages(2);
    vi_avail_phys = pmm_alloc_page();
    vi_used_phys = pmm_alloc_page();
    vi_buf_phys = pmm_alloc_page();
    if (!vi_desc_phys || !vi_avail_phys || !vi_used_phys ||
        !vi_buf_phys) {
        return 0;
    }
    memset(phys_virt(vi_desc_phys), 0, 8192);
    memset(phys_virt(vi_avail_phys), 0, 4096);
    memset(phys_virt(vi_used_phys), 0, 4096);
    cc64w(CC_Q_DESC, vi_desc_phys);
    cc64w(CC_Q_DRIVER, vi_avail_phys);
    cc64w(CC_Q_DEVICE, vi_used_phys);
    vi_notify_off = cc16(CC_Q_NOTIFY_OFF);
    cc16w(CC_Q_ENABLE, 1);
    cc8w(CC_STATUS, 1 | 2 | 8 | 4); /* DRIVER_OK */
    vi_avail_idx = 0;
    vi_used_seen = 0;
    int n = vi_qsize < VINPUT_BUFS ? vi_qsize : VINPUT_BUFS;
    for (int i = 0; i < n; i++) {
        post_buffer((uint16_t)i);
    }
    vi_found = 1;
    serial_write_line("[boot] input: virtio tablet ready");
    return 1;
}

int vinput_present(void) {
    return vi_found;
}

static void drain_events(void) {
    volatile uint16_t *used =
        (volatile uint16_t *)phys_virt(vi_used_phys);
    uint16_t used_idx = used[1];
    while (vi_used_seen != used_idx) {
        const volatile uint32_t *elem =
            (const volatile uint32_t *)(phys_virt(vi_used_phys) +
                                        4 +
                                        (size_t)(vi_used_seen %
                                                 vi_qsize) * 8);
        uint16_t desc_idx = (uint16_t)elem[0];
        const uint8_t *ev = (const uint8_t *)phys_virt(
            vi_buf_phys + (uint64_t)desc_idx * 8);
        uint16_t type = (uint16_t)(ev[0] | (ev[1] << 8));
        uint16_t code = (uint16_t)(ev[2] | (ev[3] << 8));
        int32_t value = (int32_t)((uint32_t)ev[4] |
                                  ((uint32_t)ev[5] << 8) |
                                  ((uint32_t)ev[6] << 16) |
                                  ((uint32_t)ev[7] << 24));
        if (type == EV_ABS && code == ABS_X) {
            vi_raw_x = value;
            vi_have_pos = 1;
        } else if (type == EV_ABS && code == ABS_Y) {
            vi_raw_y = value;
            vi_have_pos = 1;
        } else if (type == EV_KEY) {
            int bit = -1;
            if (code == BTN_LEFT) {
                bit = 1;
            } else if (code == BTN_RIGHT) {
                bit = 2;
            } else if (code == BTN_MIDDLE) {
                bit = 4;
            }
            if (bit > 0) {
                if (value) {
                    vi_buttons |= bit;
                } else {
                    vi_buttons &= ~bit;
                }
            }
        }
        vi_used_seen++;
        post_buffer(desc_idx);
    }
}

int vinput_poll(int *x, int *y, int *buttons, int width,
                int height) {
    if (!vi_found) {
        return 0;
    }
    drain_events();
    if (!vi_have_pos && vi_buttons == vi_last_buttons) {
        return 0;
    }
    int px = 0;
    int py = 0;
    if (vi_max_x > 0 && width > 0) {
        px = (int)((int64_t)vi_raw_x * (width - 1) / vi_max_x);
    }
    if (vi_max_y > 0 && height > 0) {
        py = (int)((int64_t)vi_raw_y * (height - 1) / vi_max_y);
    }
    if (px == vi_last_px && py == vi_last_py &&
        vi_buttons == vi_last_buttons) {
        return 0;
    }
    vi_last_px = px;
    vi_last_py = py;
    vi_last_buttons = vi_buttons;
    *x = px;
    *y = py;
    *buttons = vi_buttons;
    return 1;
}

void vinput_state(int *raw_x, int *raw_y, int *buttons,
                  int *max_x, int *max_y) {
    if (vi_found) {
        drain_events();
    }
    *raw_x = vi_raw_x;
    *raw_y = vi_raw_y;
    *buttons = vi_buttons;
    *max_x = vi_max_x;
    *max_y = vi_max_y;
}
