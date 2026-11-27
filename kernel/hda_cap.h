/* PrismOS — HD Audio 録音の純粋な採取ロジック。
 *
 * ハードウェアに触れない計算だけを抜き出したもの:
 * BDL (バッファ記述子リスト) の構築、録音時間からの目標
 * バイト数、DMA 位置 (LPIB) からの取得量、ピーク走査、
 * 採取バッファへの読み込み可否。kernel/hda.c の hda_record /
 * hda_capture_alloc / hda_capture_load が同じ関数を使う。
 * ホストテスト tests/test_hda_cap.c が同じコードを検証する。
 *
 * 注意: QEMU では音声バックエンド (none / SDL ダミー) が
 * 採取 DMA を進めないため、LPIB が進んで実サンプルが届く
 * ところまでは現環境では観測できない。ここで検証するのは
 * 採取ロジックそのもので、DMA 完了の観測ではない。
 */
#ifndef PRISM_HDA_CAP_H
#define PRISM_HDA_CAP_H

#include <stdint.h>

/* 採取バッファ: 36 エントリ x 16KB = 576KB。3 秒 (48000Hz x
 * 4 バイト x 3 = 576000 バイト) が収まる。 */
#define HDA_CAP_ENTRIES 36
#define HDA_CAP_ENTRY_BYTES 16384
#define HDA_CAP_BYTES (HDA_CAP_ENTRIES * HDA_CAP_ENTRY_BYTES)

/* 録音時間 (ミリ秒) から目標バイト数を決める。0 または
 * 3000 超は 2000 ミリ秒扱い。48000Hz x 4 バイト (16bit
 * ステレオ) で、上限は HDA_CAP_BYTES、4 バイト境界に
 * 切り下げる。 */
uint32_t hda_cap_target_bytes(uint32_t millis);

/* DMA 位置 (LPIB の最大値) と目標から、実際に取り出す
 * バイト数を決める。目標を超えない、4 バイト境界に
 * 切り下げる。 */
uint32_t hda_cap_got_bytes(uint32_t max_pos, uint32_t target);

/* PCM サンプル (samples 個) のピーク (絶対値の最大)。
 * pcm が NULL なら 0。-32768 は 32768 になる。 */
uint32_t hda_cap_peak(const int16_t *pcm, uint32_t samples);

/* 採取用 BDL を作る。bdl は 256 ワード以上あること。
 * 36 エントリ分を pcm_phys からの連続領域に向け、長さ
 * 16384、IOC なし (LPIB をポーリングする) にする。残りは 0。 */
void hda_cap_build_bdl(uint32_t *bdl, uint64_t pcm_phys);

/* 採取バッファへ読み込む前の検査。src が NULL、または
 * bytes が HDA_CAP_BYTES を超えていたら -1、そうでなければ 0。 */
int hda_cap_load_ok(const int16_t *src, uint32_t bytes);

#endif /* PRISM_HDA_CAP_H */
