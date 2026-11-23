/* PrismOS — ARM 用 virtio-mmio トランスポート (レガシー)
 *
 * レジスタ配置は virtio 仕様の mmio レガシー版に従う。
 * キューのレイアウトは desc → avail → (4096 境界) → used の
 * 伝統的な形 (vring) で、サイズ計算も仕様の式どおり。
 */
#include "virtio.h"

#include "timer.h"
#include "uart.h"

#ifdef VIRTIO_DEBUG
static void uart_dbg_hex(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    int i;
    arm_uart_write("0x");
    for (i = 28; i >= 0; i -= 4) {
        arm_uart_putc(digits[(v >> i) & 0xfu]);
    }
}
#endif

/* ---- レジスタオフセット ---- */
#define REG_MAGIC 0x000u
#define REG_VERSION 0x004u
#define REG_DEVICE_ID 0x008u
#define REG_DEVICE_FEATURES 0x010u
#define REG_DEVICE_FEATURES_SEL 0x014u
#define REG_DRIVER_FEATURES 0x020u
#define REG_DRIVER_FEATURES_SEL 0x024u
#define REG_GUEST_PAGE_SIZE 0x028u
#define REG_QUEUE_SEL 0x030u
#define REG_QUEUE_NUM_MAX 0x034u
#define REG_QUEUE_NUM 0x038u
#define REG_QUEUE_ALIGN 0x03cu
#define REG_QUEUE_PFN 0x040u
#define REG_QUEUE_NOTIFY 0x050u
#define REG_INTERRUPT_STATUS 0x060u
#define REG_INTERRUPT_ACK 0x064u
#define REG_STATUS 0x070u
#define REG_CONFIG 0x100u

#define VIRTIO_MAGIC 0x74726976u /* 'virt' */
#define STATUS_ACKNOWLEDGE 1u
#define STATUS_DRIVER 2u
#define STATUS_DRIVER_OK 4u
#define STATUS_FAILED 128u

#define DESC_F_NEXT 1u
#define DESC_F_WRITE 2u

#define MMIO_BASE 0x0a000000u
#define MMIO_STRIDE 0x200u /* QEMU virt の実配置 (DTB で確認) */
#define MMIO_SLOTS 32 /* QEMU virt は 32 トランスポート。機器は
                       * 末尾のスロットから付くので全部走査する */

/* キュー 1 本分の静的メモリ。desc+avail が 1 ページ、
 * used が次のページ境界から。余裕を持って 3 ページ取る。 */
#define VQUEUE_MEM_SIZE 12288u
static uint8_t queue_mem[VIRTIO_MAX_QUEUES * 2][VQUEUE_MEM_SIZE]
    __attribute__((aligned(4096)));
static int queue_mem_used[VIRTIO_MAX_QUEUES * 2];

static uint8_t *queue_mem_alloc(void) {
    int i;
    for (i = 0; i < VIRTIO_MAX_QUEUES * 2; i++) {
        if (!queue_mem_used[i]) {
            queue_mem_used[i] = 1;
            return queue_mem[i];
        }
    }
    return NULL;
}

static uint32_t reg_read(virtio_dev *dev, uint32_t off) {
    return dev->regs[off / 4];
}

static void reg_write(virtio_dev *dev, uint32_t off, uint32_t v) {
    dev->regs[off / 4] = v;
}

static void mem_barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t now_ms(void) {
    uint64_t freq = arm_timer_freq_hz();
    if (freq == 0) {
        return 0;
    }
    return arm_timer_now() * 1000 / freq;
}

int virtio_mmio_probe(uint32_t device_id, virtio_dev *dev) {
    int i;
    for (i = 0; i < MMIO_SLOTS; i++) {
        volatile uint32_t *regs = (volatile uint32_t *)(uintptr_t)(
            MMIO_BASE + (uint32_t)i * MMIO_STRIDE);
        uint32_t guard;
#ifdef VIRTIO_DEBUG
        arm_uart_write("[virtio] slot ");
        arm_uart_putc((char)('0' + i));
        arm_uart_write(" magic=");
        uart_dbg_hex(regs[0]);
        arm_uart_write(" ver=");
        uart_dbg_hex(regs[1]);
        arm_uart_write(" id=");
        uart_dbg_hex(regs[2]);
        arm_uart_write_line("");
#endif
        if (regs[REG_MAGIC / 4] != VIRTIO_MAGIC) {
            continue;
        }
        if (regs[REG_DEVICE_ID / 4] != device_id) {
            continue;
        }
        if (regs[REG_VERSION / 4] != 1u) {
            continue; /* レガシーのみ対応 */
        }
        dev->regs = regs;
        dev->device_id = device_id;
        dev->num_queues = 0;
        /* リセット: Status へ 0 を書き、0 に戻るまで待つ。 */
        reg_write(dev, REG_STATUS, 0);
        guard = 100000;
        while (reg_read(dev, REG_STATUS) != 0 && guard-- > 0) {
        }
        reg_write(dev, REG_STATUS, STATUS_ACKNOWLEDGE);
        reg_write(dev, REG_STATUS,
                  STATUS_ACKNOWLEDGE | STATUS_DRIVER);
        /* フィーチャは下位 32 ビットを読み、何も受理しない
         * (レガシーの基本機能だけで動かす)。 */
        reg_write(dev, REG_DEVICE_FEATURES_SEL, 0);
        (void)reg_read(dev, REG_DEVICE_FEATURES);
        reg_write(dev, REG_DRIVER_FEATURES_SEL, 0);
        reg_write(dev, REG_DRIVER_FEATURES, 0);
        /* レガシーではページ大きさを先に伝える (キュー設定の前提)。 */
        reg_write(dev, REG_GUEST_PAGE_SIZE, 4096u);
        return 0;
    }
    return -1;
}

int virtio_queue_init(virtio_dev *dev, int qidx) {
    virtio_queue *q;
    uint32_t num_max;
    uint32_t avail_off, used_off;
    int i;

    if (qidx < 0 || qidx >= VIRTIO_MAX_QUEUES) {
        return -1;
    }
    q = &dev->q[qidx];
    q->mem = queue_mem_alloc();
    if (q->mem == NULL) {
        return -1;
    }
    for (i = 0; i < (int)VQUEUE_MEM_SIZE; i++) {
        q->mem[i] = 0;
    }

    reg_write(dev, REG_QUEUE_SEL, (uint32_t)qidx);
    num_max = reg_read(dev, REG_QUEUE_NUM_MAX);
#ifdef VIRTIO_DEBUG
    arm_uart_write("[virtio] queue num_max=");
    uart_dbg_hex(num_max);
    arm_uart_write_line("");
#endif
    if (num_max == 0) {
        return -1;
    }
    q->num = VIRTIO_QUEUE_NUM;
    if (num_max < (uint32_t)q->num) {
        q->num = (uint16_t)num_max;
    }
    reg_write(dev, REG_QUEUE_NUM, q->num);

    /* レイアウト: desc (16*num) → avail (4 + 2*num + 2) →
     * 4096 境界 → used (4 + 8*num + 2)。 */
    q->desc = (virtio_desc *)q->mem;
    avail_off = 16u * q->num;
    q->avail_flags = (volatile uint16_t *)(q->mem + avail_off);
    q->avail_idx = (volatile uint16_t *)(q->mem + avail_off + 2);
    q->avail_ring = (volatile uint16_t *)(q->mem + avail_off + 4);
    used_off = (avail_off + 6u + 2u * q->num + 4095u) & ~4095u;
    q->used_flags = (volatile uint16_t *)(q->mem + used_off);
    q->used_idx = (volatile uint16_t *)(q->mem + used_off + 2);
    q->used_ring =
        (void *)(q->mem + used_off + 4);
    q->avail_pos = 0;
    q->last_used_idx = 0;
    q->free_count = (int)q->num;
    for (i = 0; i < VIRTIO_QUEUE_NUM; i++) {
        q->cookie[i] = NULL;
        /* 逆順に入れて、取り出すと 0, 1, 2... の順になるように */
        q->free_stack[i] = (uint16_t)(q->num - 1 - i);
    }

    reg_write(dev, REG_QUEUE_PFN,
              (uint32_t)((uint64_t)(uintptr_t)q->mem / 4096u));
    if (qidx + 1 > dev->num_queues) {
        dev->num_queues = qidx + 1;
    }
    return 0;
}

void virtio_driver_ok(virtio_dev *dev) {
    reg_write(dev, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER |
                                   STATUS_DRIVER_OK);
}

int virtio_queue_add(virtio_dev *dev, int qidx, const virtio_seg *segs,
                     int nsegs, void *cookie) {
    virtio_queue *q = &dev->q[qidx];
    uint16_t ids[VIRTIO_QUEUE_NUM];
    uint16_t head;
    int i;

    if (nsegs <= 0 || nsegs > q->free_count) {
        return -1;
    }
    for (i = 0; i < nsegs; i++) {
        ids[i] = q->free_stack[--q->free_count];
    }
    head = ids[0];
    for (i = 0; i < nsegs; i++) {
        virtio_desc *d = &q->desc[ids[i]];
        d->addr = (uint64_t)(uintptr_t)segs[i].buf;
        d->len = segs[i].len;
        d->flags = 0;
        if (segs[i].write) {
            d->flags |= DESC_F_WRITE;
        }
        if (i + 1 < nsegs) {
            d->flags |= DESC_F_NEXT;
            d->next = ids[i + 1];
        } else {
            d->next = 0;
        }
    }
    q->cookie[head] = cookie;
    mem_barrier();
    q->avail_ring[q->avail_pos % q->num] = head;
    mem_barrier();
    q->avail_pos++;
    *q->avail_idx = q->avail_pos;
    return (int)head;
}

void virtio_queue_notify(virtio_dev *dev, int qidx) {
    mem_barrier();
    reg_write(dev, REG_QUEUE_NOTIFY, (uint32_t)qidx);
}

int virtio_queue_poll(virtio_dev *dev, int qidx, uint32_t *out_id,
                      uint32_t *out_len, void **out_cookie) {
    virtio_queue *q = &dev->q[qidx];
    uint16_t used_now;
    uint32_t id, len;
    uint32_t isr;

    mem_barrier();
    used_now = *q->used_idx;
    if (used_now == q->last_used_idx) {
        return -1;
    }
    id = q->used_ring[q->last_used_idx % q->num].id;
    len = q->used_ring[q->last_used_idx % q->num].len;
    q->last_used_idx++;
    /* 完了したチェーンの記述子を空き山へ戻す。 */
    if (id < (uint32_t)q->num) {
        uint32_t cur = id;
        int guard = 0;
        while (guard++ < (int)q->num) {
            uint16_t flags = q->desc[cur].flags;
            uint16_t next = q->desc[cur].next;
            q->free_stack[q->free_count++] = (uint16_t)cur;
            if ((flags & DESC_F_NEXT) == 0) {
                break;
            }
            cur = next;
        }
    }
    if (out_id != NULL) {
        *out_id = id;
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    if (out_cookie != NULL) {
        *out_cookie = (id < VIRTIO_QUEUE_NUM) ? q->cookie[id] : NULL;
    }
    /* 割り込み状態は読んで消しておく (使わないが残さない)。 */
    isr = reg_read(dev, REG_INTERRUPT_STATUS);
    if (isr != 0) {
        reg_write(dev, REG_INTERRUPT_ACK, isr);
    }
    return 0;
}

int virtio_exec(virtio_dev *dev, int qidx, const virtio_seg *segs,
                int nsegs, uint32_t *out_len, uint32_t timeout_ms) {
    uint64_t start = now_ms();
    uint32_t len = 0;
    if (virtio_queue_add(dev, qidx, segs, nsegs, NULL) < 0) {
        return -1;
    }
    virtio_queue_notify(dev, qidx);
    for (;;) {
        if (virtio_queue_poll(dev, qidx, NULL, &len, NULL) == 0) {
            if (out_len != NULL) {
                *out_len = len;
            }
            return 0;
        }
        if (now_ms() - start > timeout_ms) {
            return -1;
        }
    }
}

uint32_t virtio_config32(virtio_dev *dev, uint32_t offset) {
    return reg_read(dev, REG_CONFIG + offset);
}

uint64_t virtio_config64(virtio_dev *dev, uint32_t offset) {
    uint64_t lo = reg_read(dev, REG_CONFIG + offset);
    uint64_t hi = reg_read(dev, REG_CONFIG + offset + 4);
    return lo | (hi << 32);
}
