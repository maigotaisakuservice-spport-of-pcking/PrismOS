/* PrismOS — ARM 用 virtio-mmio トランスポート (レガシー)
 *
 * QEMU virt 構成の virtio-mmio スロット (0x0a000000 から
 * 0x200 刻みで 8 個。DTB の virtio_mmio ノードで確認した
 * 実際の配置) を走査し、目的のデバイスを探す。
 * レガシーインターフェース (Version 1) でキューを 1 本ないし
 * 2 本用意し、記述子チェーンの同期実行と、受信バッファの
 * 事前投入＋ポーリングの両方を提供する。割り込みは使わず
 * ポーリングで完了を待つ (x86 側ドライバと同じ流儀)。
 *
 * メモリは恒等写像なので、物理アドレス = 仮想アドレス。
 * キューのメモリは静的に確保し、4096 バイト境界に置く
 * (レガシーの QueueAlign = 4096)。
 */
#ifndef PRISM_ARM_VIRTIO_H
#define PRISM_ARM_VIRTIO_H

#include <stddef.h>
#include <stdint.h>

#define VIRTIO_MAX_QUEUES 2
#define VIRTIO_QUEUE_NUM 16

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtio_desc;

typedef struct {
    /* キューの実メモリ (静的確保、4096 境界) */
    uint8_t *mem;
    virtio_desc *desc;
    volatile uint16_t *avail_flags;
    volatile uint16_t *avail_idx;
    volatile uint16_t *avail_ring;
    volatile uint16_t *used_flags;
    volatile uint16_t *used_idx;
    struct {
        volatile uint32_t id;
        volatile uint32_t len;
    } *used_ring;
    uint16_t num;
    uint16_t free_stack[VIRTIO_QUEUE_NUM]; /* 空き記述子 id の山 */
    int free_count;
    uint16_t avail_pos;      /* avail リングへ入れた数 */
    uint16_t last_used_idx;  /* 消費した used の位置 */
    void *cookie[VIRTIO_QUEUE_NUM]; /* 記述子 id → 呼び出し側の印 */
} virtio_queue;

typedef struct {
    volatile uint32_t *regs;
    uint32_t device_id;
    virtio_queue q[VIRTIO_MAX_QUEUES];
    int num_queues;
} virtio_dev;

/* 1 記述子の断片。write != 0 でデバイスが書き込む側。 */
typedef struct {
    void *buf;
    uint32_t len;
    int write;
} virtio_seg;

/* スロットを走査して device_id のデバイスを探し、レガシー
 * 初期化 (リセット → ACK → DRIVER、フィーチャは 0 を受理)
 * まで行う。見つかれば 0。 */
int virtio_mmio_probe(uint32_t device_id, virtio_dev *dev);

/* キューを用意する (QueueSel/QueueNum/QueuePFN の設定)。 */
int virtio_queue_init(virtio_dev *dev, int qidx);

/* DRIVER_OK を立てて初期化を終える。 */
void virtio_driver_ok(virtio_dev *dev);

/* チェーンを avail に入れる。cookie は完了時に返る。
 * 戻り値は先頭記述子の id、失敗したら -1。 */
int virtio_queue_add(virtio_dev *dev, int qidx, const virtio_seg *segs,
                     int nsegs, void *cookie);

/* キューへ通知する。 */
void virtio_queue_notify(virtio_dev *dev, int qidx);

/* 完了したチェーンを 1 つ取る (ブロックしない)。
 * 完了があれば 0 を返し、*out_id / *out_len / *out_cookie に
 * 入れる。無ければ -1。 */
int virtio_queue_poll(virtio_dev *dev, int qidx, uint32_t *out_id,
                      uint32_t *out_len, void **out_cookie);

/* チェーンを同期実行する: 入れて通知し、完了まで待つ
 * (制限時間つき)。完了したら 0、*out_len に used の長さ。 */
int virtio_exec(virtio_dev *dev, int qidx, const virtio_seg *segs,
                int nsegs, uint32_t *out_len, uint32_t timeout_ms);

/* デバイス設定領域 (offset 0x100 から) を読む。 */
uint32_t virtio_config32(virtio_dev *dev, uint32_t offset);
uint64_t virtio_config64(virtio_dev *dev, uint32_t offset);

#endif /* PRISM_ARM_VIRTIO_H */
