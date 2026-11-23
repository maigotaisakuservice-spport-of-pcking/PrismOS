/* PrismOS — 動画プレイヤーの純粋ロジック。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない。
 *
 * 動画は VFS のフレームファイルの連番。2 つの形式を読む。
 * 新しい PV1 形式 ("vid-01.pv1" …) は本物の圧縮動画で、
 * 古い BMP 連番 ("vid-01.bmp" …) も引き続き読める。
 *
 * PV1 (1 ファイル = 1 フレーム、1 ファイル 16384 バイトの
 * 上限に収まる。scripts/encode-video.py が作る):
 *     0  magic "PRV1" (4 バイト)
 *     4  u16 width (リトルエンディアン)
 *     6  u16 height
 *     8  u8  type: 0 = キーフレーム、1 = 差分フレーム
 *     9  u8  予約 (0)
 *    10  u32 raw_len  (圧縮前のバイト数)
 *    14  u32 comp_len (圧縮後のバイト数)
 *    18  comp_len バイトの raw DEFLATE ストリーム
 *    18 + comp_len  以降 (任意): 音声トラック。8 ビット符号なし
 *         PCM (128 が無音)、8000 Hz、1 フレーム分 (4 fps なら
 *         2000 サンプル = 0.25 秒)。音声のない古い PV1 は
 *         18 + comp_len バイトで終わり、そのまま読める。
 *         2000 サンプルを超える音声は付けない (音声の再生は
 *         4096 サンプルまでの転送バッファを使うため)。
 * キーフレームのデータは width*height 個の RGB565 (LE)
 * 画素。差分フレームのデータは { u16 skip, u16 count,
 * count 個の RGB565 画素 } の繰り返しで、skip == 0xFFFF
 * で終わり。前のフレームに skip だけ飛ばして count 個
 * 上書きする。色は RGB565 (1 画素 2 バイト) に落とすことと、
 * フレーム間差分と DEFLATE の 3 段で圧縮する。
 * キーフレームは 8 フレームごと。再生は 4 fps。
 */
#ifndef PRISM_VIDEO_H
#define PRISM_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#define VIDEO_FRAME_PREFIX "vid-"
#define VIDEO_FRAME_SUFFIX ".bmp"
#define VIDEO_PV1_SUFFIX ".pv1"
#define VIDEO_FPS 4
#define VIDEO_MAX_FRAMES 32

/* PV1 形式。 */
#define PV1_MAGIC "PRV1"
#define PV1_MAGIC_LEN 4
#define PV1_HEADER_LEN 18
#define PV1_TYPE_KEY 0
#define PV1_TYPE_DIFF 1
#define PV1_KEYFRAME_INTERVAL 8
#define PV1_MAX_WIDTH 320
#define PV1_MAX_HEIGHT 240
#define PV1_MAX_PIXELS (PV1_MAX_WIDTH * PV1_MAX_HEIGHT)

/* PV1 の音声トラック: 8 ビット符号なし PCM、8000 Hz。
 * 1 フレームは 4 fps で 0.25 秒なので 2000 サンプル。 */
#define PV1_AUDIO_RATE 8000
#define PV1_AUDIO_SAMPLES_PER_FRAME (PV1_AUDIO_RATE / VIDEO_FPS)
#define PV1_AUDIO_MAX_SAMPLES 4096

/* 名前の一覧から "vid-*.bmp" のものだけを集め、名前順に
 * 並べる。names[i] は VFS のファイル名。out には names の
 * 添字を入れる。戻り値は集めたフレーム数。 */
int video_collect(const char **names, int count, int *out, int max);

/* video_collect と同じだが "vid-*.pv1" を集める。 */
int video_collect_pv1(const char **names, int count, int *out, int max);

/* 再生の状態。 */
typedef struct {
    int count;   /* フレーム総数 */
    int frame;   /* 現在のフレーム (0 起点) */
    int playing; /* 1 = 再生中 */
} video_player;

void video_init(video_player *p, int count);

/* 次のフレームへ (最後なら先頭に戻る)。 */
void video_advance(video_player *p);

/* delta フレームだけ移動する (負は後ろへ)。端では止まる。 */
void video_seek(video_player *p, int delta);

/* 1 秒あたりの tick 回数から、1 フレームあたりの tick 回数を
 * 返す。tick 回数が分からなければ 1 を返す。 */
int64_t video_ticks_per_frame(int64_t ticks_per_sec);

/* 再生中で、now が next_tick に達していれば次のフレームへ
 * 進み、next_tick を 1 フレーム分先へ動かして 1 を返す。
 * そうでなければ 0。 */
int video_tick(video_player *p, int64_t now, int64_t *next_tick,
               int64_t ticks_per_frame);

/* PV1 フレームのヘッダ。 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t type;     /* 0 = キーフレーム、1 = 差分フレーム */
    uint32_t raw_len; /* 圧縮前のバイト数 */
    uint32_t comp_len; /* 圧縮後のバイト数 */
} pv1_header;

/* data (len バイト) が PV1 フレームならヘッダを out に入れて
 * 0 を返す。違えば -1。ヘッダのあとに音声トラックが付いて
 * いてもよい (長さは pv1_audio_len で読む)。 */
int pv1_parse_header(const uint8_t *data, size_t len, pv1_header *out);

/* data の PV1 フレームに付いた音声トラックのサンプル数を
 * 返す。音声がなければ 0。ヘッダが壊れているか、音声が
 * PV1_AUDIO_MAX_SAMPLES を超えていれば -1。 */
int pv1_audio_len(const uint8_t *data, size_t len);

/* data の PV1 フレームに付いた音声トラックの先頭を返す。
 * 音声がなければ NULL。壊れたヘッダや長すぎる音声でも
 * NULL を返す。戻り値は data の中を指す (コピーしない)。 */
const uint8_t *pv1_audio_data(const uint8_t *data, size_t len);

/* frame の直前のキーフレームの番号を返す (0 起点)。
 * frame が 0 なら 0。 */
int pv1_keyframe_for(int frame);

/* PV1 キーフレームを pixels (cap 画素分) に展開する。
 * data は PV1 ファイル全体。成功すれば 0、失敗すれば -1。
 * 幅・高さは out_w/out_h (NULL 可) に入る。 */
int pv1_decode_key(const uint8_t *data, size_t len, uint16_t *pixels,
                   size_t cap, uint16_t *out_w, uint16_t *out_h);

/* PV1 差分フレームを pixels (cap 画素分、現在のフレーム) に
 * 適用する。scratch は展開作業用の領域 (raw_len バイト以上
 * が要る。足りなければ -1)。成功すれば 0、失敗すれば -1。
 * 幅・高さは out_w/out_h (NULL 可) に入る。 */
int pv1_decode_diff(const uint8_t *data, size_t len, uint16_t *pixels,
                    size_t cap, uint8_t *scratch, size_t scratch_cap,
                    uint16_t *out_w, uint16_t *out_h);

#endif /* PRISM_VIDEO_H */
