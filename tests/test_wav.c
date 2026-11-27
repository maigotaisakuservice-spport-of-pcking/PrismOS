/* PrismOS — WAV 解析のホストテスト。本物のサンプルファイル
 * (assets/melody.wav) も読んで確かめる。 */
#include <stdio.h>
#include <string.h>

#include "wav.h"
#include "../shell/sample_wav.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

int main(void) {
    wav_info info;

    /* 本物のサンプル: 8 ビット・モノラル・8000 Hz、3200 サンプル。 */
    check(wav_parse(melody_sample_wav, melody_sample_wav_len, &info) == 0,
          "sample melody.wav parses");
    check(info.sample_rate == 8000, "sample rate is 8000 Hz");
    check(info.sample_count == 3200, "sample count is 3200");
    check(info.samples != NULL, "samples pointer set");
    /* 最初のサンプルは無音付近 (128)、途中に音がある。 */
    int loud = 0;
    for (size_t i = 0; i < info.sample_count; i++) {
        int v = (int)info.samples[i] - 128;
        if (v < 0) {
            v = -v;
        }
        if (v > 60) {
            loud = 1;
        }
    }
    check(loud, "sample has loud parts");

    /* 壊れた入力は拒否する。全文コピーを使う: 短く切ると
     * チャンク境界の検査で先に失敗し、形式の検査まで届かない。 */
    check(wav_parse(NULL, 100, &info) == -1, "NULL data rejected");
    check(wav_parse(melody_sample_wav, 10, &info) == -1,
          "short file rejected");
    static uint8_t copy[4000];
    for (size_t i = 0; i < melody_sample_wav_len; i++) {
        copy[i] = melody_sample_wav[i];
    }
    copy[0] = 'X';
    check(wav_parse(copy, melody_sample_wav_len, &info) == -1,
          "bad RIFF magic rejected");
    for (size_t i = 0; i < melody_sample_wav_len; i++) {
        copy[i] = melody_sample_wav[i];
    }
    copy[8] = 'X';
    check(wav_parse(copy, melody_sample_wav_len, &info) == -1,
          "bad WAVE magic rejected");

    /* ステレオは拒否する (チャンネル数を 2 に書き換え)。 */
    for (size_t i = 0; i < melody_sample_wav_len; i++) {
        copy[i] = melody_sample_wav[i];
    }
    copy[22] = 2;
    copy[23] = 0;
    check(wav_parse(copy, melody_sample_wav_len, &info) == -1,
          "stereo rejected");

    /* 16 ビットは拒否する (ビット数を 16 に書き換え)。 */
    for (size_t i = 0; i < melody_sample_wav_len; i++) {
        copy[i] = melody_sample_wav[i];
    }
    copy[34] = 16;
    copy[35] = 0;
    check(wav_parse(copy, melody_sample_wav_len, &info) == -1,
          "16-bit rejected");

    /* 16 ビット WAV ヘッダの生成 (録音保存用)。 */
    {
        uint8_t hdr[44];
        check(wav_build_header16(hdr, sizeof(hdr), 192000, 48000,
                                 2) == 0,
              "header16 builds");
        check(memcmp(hdr, "RIFF", 4) == 0 &&
                  memcmp(hdr + 8, "WAVE", 4) == 0 &&
                  memcmp(hdr + 36, "data", 4) == 0,
              "header16 magics");
        check(hdr[22] == 2 && hdr[23] == 0, "header16 stereo");
        check(hdr[24] == 0x80 && hdr[25] == 0xBB,
              "header16 rate 48000");
        check(hdr[34] == 16 && hdr[35] == 0, "header16 16-bit");
        uint32_t dsz = (uint32_t)hdr[40] | ((uint32_t)hdr[41] << 8) |
                       ((uint32_t)hdr[42] << 16) |
                       ((uint32_t)hdr[43] << 24);
        check(dsz == 192000, "header16 data size");
        check(wav_build_header16(hdr, 10, 0, 48000, 2) == -1,
              "header16 small cap rejected");
    }

    printf("wav tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
