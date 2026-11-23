/* PrismOS — 物理ページアロケータ (PMM)
 *
 * 設計:
 *   * 空きページ範囲のリスト (フリーリスト) で管理する。ビットマップ
 *     ではなく範囲リストにしたのは、メモリ量に依らず固定の小さな
 *     記憶域で済み、ホスト側の単体テストでも同じコードを動かせる
 *     ため。範囲は最大 64 個まで (QEMU のメモリマップは十数件)。
 *   * 使えるのは呼び出し側が渡した「使用可」範囲だけ。カーネル本体
 *     やブートローダが使う範囲は、呼び出し側が渡さないことで除外
 *     する (カーネル側ではメモリマップの種別で振り分ける)。
 *   * 1MB より下の低位メモリは使わない (ファームウェアが使うため)。
 *   * 割り当ては先頭の範囲から切り出す (first-fit)。解放は範囲を
 *     挿入し、隣り合う範囲と結合する。二重解放は無視する。
 *   * このファイルはハードウェアに触らない。libc も使わない。
 *     そのためカーネルでもホストテストでも同じソースを使える。
 *   * 割り込み安全性: 割り込みハンドラからは呼ばない (シェルの
 *     文脈からのみ呼ぶ)。呼び出し側がその約束を守ること。
 */
#ifndef PRISM_PMM_H
#define PRISM_PMM_H

#include <stddef.h>
#include <stdint.h>

#define PMM_PAGE_SIZE 4096ULL

/* メモリマップの 1 項目。呼び出し側がブートローダの種別を
 * そのまま渡す (使用可 = 0)。 */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint64_t type;
} prism_memmap_entry;

#define PMM_TYPE_USABLE 0ULL

/* 使用可範囲だけを空きリストに登録して初期化する。 */
void pmm_init(const prism_memmap_entry *entries, size_t count);

/* 1 ページ確保する。物理アドレスを返す。枯渇したら 0 を返す。
 * 0 は有効な物理アドレスとしては返さない (低位 1MB は使わない)。 */
uint64_t pmm_alloc_page(void);

/* count ページを連続で確保する。物理アドレスを返す。枯渇したら 0。 */
uint64_t pmm_alloc_pages(size_t count);

/* 確保したページを返す。ページ境界でないもの、範囲外のもの、
 * すでに空いているものは無視する。 */
void pmm_free_page(uint64_t addr);
void pmm_free_pages(uint64_t addr, size_t count);

uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);
uint64_t pmm_used_pages_count(void);

#endif /* PRISM_PMM_H */
