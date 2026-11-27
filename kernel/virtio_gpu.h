/* PrismOS — virtio-gpu ドライバ (モダン virtio PCI、2D 表示)。
 *
 * virtio 1.0 の PCI 機能レジスタ (共通設定・通知・機器設定) を
 * 使う 2D 専用ドライバ。制御キューに GET_DISPLAY_INFO /
 * RESOURCE_CREATE_2D / RESOURCE_ATTACH_BACKING / SET_SCANOUT /
 * TRANSFER_TO_HOST_2D / RESOURCE_FLUSH を流し、fb 層の描画先を
 * GPU のリソースバッキングメモリへ切り替える。機器が無ければ
 * 何もせず、呼び出し側は従来の表示 (ブートローダ経由の
 * フレームバッファ + VBE) にフォールバックする。
 */
#ifndef PRISM_VIRTIO_GPU_H
#define PRISM_VIRTIO_GPU_H

#include <stdint.h>

/* 機器を探して初期化する。表示装置として使える状態に
 * なれば 1、機器が無い/失敗なら 0 を返す。成功した場合は
 * virtio_gpu_framebuffer() 等で描画先が取れる。 */
int virtio_gpu_init(uint64_t hhdm_offset);

/* 表示先として有効か (初期化成功後 1)。 */
int virtio_gpu_active(void);

/* fb_init へ渡す描画先バッキングメモリの仮想アドレスと
 * 寸法。pitch は width * 4 (32bpp 固定)。 */
void *virtio_gpu_framebuffer(void);
uint32_t virtio_gpu_width(void);
uint32_t virtio_gpu_height(void);
uint64_t virtio_gpu_pitch(void);

/* fb_flush の present フック: バッキングの内容を機器へ
 * 転送し、スキャンアウトへ反映する。 */
void virtio_gpu_present(void);

/* 解像度変更 (vbe_set_mode から委譲される)。成功したら 0、
 * 失敗したら -1。fb 層は切り替え済みになる。 */
int virtio_gpu_set_mode(uint32_t width, uint32_t height);

/* S3 復帰: 機器側の状態 (キュー・リソース・スキャンアウト)
 * を作り直して再表示する。成功したら 0。 */
int virtio_gpu_resume(void);

#endif /* PRISM_VIRTIO_GPU_H */
