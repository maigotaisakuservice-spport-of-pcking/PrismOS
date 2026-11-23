/* PrismOS — USB ビデオクラス (UVC) ドライバの共通部。
 *
 * 記述子解析・Probe/Commit・フレーム組立はここ (純粋
 * ロジック、ホストテスト対象)。デバイス転送の配線は
 * xhci.c 側にあり、uvc_present() などを実装する。
 * デバイスが無い環境では present は 0 を返す。 */
#include "uvc.h"

#include <string.h>

#define UVC_CLASS 0x0E
#define UVC_SC_VIDEOCONTROL 0x01
#define UVC_SC_VIDEOSTREAMING 0x02

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 構成記述子を走査する。VS インタフェースのクラス固有
 * 記述子:
 *   VS_FORMAT_UNCOMPRESSED = 0x04 (bFormatIndex, guidFormat)
 *   VS_FORMAT_MJPEG        = 0x06 (bFormatIndex)
 *   VS_FRAME_UNCOMPRESSED  = 0x05
 *   VS_FRAME_MJPEG         = 0x07
 * フレーム記述子: +3 wWidth, +5 wHeight,
 *   +17 dwMaxVideoFrameBufferSize, +21 dwDefaultFrameInterval
 * 形式 1 つにフレームが複数ぶら下がるので、(形式,フレーム)
 * の組ごとに出力する。 */
int uvc_parse_config(const uint8_t *cfg, size_t len,
                     uvc_format_frame *out, int max_out,
                     int *vc_iface, int *vs_iface,
                     uint8_t *stream_ep, uint16_t *stream_max_packet,
                     uint8_t *vs_alt_with_ep,
                     uint8_t *stream_interval) {
    int count = 0;
    int cur_alt = 0;
    int in_vs = 0;
    uint8_t pending_format_index = 0;
    int pending_mjpeg = 0;
    uint8_t pending_guid[16];
    int have_pending = 0;
    memset(pending_guid, 0, sizeof(pending_guid));
    if (vc_iface) {
        *vc_iface = -1;
    }
    if (vs_iface) {
        *vs_iface = -1;
    }
    if (stream_ep) {
        *stream_ep = 0;
    }
    if (stream_max_packet) {
        *stream_max_packet = 0;
    }
    if (vs_alt_with_ep) {
        *vs_alt_with_ep = 0;
    }
    if (stream_interval) {
        *stream_interval = 0;
    }
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t blen = cfg[i];
        uint8_t btype = cfg[i + 1];
        if (blen < 2 || i + blen > len) {
            break;
        }
        if (btype == 0x04 && blen >= 9) {
            cur_alt = cfg[i + 3];
            uint8_t cls = cfg[i + 5];
            uint8_t sub = cfg[i + 6];
            in_vs = (cls == UVC_CLASS && sub == UVC_SC_VIDEOSTREAMING);
            if (cls == UVC_CLASS && sub == UVC_SC_VIDEOCONTROL &&
                vc_iface) {
                *vc_iface = cfg[i + 2];
            }
            if (in_vs && vs_iface) {
                *vs_iface = cfg[i + 2];
            }
            have_pending = 0;
        } else if (btype == 0x05 && blen >= 7 && in_vs) {
            /* エンドポイント記述子 (VS の代替設定内) */
            uint8_t addr = cfg[i + 2];
            uint16_t mp = rd16(&cfg[i + 4]) & 0x07FF;
            if ((addr & 0x80) && stream_ep && stream_max_packet &&
                mp >= *stream_max_packet) {
                *stream_ep = addr;
                *stream_max_packet = mp;
                if (vs_alt_with_ep) {
                    *vs_alt_with_ep = (uint8_t)cur_alt;
                }
                if (stream_interval) {
                    *stream_interval = cfg[i + 6]; /* bInterval */
                }
            }
        } else if (btype == 0x24 && in_vs && blen >= 4) {
            uint8_t subtype = cfg[i + 2];
            if (subtype == 0x04 && blen >= 27) {
                /* VS_FORMAT_UNCOMPRESSED */
                pending_format_index = cfg[i + 3];
                pending_mjpeg = 0;
                memcpy(pending_guid, &cfg[i + 5], 16);
                have_pending = 1;
            } else if (subtype == 0x06 && blen >= 10) {
                /* VS_FORMAT_MJPEG */
                pending_format_index = cfg[i + 3];
                pending_mjpeg = 1;
                memset(pending_guid, 0, 16);
                have_pending = 1;
            } else if ((subtype == 0x05 || subtype == 0x07) &&
                       blen >= 25 && have_pending && count < max_out) {
                /* VS_FRAME_* */
                uvc_format_frame *f = &out[count++];
                memset(f, 0, sizeof(*f));
                f->format_index = pending_format_index;
                memcpy(f->guid_format, pending_guid, 16);
                f->is_mjpeg = pending_mjpeg;
                f->frame_index = cfg[i + 3];
                f->width = rd16(&cfg[i + 5]);
                f->height = rd16(&cfg[i + 7]);
                f->max_frame_size = rd32(&cfg[i + 17]);
                f->default_interval = rd32(&cfg[i + 21]);
            }
        }
        i += blen;
    }
    return count;
}

void uvc_build_probe(uint8_t out[26], uint8_t format_index,
                     uint8_t frame_index, uint32_t interval_100ns) {
    memset(out, 0, 26);
    out[2] = format_index;  /* bmFormatIndex */
    out[3] = frame_index;   /* bmFrameIndex */
    out[4] = (uint8_t)(interval_100ns & 0xFF);
    out[5] = (uint8_t)((interval_100ns >> 8) & 0xFF);
    out[6] = (uint8_t)((interval_100ns >> 16) & 0xFF);
    out[7] = (uint8_t)((interval_100ns >> 24) & 0xFF);
}

void uvc_parse_probe(const uint8_t in[26], uint32_t *max_frame_size,
                     uint32_t *interval_100ns, uint16_t *payload_size) {
    if (interval_100ns) {
        *interval_100ns = rd32(&in[4]);
    }
    if (max_frame_size) {
        *max_frame_size = rd32(&in[18]);
    }
    if (payload_size) {
        *payload_size = rd16(&in[22]);
    }
}

int uvc_parse_payload_header(const uint8_t *buf, size_t len,
                             uvc_payload_header *hdr) {
    if (len < 2) {
        return -1;
    }
    uint8_t hlen = buf[0];
    if (hlen < 2 || hlen > len) {
        return -1;
    }
    hdr->header_len = hlen;
    hdr->fid = (buf[1] & 0x01) ? 1 : 0;
    hdr->eof = (buf[1] & 0x02) ? 1 : 0;
    hdr->err = (buf[1] & 0x40) ? 1 : 0;
    return 0;
}

void uvc_asm_init(uvc_assembler *a, uint8_t *frame_buf,
                  uint32_t capacity) {
    a->frame = frame_buf;
    a->capacity = capacity;
    a->used = 0;
    a->cur_fid = -1;
    a->complete = 0;
    a->frames_done = 0;
    a->frames_dropped = 0;
}

int uvc_asm_feed(uvc_assembler *a, const uint8_t *chunk, size_t len) {
    uvc_payload_header hdr;
    if (uvc_parse_payload_header(chunk, len, &hdr) != 0) {
        return 0;
    }
    if (hdr.err) {
        a->frames_dropped++;
        a->used = 0;
        a->cur_fid = -1;
        return 0;
    }
    if (a->cur_fid != hdr.fid) {
        /* 新しいフレームの開始。途中だったら前のフレーム
         * は破棄 (仕様: FID の反転 = 新フレーム)。 */
        if (a->cur_fid >= 0 && a->used > 0) {
            a->frames_dropped++;
        }
        a->used = 0;
        a->cur_fid = hdr.fid;
        a->complete = 0;
    }
    size_t payload = len - hdr.header_len;
    if (a->used + payload > a->capacity) {
        a->frames_dropped++;
        a->used = 0;
        a->cur_fid = -1;
        return 0;
    }
    memcpy(a->frame + a->used, chunk + hdr.header_len, payload);
    a->used += (uint32_t)payload;
    if (hdr.eof) {
        a->complete = 1;
        a->frames_done++;
        a->cur_fid = -1;
        return 1;
    }
    return 0;
}
