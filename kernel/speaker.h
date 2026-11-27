/* PrismOS — PC スピーカー (PIT チャンネル 2 + ポート 0x61)。
 *
 * beep は単一周波数の矩形波を鳴らす。タイミングは PIT
 * チャンネル 0 のカウンタをポーリングして取る (割り込みは
 * 使わない)。WAV の再生は SB16 (kernel/sb16.c) が行い、
 * こちらは使わない。
 * 注意: この QEMU (8.2) には PC スピーカーの音声出力が無く、
 * beep の音は録音では確かめられない。SB16 の再生は
 * scripts/music-test.py で録音して確かめてある。
 */
#ifndef PRISM_SPEAKER_H
#define PRISM_SPEAKER_H

#include <stddef.h>
#include <stdint.h>

/* PIT チャンネル 0 のカウンタを 1 回読む (減少カウンタ)。 */
uint16_t speaker_pit0_count(void);

/* 指定した PIT ティック数だけ待つ (1 ティック = 1/1193182 秒)。
 * 16 ビットカウンタの折り返しをまたいでも正しく待つ。 */
void speaker_wait_ticks(uint32_t ticks);

/* 周波数 freq_hz の矩形波を ms ミリ秒だけ鳴らす。 */
void speaker_tone(uint32_t freq_hz, uint32_t ms);

/* スピーカーを止める (ポート 0x61 のビットを落とす)。 */
void speaker_off(void);

#endif /* PRISM_SPEAKER_H */
