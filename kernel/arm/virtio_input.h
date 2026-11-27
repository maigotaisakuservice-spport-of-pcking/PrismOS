/* PrismOS — ARM 用 virtio-input キーボード。
 * virtio-mmio の入力デバイス (ID 18) の eventq に受信
 * バッファを張り、EV_KEY の押下イベントを ASCII に直して
 * 返す。マウス (EV_REL/EV_ABS) は今のところ読まない。 */
#ifndef PRISM_ARM_VIRTIO_INPUT_H
#define PRISM_ARM_VIRTIO_INPUT_H

/* デバイスを探して初期化する。0 = 使える、負数 = 無い。 */
int arm_virtio_input_init(void);

/* 押されたキーがあれば ASCII を返す。無ければ 0。
 * 呼び出し側はポーリングで使う (割り込みは使わない)。 */
int arm_virtio_input_poll_char(void);

#endif /* PRISM_ARM_VIRTIO_INPUT_H */
