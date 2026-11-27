/* PrismOS — USB ビデオクラス (UVC) ドライバ。
 *
 * USB Video Class 1.1 の仕様に基づいて実装する:
 *  - VideoControl (VC) / VideoStreaming (VS) インタフェース
 *    記述子を走査してフォーマットとフレーム記述子を集める
 *  - VS Probe/Commit 制御 (GET_CUR / SET_CUR) で形式と
 *    フレーム間隔を取り決める
 *  - 代替設定を選んでアイソクロナス転送を開始し、ペイ
 *    ロードヘッダ (FID/EOF) を見てフレームを組み立てる
 *
 * 検証状態 (正直に): QEMU メインラインには UVC の
 * エミュレーションが無いため、実転送は未検証。記述子の
 * 解析・Probe/Commit のバイト列・フレーム組立はホスト
 * テストで検証する (tests/test_uvc.c)。デバイス列挙と
 * 制御転送の配線は xhci.c のストレージと同じ経路を使う。 */
#ifndef PRISM_UVC_H
#define PRISM_UVC_H

#include <stddef.h>
#include <stdint.h>

#define UVC_MAX_FORMATS 8
#define UVC_MAX_FRAMES 16

typedef struct {
    uint8_t format_index;
    uint8_t guid_format[16]; /* MJPG / YUY2 の GUID */
    int is_mjpeg;
    uint8_t frame_index;
    uint16_t width;
    uint16_t height;
    uint32_t default_interval; /* 100ns 単位 */
    uint32_t max_frame_size;
} uvc_format_frame;

/* 構成記述子全体を走査して VS の形式/フレームを集める。
 * 戻り値 = 集めた数。vc_iface/vs_iface にインタフェース
 * 番号、stream_ep/stream_max_packet にバルク/アイソク
 * ロナスの IN エンドポイント情報を返す (あれば)。 */
int uvc_parse_config(const uint8_t *cfg, size_t len,
                     uvc_format_frame *out, int max_out,
                     int *vc_iface, int *vs_iface,
                     uint8_t *stream_ep, uint16_t *stream_max_packet,
                     uint8_t *vs_alt_with_ep,
                     uint8_t *stream_interval);

/* VS Probe/Commit のペイロード (UVC 1.1: 26 バイト) を
 * 作る。hint/format/frame/interval を入れる。 */
void uvc_build_probe(uint8_t out[26], uint8_t format_index,
                     uint8_t frame_index, uint32_t interval_100ns);

/* Probe 応答から、デバイスが選んだ最大フレームサイズと
 * 間隔を取り出す。 */
void uvc_parse_probe(const uint8_t in[26], uint32_t *max_frame_size,
                     uint32_t *interval_100ns, uint16_t *payload_size);

/* ペイロードヘッダ: bHeaderLength, bmHeaderInfo
 * (bit0 FID, bit1 EOF, bit6 ERR) の解釈。 */
typedef struct {
    uint8_t header_len;
    int fid;
    int eof;
    int err;
} uvc_payload_header;
int uvc_parse_payload_header(const uint8_t *buf, size_t len,
                             uvc_payload_header *hdr);

/* フレーム組立器: 転送チャンクを順に与えると EOF で
 * 1 フレームが完成する。FID が変わったら途中でも
 * 前フレームを打ち切る (仕様どおり)。 */
typedef struct {
    uint8_t *frame;
    uint32_t capacity;
    uint32_t used;
    int cur_fid; /* -1 = 未開始 */
    int complete;
    uint32_t frames_done;
    uint32_t frames_dropped;
} uvc_assembler;
void uvc_asm_init(uvc_assembler *a, uint8_t *frame_buf,
                  uint32_t capacity);
/* チャンクを 1 つ与える。フレームが完成したら 1。 */
int uvc_asm_feed(uvc_assembler *a, const uint8_t *chunk,
                 size_t len);

/* ---- デバイス側 (xhci.c が実装) ---- */
int uvc_present(void);
int uvc_format_count(void);
const uvc_format_frame *uvc_format_at(int i);
/* 取り決め + ストリーミング開始。成功 0。 */
int uvc_start(int format_idx);
/* 完成したフレームがあれば長さを返し buf にコピー。 */
int uvc_poll_frame(uint8_t *buf, uint32_t cap);
void uvc_stop(void);

#endif /* PRISM_UVC_H */
