/* PrismOS — SMP (マルチコア起動)
 *
 * ACPI MADT を解析して CPU 一覧と Local APIC の場所を
 * 得て、BSP 以外の CPU (AP) を INIT/SIPI で起動する。
 * AP はトランポリン (物理 0x8000) からロングモードに
 * 入り、ap_main で GDT/IDT を読み直し、LAPIC を有効化
 * してチェックインする。その後は割り込み待ち (hlt) で
 * 待機し、BSP からの IPI (ベクタ 0x40) に応答する。
 *
 * 作業分担: スケジューラは無いが、BSP が関数ポインタを
 * AP に渡して IPI で実行させる仕組み (smp_post_work)
 * がある。AP は割り込み文脈でその仕事を実行し、完了を
 * フラグで返す。眠る/ブロックする処理は渡せない。
 */
#ifndef PRISM_SMP_H
#define PRISM_SMP_H

#include <stddef.h>
#include <stdint.h>

/* RSDP の物理アドレス (Limine 応答) を受け取り初期化。
 * 起動できた CPU 数 (BSP 込み) を返す。失敗時は 1。 */
int smp_init(uint64_t rsdp_addr, uint64_t hhdm_offset);
int smp_cpu_count(void);
uint8_t smp_apic_id(int index);
int smp_is_bsp(int index);
/* その CPU が IPI に応答した回数。 */
uint32_t smp_ping_count(int index);
/* 全 AP に IPI を送る (ベクタ 0x40)。 */
void smp_ping_all(void);
/* IDT の IPI ハンドラから呼ばれる。 */
void smp_ipi_handler(void);

/* --- 実作業分担 (スケジューラなしの範囲) ---
 * BSP が関数ポインタと引数を AP のスロットに置いて
 * IPI を送ると、AP は割り込みハンドラ内でそれを実行
 * する。重い/眠る処理は渡さないこと (割り込み文脈)。 */
typedef void (*smp_work_fn)(void *arg);
/* AP (index は BSP でない CPU) に仕事を投げる。
 * 0=受付、-1=不可 (対象外・実行中)。 */
int smp_post_work(int index, smp_work_fn fn, void *arg);
/* 投げた仕事が終わっていれば 1。 */
int smp_work_finished(int index);
/* その CPU が完了した仕事の総数。 */
uint32_t smp_work_completed(int index);

/* 検証用の仕事: バッファ範囲の FNV-1a 64 ハッシュを
 * 計算して job->hash に書く。smp_post_work に渡す。 */
struct smp_hash_job {
    const uint8_t *p;
    size_t n;
    uint64_t hash;
};
void smp_hash_work(void *arg);

#endif /* PRISM_SMP_H */
