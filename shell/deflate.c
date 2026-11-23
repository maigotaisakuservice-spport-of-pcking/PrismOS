/* PrismOS — DEFLATE 圧縮 (固定ハフマン + LZ77)。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。ZIP の書き込みで
 * .pptx の XML を圧縮するために必要。
 *
 * 1 つの固定ハフマンブロックを出す。3 バイトのハッシュで
 * 直近の一致位置を探し、長さ 3 以上の一致を長さ・距離の組
 * で出す (貪欲法)。XML のような繰り返しの多いテキストは
 * 3〜4 割に縮む。圧縮しても縮まない入力には使う意味が
 * ないので、呼び出し側が元の大きさと比べて選ぶこと。
 * 出力は正しい DEFLATE ストリームで、どんな ZIP 読み手
 * でも展開できる。
 */
#include "deflate.h"

#define DEFLATE_WINDOW 32768
#define DEFLATE_HASH_BITS 12
#define DEFLATE_HASH_SIZE (1 << DEFLATE_HASH_BITS)
#define DEFLATE_MAX_MATCH 258
#define DEFLATE_MIN_MATCH 3

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t len;
    uint32_t bitbuf;
    int bitcnt;
} bit_writer;

static int bits_write(bit_writer *w, uint32_t value, int n) {
    w->bitbuf |= value << w->bitcnt;
    w->bitcnt += n;
    while (w->bitcnt >= 8) {
        if (w->len >= w->cap) {
            return -1;
        }
        w->out[w->len++] = (uint8_t)(w->bitbuf & 0xFF);
        w->bitbuf >>= 8;
        w->bitcnt -= 8;
    }
    return 0;
}

static int bits_flush(bit_writer *w) {
    if (w->bitcnt > 0) {
        if (w->len >= w->cap) {
            return -1;
        }
        w->out[w->len++] = (uint8_t)(w->bitbuf & 0xFF);
        w->bitbuf = 0;
        w->bitcnt = 0;
    }
    return 0;
}

/* 符号のビットを反転する (DEFLATE はハフマン符号を
 * 最上位ビットから書く)。 */
static uint32_t reverse_bits(uint32_t code, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        r = (r << 1) | (code & 1u);
        code >>= 1;
    }
    return r;
}

/* リテラル・長さシンボルの固定ハフマン符号を書く。 */
static int write_litlen(bit_writer *w, int sym) {
    uint32_t code = 0;
    int nbits = 0;
    if (sym < 144) {
        code = 0x30u + (uint32_t)sym;
        nbits = 8;
    } else if (sym < 256) {
        code = 0x190u + (uint32_t)(sym - 144);
        nbits = 9;
    } else if (sym < 280) {
        code = (uint32_t)(sym - 256);
        nbits = 7;
    } else {
        code = 0xC0u + (uint32_t)(sym - 280);
        nbits = 8;
    }
    return bits_write(w, reverse_bits(code, nbits), nbits);
}

static const uint16_t LENGTH_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* 長さから長さシンボルを引く。 */
static int length_symbol(size_t length) {
    for (int i = 28; i >= 0; i--) {
        if (length >= LENGTH_BASE[i]) {
            return 257 + i;
        }
    }
    return 257;
}

/* 距離から距離シンボルを引く。 */
static int dist_symbol(size_t dist) {
    for (int i = 29; i >= 0; i--) {
        if (dist >= DIST_BASE[i]) {
            return i;
        }
    }
    return 0;
}

static uint32_t hash3(const uint8_t *p) {
    uint32_t h = ((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^
                 (uint32_t)p[2];
    return h & (DEFLATE_HASH_SIZE - 1);
}

int deflate_compress_fixed(const uint8_t *src, size_t len, uint8_t *out,
                           size_t cap) {
    if (src == NULL || out == NULL) {
        return -1;
    }
    static int head[DEFLATE_HASH_SIZE];
    static int prev_pos[DEFLATE_WINDOW];
    for (int i = 0; i < DEFLATE_HASH_SIZE; i++) {
        head[i] = -1;
    }
    bit_writer w;
    w.out = out;
    w.cap = cap;
    w.len = 0;
    w.bitbuf = 0;
    w.bitcnt = 0;
    /* BFINAL=1, BTYPE=01 (固定ハフマン)。3 ビットの値は
     * BFINAL | (BTYPE << 1) = 3。 */
    if (bits_write(&w, 3, 3) != 0) {
        return -1;
    }
    size_t pos = 0;
    while (pos < len) {
        size_t best_len = 0;
        size_t best_dist = 0;
        if (pos + DEFLATE_MIN_MATCH <= len) {
            uint32_t h = hash3(src + pos);
            /* ハッシュ鎖をたどる (最大 32 候補)。 */
            int cand = head[h];
            int tried = 0;
            while (cand >= 0 && tried < 32) {
                size_t dist = pos - (size_t)cand;
                if (dist > DEFLATE_WINDOW) {
                    break;
                }
                size_t max_len = len - pos;
                if (max_len > DEFLATE_MAX_MATCH) {
                    max_len = DEFLATE_MAX_MATCH;
                }
                size_t l = 0;
                while (l < max_len &&
                       src[(size_t)cand + l] == src[pos + l]) {
                    l++;
                }
                if (l >= DEFLATE_MIN_MATCH && l > best_len) {
                    best_len = l;
                    best_dist = dist;
                    if (l >= DEFLATE_MAX_MATCH) {
                        break;
                    }
                }
                cand = prev_pos[(size_t)cand % DEFLATE_WINDOW];
                tried++;
            }
            /* この位置をハッシュ鎖に入れる。 */
            prev_pos[pos % DEFLATE_WINDOW] = head[h];
            head[h] = (int)pos;
        }
        if (best_len >= DEFLATE_MIN_MATCH) {
            int sym = length_symbol(best_len);
            int li = sym - 257;
            if (write_litlen(&w, sym) != 0) {
                return -1;
            }
            uint32_t extra =
                (uint32_t)(best_len - LENGTH_BASE[li]);
            if (LENGTH_EXTRA[li] > 0 &&
                bits_write(&w, extra, LENGTH_EXTRA[li]) != 0) {
                return -1;
            }
            int dsym = dist_symbol(best_dist);
            /* 距離シンボルは 5 ビットの固定符号。 */
            if (bits_write(&w, reverse_bits((uint32_t)dsym, 5), 5) !=
                0) {
                return -1;
            }
            extra = (uint32_t)(best_dist - DIST_BASE[dsym]);
            if (DIST_EXTRA[dsym] > 0 &&
                bits_write(&w, extra, DIST_EXTRA[dsym]) != 0) {
                return -1;
            }
            /* 一致した範囲の途中位置もハッシュに入れる。 */
            for (size_t k = 1; k < best_len; k++) {
                size_t p = pos + k;
                if (p + DEFLATE_MIN_MATCH <= len) {
                    uint32_t hh = hash3(src + p);
                    prev_pos[p % DEFLATE_WINDOW] = head[hh];
                    head[hh] = (int)p;
                }
            }
            pos += best_len;
        } else {
            if (write_litlen(&w, src[pos]) != 0) {
                return -1;
            }
            pos++;
        }
    }
    /* ブロック終了 (シンボル 256、7 ビットの 0)。 */
    if (write_litlen(&w, 256) != 0) {
        return -1;
    }
    if (bits_flush(&w) != 0) {
        return -1;
    }
    return (int)w.len;
}
