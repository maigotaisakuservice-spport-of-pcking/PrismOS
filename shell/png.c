/* PrismOS — PNG デコーダ。png.h の範囲コメント参照。 */
#include "png.h"

#include "inflate.h"

#define PNG_IDAT_CAP 262144 /* 圧縮データの上限 (取得上限と同じ) */
#define PNG_RAW_CAP 1048576 /* 展開後スキャンラインの上限 */

static uint8_t png_idat[PNG_IDAT_CAP];
static uint8_t png_raw[PNG_RAW_CAP];
static uint8_t png_plte[256 * 3];
static uint8_t png_trns_gray[2];
static int png_trns_gray_set;

static uint32_t png_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int png_is_png(const uint8_t *data, size_t len) {
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47,
                                   0x0D, 0x0A, 0x1A, 0x0A};
    if (data == NULL || len < 8) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (data[i] != sig[i]) {
            return 0;
        }
    }
    return 1;
}

static int png_channels(int color_type) {
    switch (color_type) {
        case 0:
            return 1;
        case 2:
            return 3;
        case 3:
            return 1;
        case 4:
            return 2;
        case 6:
            return 4;
        default:
            return 0;
    }
}

static int png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

/* 1 ピクセル分のサンプルを取り出す。row はフィルタ復元後の
 * 行先頭、bit は左端からのピクセル番号。深度 16 は上位
 * バイト、深度 8 はそのまま、パレット (1/2/4) はインデックス。 */
static int png_sample(const uint8_t *row, int x, int depth,
                      int channel) {
    if (depth == 16) {
        return row[x * 2 + channel * 2];
    }
    if (depth == 8) {
        return row[x + channel];
    }
    /* depth 1/2/4: パレットのインデックス抽出。 */
    int bits = x * depth;
    int byte = row[bits / 8];
    int shift = 8 - depth - (bits % 8);
    int mask = (1 << depth) - 1;
    return (byte >> shift) & mask;
}

int png_decode(const uint8_t *data, size_t len, uint8_t *out,
               size_t out_cap, int max_w, int max_h, int *w_out,
               int *h_out) {
    if (!png_is_png(data, len) || out == NULL || w_out == NULL ||
        h_out == NULL || max_w < 1 || max_h < 1) {
        return -1;
    }
    int width = 0, height = 0, depth = 0, ctype = -1;
    int plte_len = 0;
    size_t idat_len = 0;
    png_trns_gray_set = 0;
    int have_ihdr = 0;

    size_t pos = 8;
    while (pos + 8 <= len) {
        uint32_t clen = png_rd32(data + pos);
        const uint8_t *ctype_p = data + pos + 4;
        const uint8_t *cdata = data + pos + 8;
        if ((uint64_t)pos + 8 + clen + 4 > (uint64_t)len) {
            return -1;
        }
        if (ctype_p[0] == 'I' && ctype_p[1] == 'H' &&
            ctype_p[2] == 'D' && ctype_p[3] == 'R') {
            if (clen != 13) {
                return -1;
            }
            width = (int)png_rd32(cdata);
            height = (int)png_rd32(cdata + 4);
            depth = cdata[8];
            ctype = cdata[9];
            if (width < 1 || height < 1 || cdata[10] != 0 ||
                cdata[11] != 0) {
                return -1;
            }
            if (cdata[12] != 0) {
                return -2; /* Adam7 インターレース */
            }
            have_ihdr = 1;
        } else if (ctype_p[0] == 'P' && ctype_p[1] == 'L' &&
                   ctype_p[2] == 'T' && ctype_p[3] == 'E') {
            if (clen > sizeof(png_plte) || clen % 3 != 0) {
                return -1;
            }
            for (uint32_t i = 0; i < clen; i++) {
                png_plte[i] = cdata[i];
            }
            plte_len = (int)clen / 3;
        } else if (ctype_p[0] == 't' && ctype_p[1] == 'R' &&
                   ctype_p[2] == 'N' && ctype_p[3] == 'S') {
            if (ctype == 0 && clen == 2) {
                png_trns_gray[0] = cdata[0];
                png_trns_gray[1] = cdata[1];
                png_trns_gray_set = 1;
            }
            /* パレット/RGB の tRNS は未対応 (αなしとして扱う)。 */
        } else if (ctype_p[0] == 'I' && ctype_p[1] == 'D' &&
                   ctype_p[2] == 'A' && ctype_p[3] == 'T') {
            if (idat_len + clen > PNG_IDAT_CAP) {
                return -3;
            }
            for (uint32_t i = 0; i < clen; i++) {
                png_idat[idat_len + i] = cdata[i];
            }
            idat_len += clen;
        } else if (ctype_p[0] == 'I' && ctype_p[1] == 'E' &&
                   ctype_p[2] == 'N' && ctype_p[3] == 'D') {
            break;
        }
        pos += 8 + clen + 4;
    }
    if (!have_ihdr || idat_len < 2) {
        return -1;
    }
    int channels = png_channels(ctype);
    if (channels == 0) {
        return -1;
    }
    if (ctype == 3) {
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8) {
            return -1;
        }
        if (plte_len == 0) {
            return -1;
        }
    } else {
        if (depth != 8 && depth != 16) {
            return -1;
        }
    }
    /* 1 行のバイト数 (フィルタバイト除く)。 */
    uint64_t bits_per_px =
        (uint64_t)channels * (uint64_t)(ctype == 3 ? depth : depth);
    uint64_t row_bytes =
        ((uint64_t)width * bits_per_px + 7) / 8;
    uint64_t raw_total = (row_bytes + 1) * (uint64_t)height;
    if (raw_total > PNG_RAW_CAP) {
        return -3;
    }
    /* zlib ストリーム: 先頭 2 バイト (CMF/FLG) を飛ばして
     * 生 DEFLATE を inflate する。末尾の Adler-32 は見ない。 */
    int got = inflate_decompress(png_idat + 2, idat_len - 2,
                                 png_raw, PNG_RAW_CAP);
    if (got < 0 || (uint64_t)got < raw_total) {
        return -1;
    }
    /* フィルタ復元 (その場で)。bpp はフィルタ単位のバイト数。 */
    int fbpp = (int)((bits_per_px + 7) / 8);
    if (fbpp < 1) {
        fbpp = 1;
    }
    for (int y = 0; y < height; y++) {
        uint8_t *row = png_raw + (size_t)y * (size_t)(row_bytes + 1) + 1;
        const uint8_t *prev =
            y > 0 ? png_raw + (size_t)(y - 1) * (size_t)(row_bytes + 1) +
                        1
                  : NULL;
        int filter = png_raw[(size_t)y * (size_t)(row_bytes + 1)];
        for (uint64_t i = 0; i < row_bytes; i++) {
            int a = i >= (uint64_t)fbpp ? row[i - fbpp] : 0;
            int b = prev != NULL ? prev[i] : 0;
            int c = (prev != NULL && i >= (uint64_t)fbpp)
                        ? prev[i - fbpp]
                        : 0;
            int v = row[i];
            switch (filter) {
                case 0:
                    break;
                case 1:
                    v = (v + a) & 0xFF;
                    break;
                case 2:
                    v = (v + b) & 0xFF;
                    break;
                case 3:
                    v = (v + (a + b) / 2) & 0xFF;
                    break;
                case 4:
                    v = (v + png_paeth(a, b, c)) & 0xFF;
                    break;
                default:
                    return -1;
            }
            row[i] = (uint8_t)v;
        }
    }
    /* 出力寸法 (最近傍縮小)。 */
    int dw = width, dh = height;
    if (dw > max_w) {
        dh = (int)((int64_t)dh * max_w / dw);
        dw = max_w;
    }
    if (dh > max_h) {
        dw = (int)((int64_t)dw * max_h / dh);
        dh = max_h;
    }
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    if ((uint64_t)dw * (uint64_t)dh * 3 > (uint64_t)out_cap) {
        return -3;
    }
    int sample_depth = ctype == 3 ? depth : depth;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * height / dh);
        const uint8_t *row =
            png_raw + (size_t)sy * (size_t)(row_bytes + 1) + 1;
        /* 深度16の行内オフセットはサンプル単位で計算する。 */
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * width / dw);
            int r = 0, g = 0, b = 0, alpha = 255;
            if (ctype == 3) {
                int idx = png_sample(row, sx, depth, 0);
                if (idx >= plte_len) {
                    idx = 0;
                }
                r = png_plte[idx * 3];
                g = png_plte[idx * 3 + 1];
                b = png_plte[idx * 3 + 2];
            } else if (ctype == 0) {
                int v = sample_depth == 16
                            ? png_sample(row, sx * channels, 16, 0)
                            : row[sx * channels];
                r = g = b = v;
                if (png_trns_gray_set && depth == 16 &&
                    row[sx * 2] == png_trns_gray[0] &&
                    row[sx * 2 + 1] == png_trns_gray[1]) {
                    alpha = 0;
                }
            } else if (ctype == 2) {
                if (sample_depth == 16) {
                    r = row[sx * 6];
                    g = row[sx * 6 + 2];
                    b = row[sx * 6 + 4];
                } else {
                    r = row[sx * 3];
                    g = row[sx * 3 + 1];
                    b = row[sx * 3 + 2];
                }
            } else if (ctype == 4) {
                if (sample_depth == 16) {
                    r = g = b = row[sx * 4];
                    alpha = row[sx * 4 + 2];
                } else {
                    r = g = b = row[sx * 2];
                    alpha = row[sx * 2 + 1];
                }
            } else { /* 6: RGBA */
                if (sample_depth == 16) {
                    r = row[sx * 8];
                    g = row[sx * 8 + 2];
                    b = row[sx * 8 + 4];
                    alpha = row[sx * 8 + 6];
                } else {
                    r = row[sx * 4];
                    g = row[sx * 4 + 1];
                    b = row[sx * 4 + 2];
                    alpha = row[sx * 4 + 3];
                }
            }
            if (alpha < 255) {
                /* 白地に合成する。 */
                r = (r * alpha + 255 * (255 - alpha)) / 255;
                g = (g * alpha + 255 * (255 - alpha)) / 255;
                b = (b * alpha + 255 * (255 - alpha)) / 255;
            }
            uint8_t *dst = out + ((size_t)y * (size_t)dw + (size_t)x) * 3;
            dst[0] = (uint8_t)b;
            dst[1] = (uint8_t)g;
            dst[2] = (uint8_t)r;
        }
    }
    *w_out = dw;
    *h_out = dh;
    return 0;
}
