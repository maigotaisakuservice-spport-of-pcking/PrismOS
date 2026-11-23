/* PrismOS — Intel HD Audio (HDA) ドライバ
 *
 * PCI クラス 04/03 (Audio) のコントローラを初期化し、
 * コーデックを列挙して出力系 (DAC + ピン) を設定し、
 * 出力ストリームで PCM を再生する。バーブの送受信は
 * Immediate Command レジスタ (ICOI/ICII/ICSI) を使う
 * (CORB/RIRB は使わない)。割り込みは使わずポーリング。
 * SB16 とは独立した追加ドライバで、`hda` コマンドから
 * 状態表示と試聴 (トーン再生) ができる。
 */
#ifndef PRISM_HDA_H
#define PRISM_HDA_H

#include <stdint.h>

int hda_init(uint64_t hhdm_offset);
int hda_present(void);
const char *hda_codec_name(void);
/* 440Hz のトーンを約 0.6 秒再生する。0 = 開始できた。 */
int hda_beep(void);
/* ストリーム位置 (LPIB) — 再生が進んでいる証拠に使う。 */
uint32_t hda_stream_pos(void);
void hda_debug_dump(void);

/* --- 録音 (入力ストリーム + ADC) ---
 * QEMU の hda-duplex コーデックには ADC (Audio Input)
 * があり、設定 (電源・接続選択・ストリーム ID・書式) は
 * 読み戻しで確認済み。ただしこの VM で使える音声
 * バックエンド (none / sdl / wav) はどれもゲスト採取を
 * クロックしないため、入力 DMA の完了はここでは観測
 * できていない (LPIB が進まない)。採取可能なバック
 * エンド (PulseAudio 等) か実機での確認が残る。
 * 形式は再生と同じ 48kHz/16bit/ステレオ。 */
int hda_capture_supported(void);
/* millis ミリ秒だけ録音する。実際に録れたバイト数と
 * ピーク振幅 (絶対値) を返す。成功 0、不可なら -1。 */
int hda_record(uint32_t millis, uint32_t *bytes_out,
               uint32_t *peak_out);
/* 録音バッファ (HHDM 上の PCM) と、その物理アドレスを
 * 使った再生。 */
const int16_t *hda_capture_data(void);
int hda_capture_load(const int16_t *src, uint32_t bytes);
int hda_play_capture(uint32_t bytes);
/* 入力ストリームの LPIB。 */
uint32_t hda_capture_pos(void);

#endif /* PRISM_HDA_H */
