/* PrismOS — WAV (PCM) の解析 */
#include "wav.h"

static uint16_t wav_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t wav_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int wav_is(const uint8_t *p, const char *tag) {
    return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1] &&
           p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3];
}

int wav_parse(const uint8_t *data, size_t len, wav_info *out) {
    if (data == NULL || out == NULL || len < 44) {
        return -1;
    }
    if (!wav_is(data, "RIFF") || !wav_is(data + 8, "WAVE")) {
        return -1;
    }
    /* チャンクを順にたどる。"fmt " と "data" を探す。 */
    size_t pos = 12;
    int have_fmt = 0;
    int have_data = 0;
    uint16_t audio_fmt = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    const uint8_t *samples = NULL;
    size_t sample_count = 0;
    while (pos + 8 <= len) {
        uint32_t chunk_size = wav_rd32(data + pos + 4);
        if (pos + 8 + chunk_size > len) {
            return -1; /* チャンクがファイルをはみ出す */
        }
        if (wav_is(data + pos, "fmt ")) {
            if (chunk_size < 16) {
                return -1;
            }
            audio_fmt = wav_rd16(data + pos + 8);
            channels = wav_rd16(data + pos + 10);
            rate = wav_rd32(data + pos + 12);
            bits = wav_rd16(data + pos + 22);
            have_fmt = 1;
        } else if (wav_is(data + pos, "data")) {
            samples = data + pos + 8;
            sample_count = chunk_size;
            have_data = 1;
        }
        /* チャンクは偶数バイト境界に詰める。 */
        pos += 8 + chunk_size + (chunk_size & 1);
    }
    if (!have_fmt || !have_data) {
        return -1;
    }
    if (audio_fmt != 1) { /* PCM だけ */
        return -1;
    }
    if (channels != 1) { /* モノラルだけ */
        return -1;
    }
    if (bits != 8) { /* 8 ビットだけ */
        return -1;
    }
    if (rate < 4000 || rate > 48000) {
        return -1;
    }
    if (sample_count == 0) {
        return -1;
    }
    out->samples = samples;
    out->sample_count = sample_count;
    out->sample_rate = rate;
    return 0;
}

static void wav_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wav_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int wav_build_header16(uint8_t *out, size_t cap,
                       uint32_t data_bytes, uint32_t sample_rate,
                       uint16_t channels) {
    if (out == NULL || cap < 44 || channels == 0) {
        return -1;
    }
    const char *riff = "RIFF";
    const char *wave = "WAVE";
    const char *fmt = "fmt ";
    const char *data = "data";
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)riff[i];
        out[8 + i] = (uint8_t)wave[i];
        out[12 + i] = (uint8_t)fmt[i];
        out[36 + i] = (uint8_t)data[i];
    }
    wav_wr32(out + 4, 36 + data_bytes);
    wav_wr32(out + 16, 16); /* fmt チャンク長 */
    wav_wr16(out + 20, 1);  /* PCM */
    wav_wr16(out + 22, channels);
    wav_wr32(out + 24, sample_rate);
    wav_wr32(out + 28, sample_rate * channels * 2);
    wav_wr16(out + 32, (uint16_t)(channels * 2));
    wav_wr16(out + 34, 16); /* ビット数 */
    wav_wr32(out + 40, data_bytes);
    return 0;
}
