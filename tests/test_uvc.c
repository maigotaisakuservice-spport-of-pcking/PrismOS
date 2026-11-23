/* ホストテスト: UVC の記述子解析・Probe/Commit・
 * フレーム組立 (kernel/uvc.c の純粋ロジック)。 */
#include "../kernel/uvc.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;
#define CHECK(cond)                                       \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: %s\n", __FILE__,          \
                   __LINE__, #cond);                      \
        }                                                 \
    } while (0)

/* 合成した構成記述子: IAD + VC(if0) + VS(if1, alt0 EP無し,
 * alt1 アイソクロナス IN EP 0x81 maxpacket 1024)。
 * VS クラス固有: MJPEG 形式 1 + フレーム 640x480、
 * 未圧縮形式 2 + フレーム 320x240。 */
static size_t build_cfg(uint8_t *b) {
    size_t n = 0;
    /* 構成ヘッダ (9) */
    b[n++] = 9; b[n++] = 0x02; b[n++] = 0; b[n++] = 0;
    b[n++] = 2; b[n++] = 1; b[n++] = 0; b[n++] = 0x80; b[n++] = 50;
    /* VC インタフェース (9) */
    b[n++] = 9; b[n++] = 0x04; b[n++] = 0; b[n++] = 0;
    b[n++] = 0; b[n++] = 0x0E; b[n++] = 0x01; b[n++] = 0; b[n++] = 0;
    /* VS インタフェース alt0 (9) */
    b[n++] = 9; b[n++] = 0x04; b[n++] = 1; b[n++] = 0;
    b[n++] = 0; b[n++] = 0x0E; b[n++] = 0x02; b[n++] = 0; b[n++] = 0;
    /* VS_INPUT_HEADER (13) */
    b[n++] = 13; b[n++] = 0x24; b[n++] = 0x01; b[n++] = 1;
    b[n++] = 0; b[n++] = 0; b[n++] = 0x81; b[n++] = 0;
    b[n++] = 0; b[n++] = 0; b[n++] = 0; b[n++] = 0; b[n++] = 0;
    /* VS_FORMAT_MJPEG (10): index 1 */
    b[n++] = 10; b[n++] = 0x24; b[n++] = 0x06; b[n++] = 1;
    b[n++] = 1; b[n++] = 0; b[n++] = 0; b[n++] = 0; b[n++] = 0; b[n++] = 0;
    /* VS_FRAME_MJPEG (38): index 1, 640x480 */
    {
        uint8_t *f = &b[n];
        memset(f, 0, 38);
        f[0] = 38; f[1] = 0x24; f[2] = 0x07; f[3] = 1;
        f[5] = 640 & 0xFF; f[6] = 640 >> 8;
        f[7] = 480 & 0xFF; f[8] = 480 >> 8;
        uint32_t sz = 614400, iv = 333333;
        memcpy(&f[17], &sz, 4);
        memcpy(&f[21], &iv, 4);
        f[25] = 0; /* 連続間隔 */
        n += 38;
    }
    /* VS_FORMAT_UNCOMPRESSED (27): index 2, YUY2 GUID */
    {
        uint8_t *f = &b[n];
        memset(f, 0, 27);
        f[0] = 27; f[1] = 0x24; f[2] = 0x04; f[3] = 2; f[4] = 1;
        f[5] = 0x59; f[6] = 0x55; f[7] = 0x59; f[8] = 0x32; /* YUY2 */
        f[21] = 16; /* bBitsPerPixel */
        n += 27;
    }
    /* VS_FRAME_UNCOMPRESSED (38): index 1, 320x240 */
    {
        uint8_t *f = &b[n];
        memset(f, 0, 38);
        f[0] = 38; f[1] = 0x24; f[2] = 0x05; f[3] = 1;
        f[5] = 320 & 0xFF; f[6] = 320 >> 8;
        f[7] = 240 & 0xFF; f[8] = 240 >> 8;
        uint32_t sz = 153600, iv = 666666;
        memcpy(&f[17], &sz, 4);
        memcpy(&f[21], &iv, 4);
        n += 38;
    }
    /* VS インタフェース alt1 (9) */
    b[n++] = 9; b[n++] = 0x04; b[n++] = 1; b[n++] = 1;
    b[n++] = 1; b[n++] = 0x0E; b[n++] = 0x02; b[n++] = 0; b[n++] = 0;
    /* エンドポイント (7): 0x81 アイソクロナス 1024 */
    b[n++] = 7; b[n++] = 0x05; b[n++] = 0x81; b[n++] = 0x01;
    b[n++] = 0x00; b[n++] = 0x04; b[n++] = 1;
    /* wTotalLength を埋める */
    b[2] = (uint8_t)(n & 0xFF);
    b[3] = (uint8_t)(n >> 8);
    return n;
}

int main(void) {
    uint8_t cfg[512];
    size_t len = build_cfg(cfg);
    uvc_format_frame out[8];
    int vc = -9, vs = -9;
    uint8_t ep = 0, alt = 0, bint = 0;
    uint16_t mp = 0;
    int cnt = uvc_parse_config(cfg, len, out, 8, &vc, &vs, &ep, &mp,
                               &alt, &bint);
    CHECK(cnt == 2);
    CHECK(vc == 0);
    CHECK(vs == 1);
    CHECK(ep == 0x81);
    CHECK(mp == 1024);
    CHECK(alt == 1);
    CHECK(bint == 1);
    CHECK(out[0].format_index == 1);
    CHECK(out[0].is_mjpeg == 1);
    CHECK(out[0].width == 640 && out[0].height == 480);
    CHECK(out[0].max_frame_size == 614400);
    CHECK(out[0].default_interval == 333333);
    CHECK(out[1].format_index == 2);
    CHECK(out[1].is_mjpeg == 0);
    CHECK(out[1].width == 320 && out[1].height == 240);
    CHECK(out[1].guid_format[0] == 0x59 && out[1].guid_format[3] == 0x32);
    /* 壊れた記述子で止まる */
    cfg[9 + 9] = 0; /* VC の次を長さ 0 に */
    cnt = uvc_parse_config(cfg, len, out, 8, &vc, &vs, &ep, &mp,
                           &alt, &bint);
    CHECK(cnt == 0);

    /* Probe/Commit */
    uint8_t probe[26];
    uvc_build_probe(probe, 1, 1, 333333);
    CHECK(probe[2] == 1 && probe[3] == 1);
    uint32_t iv = 0;
    memcpy(&iv, &probe[4], 4);
    CHECK(iv == 333333);
    uint8_t resp[26];
    memset(resp, 0, sizeof(resp));
    uint32_t mfs = 500000, piv = 333333;
    memcpy(&resp[18], &mfs, 4);
    memcpy(&resp[4], &piv, 4);
    resp[22] = 0x00; resp[23] = 0x04; /* 1024 */
    uint32_t g_mfs = 0, g_iv = 0;
    uint16_t g_ps = 0;
    uvc_parse_probe(resp, &g_mfs, &g_iv, &g_ps);
    CHECK(g_mfs == 500000 && g_iv == 333333 && g_ps == 1024);

    /* ペイロードヘッダとフレーム組立 */
    static uint8_t framebuf[64];
    uvc_assembler a;
    uvc_asm_init(&a, framebuf, sizeof(framebuf));
    uint8_t c1[6] = {2, 0x00, 'A', 'B', 'C', 'D'}; /* FID0 */
    uint8_t c2[6] = {2, 0x02, 'E', 'F', 'G', 'H'}; /* FID0+EOF */
    CHECK(uvc_asm_feed(&a, c1, sizeof(c1)) == 0);
    CHECK(uvc_asm_feed(&a, c2, sizeof(c2)) == 1);
    CHECK(a.used == 8);
    CHECK(memcmp(framebuf, "ABCDEFGH", 8) == 0);
    CHECK(a.frames_done == 1);
    /* FID 反転で途中フレームを破棄 */
    uint8_t c3[4] = {2, 0x01, 'X', 'Y'}; /* FID1 */
    uint8_t c4[4] = {2, 0x00, 'Z', 'W'}; /* FID0: c3 を破棄 */
    CHECK(uvc_asm_feed(&a, c3, sizeof(c3)) == 0);
    CHECK(uvc_asm_feed(&a, c4, sizeof(c4)) == 0);
    CHECK(a.frames_dropped == 1);
    CHECK(a.used == 2 && framebuf[0] == 'Z');
    /* ERR ビットで破棄 */
    uint8_t c5[4] = {2, 0x40, 'Q', 'Q'};
    CHECK(uvc_asm_feed(&a, c5, sizeof(c5)) == 0);
    CHECK(a.frames_dropped == 2);
    /* 壊れたヘッダ */
    uvc_payload_header h;
    uint8_t bad[1] = {2};
    CHECK(uvc_parse_payload_header(bad, 1, &h) != 0);

    printf("test_uvc: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
