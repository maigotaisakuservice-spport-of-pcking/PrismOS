/* PrismOS — DEFLATE 展開 (RFC 1951)。 */
#include "inflate.h"

#include <string.h>

#define INFLATE_WINDOW 32768
#define INFLATE_MAX_SYMBOLS 288
#define INFLATE_MAX_BITS 15

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos; /* 次に読むバイト */
    uint32_t bitbuf;
    int bitcnt; /* bitbuf にあるビット数 */
} bit_reader;

static int bits_read(bit_reader *r, int n, uint32_t *out) {
    while (r->bitcnt < n) {
        if (r->pos >= r->len) {
            return -1;
        }
        r->bitbuf |= (uint32_t)r->data[r->pos++] << r->bitcnt;
        r->bitcnt += 8;
    }
    *out = r->bitbuf & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
    r->bitbuf >>= n;
    r->bitcnt -= n;
    return 0;
}

/* ビットをバイト境界に揃える (格納ブロック用)。 */
static void bits_align(bit_reader *r) {
    int drop = r->bitcnt % 8;
    r->bitbuf >>= drop;
    r->bitcnt -= drop;
}

/* 正準ハフマン表。lengths[i] はシンボル i の符号長 (0 = 無し)。
 * 復号は符号長ごとに先頭符号と開始インデックスで数える。 */
typedef struct {
    uint16_t counts[INFLATE_MAX_BITS + 1];
    uint16_t symbols[INFLATE_MAX_SYMBOLS];
    int max_symbol;
} huffman;

static int huffman_build(huffman *h, const uint8_t *lengths, int n) {
    memset(h, 0, sizeof(*h));
    h->max_symbol = n;
    for (int i = 0; i < n; i++) {
        if (lengths[i] > INFLATE_MAX_BITS) {
            return -1;
        }
        h->counts[lengths[i]]++;
    }
    h->counts[0] = 0;
    /* 符号長の合計が 0 (空の表) は、使われない場合に限り許す。
     * 呼び出し側が「表が要るのに空」を見分ける。 */
    int offs[INFLATE_MAX_BITS + 2];
    offs[1] = 0;
    for (int len = 1; len <= INFLATE_MAX_BITS; len++) {
        offs[len + 1] = offs[len] + h->counts[len];
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i] != 0) {
            h->symbols[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
    return 0;
}

/* ハフマン符号を 1 つ読む。戻り値はシンボル、失敗は -1。 */
static int huffman_decode(bit_reader *r, const huffman *h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= INFLATE_MAX_BITS; len++) {
        uint32_t bit = 0;
        if (bits_read(r, 1, &bit) != 0) {
            return -1;
        }
        code |= (int)bit;
        int count = h->counts[len];
        if (code - first < count) {
            return h->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* 固定ハフマン表 (RFC 1951 §3.2.6)。 */
static int huffman_fixed(huffman *lit, huffman *dist) {
    uint8_t lit_lengths[288];
    uint8_t dist_lengths[32];
    for (int i = 0; i < 144; i++) {
        lit_lengths[i] = 8;
    }
    for (int i = 144; i < 256; i++) {
        lit_lengths[i] = 9;
    }
    for (int i = 256; i < 280; i++) {
        lit_lengths[i] = 7;
    }
    for (int i = 280; i < 288; i++) {
        lit_lengths[i] = 8;
    }
    for (int i = 0; i < 32; i++) {
        dist_lengths[i] = 5;
    }
    if (huffman_build(lit, lit_lengths, 288) != 0) {
        return -1;
    }
    if (huffman_build(dist, dist_lengths, 32) != 0) {
        return -1;
    }
    return 0;
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

/* 動的ハフマン表を読む。 */
static int huffman_dynamic(bit_reader *r, huffman *lit, huffman *dist) {
    static const uint8_t ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6,
                                       10, 5, 11, 4, 12, 3, 13, 2,
                                       14, 1, 15 };
    uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (bits_read(r, 5, &hlit) != 0 || bits_read(r, 5, &hdist) != 0 ||
        bits_read(r, 4, &hclen) != 0) {
        return -1;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 288 || hdist > 32) {
        return -1;
    }
    uint8_t cl_lengths[19];
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (uint32_t i = 0; i < hclen; i++) {
        uint32_t v = 0;
        if (bits_read(r, 3, &v) != 0) {
            return -1;
        }
        cl_lengths[ORDER[i]] = (uint8_t)v;
    }
    huffman cl;
    if (huffman_build(&cl, cl_lengths, 19) != 0) {
        return -1;
    }
    uint8_t lengths[288 + 32];
    memset(lengths, 0, sizeof(lengths));
    uint32_t total = hlit + hdist;
    uint32_t i = 0;
    while (i < total) {
        int sym = huffman_decode(r, &cl);
        if (sym < 0) {
            return -1;
        }
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) {
                return -1;
            }
            uint32_t rep = 0;
            if (bits_read(r, 2, &rep) != 0) {
                return -1;
            }
            rep += 3;
            if (i + rep > total) {
                return -1;
            }
            uint8_t prev = lengths[i - 1];
            while (rep-- > 0) {
                lengths[i++] = prev;
            }
        } else if (sym == 17) {
            uint32_t rep = 0;
            if (bits_read(r, 3, &rep) != 0) {
                return -1;
            }
            rep += 3;
            if (i + rep > total) {
                return -1;
            }
            while (rep-- > 0) {
                lengths[i++] = 0;
            }
        } else if (sym == 18) {
            uint32_t rep = 0;
            if (bits_read(r, 7, &rep) != 0) {
                return -1;
            }
            rep += 11;
            if (i + rep > total) {
                return -1;
            }
            while (rep-- > 0) {
                lengths[i++] = 0;
            }
        } else {
            return -1;
        }
    }
    /* 長さ 0 の表は「その表を使わない」ことを意味する。 */
    if (huffman_build(lit, lengths, (int)hlit) != 0) {
        return -1;
    }
    if (huffman_build(dist, lengths + hlit, (int)hdist) != 0) {
        return -1;
    }
    return 0;
}

int inflate_decompress(const uint8_t *src, size_t src_len, uint8_t *out,
                       size_t cap) {
    if (src == NULL || out == NULL) {
        return -1;
    }
    static uint8_t window[INFLATE_WINDOW];
    size_t win_pos = 0; /* 次に書く履歴の位置 */
    bit_reader r;
    r.data = src;
    r.len = src_len;
    r.pos = 0;
    r.bitbuf = 0;
    r.bitcnt = 0;
    size_t out_len = 0;

    for (;;) {
        uint32_t bfinal = 0, btype = 0;
        if (bits_read(&r, 1, &bfinal) != 0 ||
            bits_read(&r, 2, &btype) != 0) {
            return -1;
        }
        if (btype == 0) {
            /* 格納ブロック: バイト境界に揃えて LEN/NLEN を読む。 */
            bits_align(&r);
            if (r.pos + 4 > r.len) {
                return -1;
            }
            uint32_t len = r.data[r.pos] | ((uint32_t)r.data[r.pos + 1] << 8);
            uint32_t nlen = r.data[r.pos + 2] |
                            ((uint32_t)r.data[r.pos + 3] << 8);
            r.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) {
                return -1;
            }
            if (r.pos + len > r.len || out_len + len > cap) {
                return -1;
            }
            for (uint32_t i = 0; i < len; i++) {
                uint8_t b = r.data[r.pos++];
                out[out_len++] = b;
                window[win_pos] = b;
                win_pos = (win_pos + 1) % INFLATE_WINDOW;
            }
        } else if (btype == 1 || btype == 2) {
            huffman lit, dist;
            if (btype == 1) {
                if (huffman_fixed(&lit, &dist) != 0) {
                    return -1;
                }
            } else {
                if (huffman_dynamic(&r, &lit, &dist) != 0) {
                    return -1;
                }
            }
            for (;;) {
                int sym = huffman_decode(&r, &lit);
                if (sym < 0) {
                    return -1;
                }
                if (sym < 256) {
                    if (out_len >= cap) {
                        return -1;
                    }
                    out[out_len++] = (uint8_t)sym;
                    window[win_pos] = (uint8_t)sym;
                    win_pos = (win_pos + 1) % INFLATE_WINDOW;
                } else if (sym == 256) {
                    break; /* ブロックの終わり */
                } else {
                    int li = sym - 257;
                    if (li < 0 || li >= 29) {
                        return -1;
                    }
                    uint32_t extra = 0;
                    if (LENGTH_EXTRA[li] > 0 &&
                        bits_read(&r, LENGTH_EXTRA[li], &extra) != 0) {
                        return -1;
                    }
                    size_t length = LENGTH_BASE[li] + extra;
                    int dsym = huffman_decode(&r, &dist);
                    if (dsym < 0 || dsym >= 30) {
                        return -1;
                    }
                    extra = 0;
                    if (DIST_EXTRA[dsym] > 0 &&
                        bits_read(&r, DIST_EXTRA[dsym], &extra) != 0) {
                        return -1;
                    }
                    size_t distance = DIST_BASE[dsym] + extra;
                    if (distance == 0 || distance > out_len) {
                        return -1;
                    }
                    if (out_len + length > cap) {
                        return -1;
                    }
                    /* 距離が長さより短い (重なるコピー) にも対応
                     * するため 1 バイトずつコピーする。 */
                    for (size_t i = 0; i < length; i++) {
                        uint8_t b =
                            window[(win_pos + INFLATE_WINDOW - distance) %
                                   INFLATE_WINDOW];
                        out[out_len++] = b;
                        window[win_pos] = b;
                        win_pos = (win_pos + 1) % INFLATE_WINDOW;
                    }
                }
            }
        } else {
            return -1; /* BTYPE == 3 は予約済み */
        }
        if (bfinal) {
            return (int)out_len;
        }
    }
}
