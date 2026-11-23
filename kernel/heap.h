/* PrismOS — カーネルヒープ (kmalloc / kfree)
 *
 * 設計:
 *   * PMM からまとまったページ (HEAP_PAGES ページ) を
 *     1 つのアリーナとして確保し、その中でブロック単位で
 *     管理する。先頭適合 (first-fit) で分割し、解放時に隣接
 *     する空きブロックと結合する。
 *   * 物理アドレスと仮想アドレスの変換は、呼び出し側が渡す
 *     オフセット (物理 + オフセット = 仮想) で行う。カーネル
 *     側ではブートローダの HHDM オフセットを渡す。ホスト
 *     テストでは 0 を渡す (擬似物理アドレスがそのまま
 *     ホストのポインタになる)。
 *   * heap_init() を呼ぶ前に kmalloc() を呼んではいけない。
 *     初期化前や枯渇時は NULL を返す。
 *   * 割り込み安全性: 割り込みハンドラからは呼ばない。シェル
 *     の文脈からのみ呼ぶ (PMM と同じ約束)。
 */
#ifndef PRISM_HEAP_H
#define PRISM_HEAP_H

#include <stddef.h>
#include <stdint.h>

#define HEAP_PAGES 512 /* 512 ページ = 2MB。VFS の全ファイル (64 件 x 16KB)
                       * とシェルの出力バッファが同時に載る大きさ。 */

void heap_init(uint64_t phys_to_virt_offset);
void *kmalloc(size_t size);
void kfree(void *ptr);

size_t heap_total_bytes(void);
size_t heap_free_bytes(void);
size_t heap_used_bytes(void);
int heap_is_ready(void);

#endif /* PRISM_HEAP_H */
