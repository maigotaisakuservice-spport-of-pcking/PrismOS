/* PrismOS — WAV (PCM) の解析。純粋関数なのでカーネルと
 * ホストテストで同じソースを使う。
 *
 * 対応するのは RIFF/WAVE、フォーマット PCM (1)、モノラル、
 * 8 ビットのみ。音楽アプリはこの形式のファイルだけ鳴らす。
 * サンプルレートは 4000〜48000 Hz を受け付ける。
 */
#ifndef PRISM_WAV_H
#define PRISM_WAV_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *samples; /* 8 ビット PCM の生データ */
    size_t sample_count;
    uint32_t sample_rate; /* Hz */
} wav_info;

/* data が WAV なら 0 を返し、out に情報を入れる。samples は
 * data の中を指す (コピーしない)。形式が違う場合は -1。 */
int wav_parse(const uint8_t *data, size_t len, wav_info *out);

/* 16 ビット PCM の WAV ヘッダ (44 バイト) を out に書く。
 * 録音ファイルの保存用。cap が 44 未満なら -1。 */
int wav_build_header16(uint8_t *out, size_t cap,
                       uint32_t data_bytes, uint32_t sample_rate,
                       uint16_t channels);

#endif /* PRISM_WAV_H */
