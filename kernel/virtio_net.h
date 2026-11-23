/* PrismOS — virtio-net ドライバ (レガシー PCI、ポーリング)
 *
 * 1AF4:1000 (transitional) をレガシー virtio PCI インターフェース
 * で駆動する。キュー 0 が受信、キュー 1 が送信。各パケットの前に
 * 10 バイトの virtio_net_hdr が付く (機能交渉なしの素の形式)。
 * e1000 が無い環境 (仮想環境版など) でネットスタックの NIC に
 * なる。割り込みは使わず、受信はポーリングで used リングを見る。
 */
#ifndef PRISM_VIRTIO_NET_H
#define PRISM_VIRTIO_NET_H

#include <stddef.h>
#include <stdint.h>

/* 見つけて初期化できたら 1、無ければ 0。 */
int virtio_net_init(uint64_t hhdm_offset);
int virtio_net_present(void);
int virtio_net_link_up(void);
const uint8_t *virtio_net_ethaddr(void);
/* 0 = 送れた、負 = 失敗。e1000_send と同じ約束。 */
int virtio_net_send(const uint8_t *frame, size_t len);
/* 受信したら長さ、無ければ 0、エラーは負。e1000_recv と同じ。 */
int virtio_net_recv(uint8_t *buf, size_t max_len);

#endif /* PRISM_VIRTIO_NET_H */
