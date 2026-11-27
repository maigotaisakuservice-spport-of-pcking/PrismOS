/* PrismOS — 動画プレイヤーの純粋ロジック (video.h を見ること)。 */
#include "video.h"

#include "inflate.h"

static int video_starts_with(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (*s != *prefix) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

static int video_ends_with(const char *s, const char *suffix) {
    size_t sl = 0;
    while (s[sl] != '\0') {
        sl++;
    }
    size_t fl = 0;
    while (suffix[fl] != '\0') {
        fl++;
    }
    if (sl < fl) {
        return 0;
    }
    s += sl - fl;
    while (*suffix != '\0') {
        if (*s != *suffix) {
            return 0;
        }
        s++;
        suffix++;
    }
    return 1;
}

static int video_name_less(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a < (unsigned char)*b;
}

static int video_collect_suffix(const char **names, int count, int *out,
                                int max, const char *suffix) {
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        if (names[i] == NULL) {
            continue;
        }
        if (!video_starts_with(names[i], VIDEO_FRAME_PREFIX)) {
            continue;
        }
        if (!video_ends_with(names[i], suffix)) {
            continue;
        }
        /* 挿入ソートで名前順に入れる。 */
        int j = n;
        while (j > 0 && video_name_less(names[i], names[out[j - 1]])) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
        n++;
    }
    return n;
}

int video_collect(const char **names, int count, int *out, int max) {
    return video_collect_suffix(names, count, out, max,
                                VIDEO_FRAME_SUFFIX);
}

int video_collect_pv1(const char **names, int count, int *out, int max) {
    return video_collect_suffix(names, count, out, max,
                                VIDEO_PV1_SUFFIX);
}

/* ---- PV1 形式 ---- */

int pv1_parse_header(const uint8_t *data, size_t len, pv1_header *out) {
    if (data == NULL || out == NULL || len < PV1_HEADER_LEN) {
        return -1;
    }
    if (data[0] != 'P' || data[1] != 'R' || data[2] != 'V' ||
        data[3] != '1') {
        return -1;
    }
    uint16_t w = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
    uint16_t h = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
    uint8_t type = data[8];
    if (data[9] != 0) {
        return -1;
    }
    if (type != PV1_TYPE_KEY && type != PV1_TYPE_DIFF) {
        return -1;
    }
    if (w == 0 || h == 0 || w > PV1_MAX_WIDTH || h > PV1_MAX_HEIGHT) {
        return -1;
    }
    uint32_t raw_len = (uint32_t)data[10] |
                       ((uint32_t)data[11] << 8) |
                       ((uint32_t)data[12] << 16) |
                       ((uint32_t)data[13] << 24);
    uint32_t comp_len = (uint32_t)data[14] |
                        ((uint32_t)data[15] << 8) |
                        ((uint32_t)data[16] << 16) |
                        ((uint32_t)data[17] << 24);
    if (comp_len == 0 || comp_len > len - PV1_HEADER_LEN) {
        return -1;
    }
    if (type == PV1_TYPE_KEY) {
        /* キーフレームのデータは全画素ちょうど。 */
        if (raw_len != (uint32_t)w * (uint32_t)h * 2) {
            return -1;
        }
    } else {
        /* 差分フレームのデータは終端 (2 バイト) より長い。 */
        if (raw_len < 2) {
            return -1;
        }
    }
    out->width = w;
    out->height = h;
    out->type = type;
    out->raw_len = raw_len;
    out->comp_len = comp_len;
    return 0;
}

int pv1_audio_len(const uint8_t *data, size_t len) {
    pv1_header hdr;
    if (pv1_parse_header(data, len, &hdr) != 0) {
        return -1;
    }
    size_t video_end = PV1_HEADER_LEN + (size_t)hdr.comp_len;
    if (len <= video_end) {
        return 0;
    }
    size_t audio = len - video_end;
    if (audio > PV1_AUDIO_MAX_SAMPLES) {
        return -1;
    }
    return (int)audio;
}

const uint8_t *pv1_audio_data(const uint8_t *data, size_t len) {
    int audio = pv1_audio_len(data, len);
    if (audio <= 0) {
        return NULL;
    }
    pv1_header hdr;
    if (pv1_parse_header(data, len, &hdr) != 0) {
        return NULL;
    }
    return data + PV1_HEADER_LEN + (size_t)hdr.comp_len;
}

int pv1_keyframe_for(int frame) {
    if (frame <= 0) {
        return 0;
    }
    return frame - frame % PV1_KEYFRAME_INTERVAL;
}

int pv1_decode_key(const uint8_t *data, size_t len, uint16_t *pixels,
                   size_t cap, uint16_t *out_w, uint16_t *out_h) {
    pv1_header hdr;
    if (pv1_parse_header(data, len, &hdr) != 0) {
        return -1;
    }
    if (hdr.type != PV1_TYPE_KEY) {
        return -1;
    }
    size_t need = (size_t)hdr.width * (size_t)hdr.height;
    if (pixels == NULL || cap < need) {
        return -1;
    }
    int got = inflate_decompress(data + PV1_HEADER_LEN, hdr.comp_len,
                                 (uint8_t *)pixels, need * 2);
    if (got != (int)(need * 2)) {
        return -1;
    }
    if (out_w != NULL) {
        *out_w = hdr.width;
    }
    if (out_h != NULL) {
        *out_h = hdr.height;
    }
    return 0;
}

int pv1_decode_diff(const uint8_t *data, size_t len, uint16_t *pixels,
                    size_t cap, uint8_t *scratch, size_t scratch_cap,
                    uint16_t *out_w, uint16_t *out_h) {
    pv1_header hdr;
    if (pv1_parse_header(data, len, &hdr) != 0) {
        return -1;
    }
    if (hdr.type != PV1_TYPE_DIFF) {
        return -1;
    }
    size_t need = (size_t)hdr.width * (size_t)hdr.height;
    if (pixels == NULL || cap < need) {
        return -1;
    }
    if (scratch == NULL || scratch_cap < hdr.raw_len) {
        return -1;
    }
    int got = inflate_decompress(data + PV1_HEADER_LEN, hdr.comp_len,
                                 scratch, hdr.raw_len);
    if (got != (int)hdr.raw_len) {
        return -1;
    }
    /* 差分の操作列を前のフレームに当てる。 */
    size_t pos = 0;
    size_t off = 0;
    while (1) {
        if (off + 2 > hdr.raw_len) {
            return -1;
        }
        uint16_t skip = (uint16_t)(scratch[off] |
                                   ((uint16_t)scratch[off + 1] << 8));
        off += 2;
        if (skip == 0xFFFF) {
            break;
        }
        if (off + 2 > hdr.raw_len) {
            return -1;
        }
        uint16_t count = (uint16_t)(scratch[off] |
                                    ((uint16_t)scratch[off + 1] << 8));
        off += 2;
        pos += skip;
        if (pos + count > need) {
            return -1;
        }
        if (off + (size_t)count * 2 > hdr.raw_len) {
            return -1;
        }
        for (uint16_t k = 0; k < count; k++) {
            pixels[pos++] = (uint16_t)(scratch[off] |
                                       ((uint16_t)scratch[off + 1] << 8));
            off += 2;
        }
    }
    if (off != hdr.raw_len) {
        return -1;
    }
    if (out_w != NULL) {
        *out_w = hdr.width;
    }
    if (out_h != NULL) {
        *out_h = hdr.height;
    }
    return 0;
}

void video_init(video_player *p, int count) {
    p->count = count;
    p->frame = 0;
    p->playing = 0;
}

void video_advance(video_player *p) {
    if (p->count <= 0) {
        return;
    }
    p->frame++;
    if (p->frame >= p->count) {
        p->frame = 0;
    }
}

void video_seek(video_player *p, int delta) {
    if (p->count <= 0) {
        return;
    }
    p->frame += delta;
    if (p->frame < 0) {
        p->frame = 0;
    }
    if (p->frame >= p->count) {
        p->frame = p->count - 1;
    }
}

int64_t video_ticks_per_frame(int64_t ticks_per_sec) {
    if (ticks_per_sec <= 0) {
        return 1;
    }
    int64_t t = ticks_per_sec / VIDEO_FPS;
    return t > 0 ? t : 1;
}

int video_tick(video_player *p, int64_t now, int64_t *next_tick,
               int64_t ticks_per_frame) {
    if (!p->playing || p->count <= 0) {
        return 0;
    }
    if (now < *next_tick) {
        return 0;
    }
    video_advance(p);
    *next_tick = now + ticks_per_frame;
    return 1;
}
