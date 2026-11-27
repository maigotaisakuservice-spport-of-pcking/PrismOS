/* PrismOS — ARM 用 virtio ネットワークデバイス (virtio-mmio)
 *
 * netstack (kernel/netstack.c) が使う e1000 と同じ名前の
 * インターフェースを virtio-net で実装する。これにより
 * netstack.c と net.c は無改造で ARM に載る。
 *
 * レガシー virtio-net の約束:
 *   キュー 0 = 受信、キュー 1 = 送信。
 *   各パケットの前に 10 バイトのヘッダ (flags/gso 等) が付く。
 *   受信バッファは先にキューへ投入しておき、完了をポーリング
 *   で拾う。送信は同期実行でよい。
 *   MAC アドレスは設定領域の先頭 6 バイト。
 */
#include "e1000.h"

#include <stddef.h>

#include "virtio.h"

#define VIRTIO_ID_NET 1u
#define NET_HDR_LEN 10u
#define NET_BUF_LEN 1514u
#define RX_BUFS 8

typedef struct {
    uint8_t hdr[NET_HDR_LEN];
    uint8_t data[NET_BUF_LEN];
} rx_slot;

static virtio_dev net_dev;
static int net_ready_flag;
static uint8_t net_mac[6];
static rx_slot rx_slots[RX_BUFS];
static uint8_t tx_hdr[NET_HDR_LEN];

static int rx_post(int idx) {
    virtio_seg segs[2];
    segs[0].buf = rx_slots[idx].hdr;
    segs[0].len = NET_HDR_LEN;
    segs[0].write = 1;
    segs[1].buf = rx_slots[idx].data;
    segs[1].len = NET_BUF_LEN;
    segs[1].write = 1;
    return virtio_queue_add(&net_dev, 0, segs, 2,
                            (void *)(uintptr_t)idx);
}

int e1000_init(uint64_t hhdm_offset) {
    int i;
    (void)hhdm_offset; /* 恒等写像なのでオフセットは要らない */
    if (virtio_mmio_probe(VIRTIO_ID_NET, &net_dev) != 0) {
        return -1;
    }
    if (virtio_queue_init(&net_dev, 0) != 0 ||
        virtio_queue_init(&net_dev, 1) != 0) {
        return -1;
    }
    virtio_driver_ok(&net_dev);
    for (i = 0; i < 6; i++) {
        uint32_t w = virtio_config32(&net_dev, (uint32_t)(i & ~3));
        net_mac[i] = (uint8_t)(w >> ((i & 3) * 8));
    }
    for (i = 0; i < RX_BUFS; i++) {
        if (rx_post(i) < 0) {
            return -1;
        }
    }
    virtio_queue_notify(&net_dev, 0);
    net_ready_flag = 1;
    return 0;
}

int e1000_ready(void) {
    return net_ready_flag;
}

const uint8_t *e1000_ethaddr(void) {
    return net_mac;
}

int e1000_link_up(void) {
    /* フィーチャ交渉をしていないため status 領域は読めない。
     * QEMU のユーザーネットワークは常時リンクアップなので、
     * デバイスがあればリンクありとして進める (netstack 側は
     * ダウンでも継続する設計)。 */
    return net_ready_flag;
}

int e1000_send(const uint8_t *frame, size_t len) {
    virtio_seg segs[2];
    uint32_t used_len = 0;
    int i;
    if (!net_ready_flag || len > NET_BUF_LEN) {
        return -1;
    }
    for (i = 0; i < (int)NET_HDR_LEN; i++) {
        tx_hdr[i] = 0; /* flags=0, gso_type=NONE: 素のフレーム */
    }
    segs[0].buf = tx_hdr;
    segs[0].len = NET_HDR_LEN;
    segs[0].write = 0;
    segs[1].buf = (void *)frame;
    segs[1].len = (uint32_t)len;
    segs[1].write = 0;
    if (virtio_exec(&net_dev, 1, segs, 2, &used_len, 5000) != 0) {
        return -1;
    }
    return 0;
}

int e1000_recv(uint8_t *buf, size_t max_len) {
    uint32_t id = 0, used_len = 0;
    void *cookie = NULL;
    int idx;
    size_t frame_len, copy;
    size_t i;

    if (!net_ready_flag) {
        return -1;
    }
    if (virtio_queue_poll(&net_dev, 0, &id, &used_len, &cookie) != 0) {
        return 0; /* 何も来ていない */
    }
    (void)id;
    idx = (int)(uintptr_t)cookie;
    if (idx < 0 || idx >= RX_BUFS || used_len < NET_HDR_LEN) {
        if (idx >= 0 && idx < RX_BUFS) {
            rx_post(idx);
            virtio_queue_notify(&net_dev, 0);
        }
        return 0;
    }
    frame_len = used_len - NET_HDR_LEN;
    copy = frame_len < max_len ? frame_len : max_len;
    for (i = 0; i < copy; i++) {
        buf[i] = rx_slots[idx].data[i];
    }
    rx_post(idx);
    virtio_queue_notify(&net_dev, 0);
    return (int)copy;
}

/* netstack.c は x86 用に virtio_net_* も直接参照する。
 * ARM では上の e1000 名前が実体なので、こちらは
 * その別名として用意する (net_init は e1000 側の
 * 成功で決まるため virtio_net_init は 0 を返す)。 */
int virtio_net_init(uint64_t hhdm_offset) {
    (void)hhdm_offset;
    return 0;
}

int virtio_net_link_up(void) {
    return e1000_link_up();
}

const uint8_t *virtio_net_ethaddr(void) {
    return e1000_ethaddr();
}

int virtio_net_send(const uint8_t *frame, size_t len) {
    return e1000_send(frame, len);
}

int virtio_net_recv(uint8_t *buf, size_t max_len) {
    return e1000_recv(buf, max_len);
}

int virtio_net_present(void) {
    return e1000_ready();
}
