/* PrismOS — Sound Blaster 16 (ISA, ポート 0x220)。
 *
 * 8 ビット PCM を DSP の DMA 転送で鳴らす本物のオーディオ
 * ドライバ。DMA チャンネル 1 を使い、PMM から取った 1 ページ
 * (4096 バイト、ページ境界に揃っているため 64KB 境界を
 * またがない) を転送バッファにする。割り込みは使わず、転送
 * 時間だけ PIT で待つ (転送は DSP がサンプルレートで進める)。
 * 再生できるのは 8 ビット・モノラル PCM、最大 4096 サンプル。
 */
#ifndef PRISM_SB16_H
#define PRISM_SB16_H

#include <stddef.h>
#include <stdint.h>

/* HHDM のオフセットを覚える (main.c の memory_init から呼ぶ)。
 * 検出や転送は初回利用時に遅延して行う。 */
void sb16_set_hhdm(uint64_t hhdm_offset);

/* カードが見つかったら 1、無ければ 0。初回は DSP をリセット
 * して検出する。 */
int sb16_present(void);

/* 8 ビット PCM を鳴らす。samples は符号なし (128 が無音)。
 * 成功したら 0、カード無し・長すぎ・レート範囲外は -1。
 * 再生が終わるまで戻らない。 */
int sb16_play_8bit(const uint8_t *samples, size_t count,
                   uint32_t rate);

/* 非同期再生 (C8): 開始だけして戻る。進み具合は
 * DMA カウンタで数える。 */
int sb16_start_8bit(const uint8_t *samples, size_t count,
                    uint32_t rate);
/* 再生中なら 1。 */
int sb16_is_playing(void);
/* 再生済みサンプル数 (近似)。 */
uint32_t sb16_played(void);
/* 全サンプル数 (再生中でなければ 0)。 */
uint32_t sb16_total(void);
void sb16_pause(void);
void sb16_resume(void);
void sb16_stop(void);

/* 音量 (0-100%) を設定する。SB16 ミキサのマスター
 * 音量レジスタに書き、再生時のサンプル利得にも使う。 */
void sb16_set_volume(int percent);
int sb16_get_volume(void);

#endif /* PRISM_SB16_H */
