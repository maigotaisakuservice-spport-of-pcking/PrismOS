/* PrismOS — 互換ゲストのサンドボックス
 *
 * ゲスト専用のページテーブル (PML4) と、暴走ゲストを止める
 * ウォッチドッグ (PIT IRQ0) を用意する。
 *
 * ゲストの PML4 の上半分 (カーネル側) は実行中のカーネルの
 * PML4 からそのまま写す。カーネルのページは管理者専用
 * (U/S=0) なので、リング 3 のゲストからは触れない。下半分
 * にはゲストのイメージ・スタック・brk だけを U/S=1 で張る。
 * それ以外のアドレスは存在しないので、ゲストが変な
 * ポインタを触るとページフォルトになり、例外ハンドラが
 * ゲストだけを終わらせる (カーネルは生き続ける)。
 */
#ifndef PRISM_SANDBOX_H
#define PRISM_SANDBOX_H

#include <stdint.h>

/* HHDM のずれを覚える (kmain が起動時に呼ぶ)。 */
void sandbox_init(uint64_t hhdm_offset);

/* ゲスト用の PML4 を作る。物理アドレスを返す (失敗は 0)。 */
uint64_t sandbox_create(void);

/* va に既存の物理ページを張る (U/S=1)。 */
int sandbox_map(uint64_t pml4_phys, uint64_t va, uint64_t phys,
                int writable, int executable);
/* va に新しいゼロ埋めページを割り当てて張る。
 * 物理アドレスを *out_phys に返す (不要なら NULL)。 */
int sandbox_map_new(uint64_t pml4_phys, uint64_t va, int writable,
                    int executable, uint64_t *out_phys);
/* va が張られている物理ページを返す (無ければ 0)。 */
uint64_t sandbox_lookup(uint64_t pml4_phys, uint64_t va);

/* ゲストのテーブルと、張ったユーザーページを全部解放する。 */
void sandbox_destroy(uint64_t pml4_phys);

/* ウォッチドッグ: PIT IRQ0 を約 100Hz で回し、ゲストの
 * 実行時間を数える。制限を超えたら例外と同じ経路で
 * ゲストだけを終わらせる。ゲスト実行の前後で呼ぶ。 */
void sandbox_watchdog_start(void);
void sandbox_watchdog_stop(void);

/* ゲスト実行中は 1 (compat_run が立てる)。例外ハンドラと
 * ウォッチドッグが参照する。 */
extern volatile int compat_guest_active;

#endif /* PRISM_SANDBOX_H */
