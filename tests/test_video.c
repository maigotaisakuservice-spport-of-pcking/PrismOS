/* PrismOS — 動画プレイヤーのホストテスト。 */
#include <stdio.h>
#include <string.h>

#include "video.h"
#include "deflate.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

/* raw を DEFLATE 圧縮して PV1 ファイル 1 個を作る。
 * 戻り値はファイルのバイト数。 */
static size_t make_pv1(uint8_t *out, size_t cap, uint16_t w, uint16_t h,
                       uint8_t type, const uint8_t *raw, size_t raw_len) {
    uint8_t comp[4096];
    int comp_len = deflate_compress_fixed(raw, raw_len, comp,
                                          sizeof(comp));
    if (comp_len < 0 || PV1_HEADER_LEN + (size_t)comp_len > cap) {
        return 0;
    }
    out[0] = 'P';
    out[1] = 'R';
    out[2] = 'V';
    out[3] = '1';
    out[4] = (uint8_t)(w & 0xFF);
    out[5] = (uint8_t)(w >> 8);
    out[6] = (uint8_t)(h & 0xFF);
    out[7] = (uint8_t)(h >> 8);
    out[8] = type;
    out[9] = 0;
    out[10] = (uint8_t)(raw_len & 0xFF);
    out[11] = (uint8_t)((raw_len >> 8) & 0xFF);
    out[12] = (uint8_t)((raw_len >> 16) & 0xFF);
    out[13] = (uint8_t)((raw_len >> 24) & 0xFF);
    out[14] = (uint8_t)(comp_len & 0xFF);
    out[15] = (uint8_t)((comp_len >> 8) & 0xFF);
    out[16] = (uint8_t)((comp_len >> 16) & 0xFF);
    out[17] = (uint8_t)((comp_len >> 24) & 0xFF);
    memcpy(out + PV1_HEADER_LEN, comp, (size_t)comp_len);
    return PV1_HEADER_LEN + (size_t)comp_len;
}

int main(void) {
    /* フレームの収集: "vid-*.bmp" だけ、名前順に並ぶ。 */
    {
        const char *names[] = {
            "README.txt", "vid-03.bmp", "melody.wav", "vid-01.bmp",
            "vid-02.bmp", "prism.bmp", "vid-10.bmp", "vid-2.bmp",
            "vid-01.txt",
        };
        int out[VIDEO_MAX_FRAMES];
        int n = video_collect(names, 9, out, VIDEO_MAX_FRAMES);
        check(n == 5, "collects 5 frames");
        check(out[0] == 3, "first frame is vid-01.bmp");
        check(out[1] == 4, "second frame is vid-02.bmp");
        check(out[2] == 1, "third frame is vid-03.bmp");
        check(out[3] == 6, "fourth frame is vid-10.bmp");
        check(out[4] == 7, "fifth frame is vid-2.bmp (name order)");
        check(video_collect(names, 0, out, VIDEO_MAX_FRAMES) == 0,
              "empty list collects none");
        check(video_collect(NULL, 0, out, VIDEO_MAX_FRAMES) == 0,
              "NULL list collects none");
        int small[2];
        check(video_collect(names, 9, small, 2) == 2,
              "max limit is respected");
    }

    /* 再生の状態遷移。 */
    {
        video_player p;
        video_init(&p, 12);
        check(p.frame == 0 && p.playing == 0, "player starts at frame 0");
        video_advance(&p);
        check(p.frame == 1, "advance moves one frame");
        video_seek(&p, 5);
        check(p.frame == 6, "seek forward 5");
        video_seek(&p, -3);
        check(p.frame == 3, "seek back 3");
        video_seek(&p, -100);
        check(p.frame == 0, "seek clamps at the start");
        video_seek(&p, 100);
        check(p.frame == 11, "seek clamps at the end");
        video_advance(&p);
        check(p.frame == 0, "advance wraps to the start");
        video_init(&p, 0);
        video_advance(&p);
        check(p.frame == 0, "advance with no frames stays at 0");
        video_seek(&p, 5);
        check(p.frame == 0, "seek with no frames stays at 0");
    }

    /* 時刻で進む再生。 */
    {
        video_player p;
        video_init(&p, 4);
        p.playing = 1;
        int64_t tpf = video_ticks_per_frame(1000);
        check(tpf == 250, "ticks per frame at 1000 ticks/sec");
        check(video_ticks_per_frame(0) == 1, "no rate means 1 tick/frame");
        check(video_ticks_per_frame(2) == 1, "slow rate clamps to 1");
        int64_t next = 1000;
        check(video_tick(&p, 999, &next, tpf) == 0 && p.frame == 0,
              "frame does not advance early");
        check(video_tick(&p, 1000, &next, tpf) == 1 && p.frame == 1,
              "frame advances at the tick");
        check(next == 1250, "next tick is one frame ahead");
        check(video_tick(&p, 1249, &next, tpf) == 0 && p.frame == 1,
              "frame holds until the next tick");
        check(video_tick(&p, 1250, &next, tpf) == 1 && p.frame == 2,
              "second frame advances on time");
        p.playing = 0;
        check(video_tick(&p, 99999, &next, tpf) == 0 && p.frame == 2,
              "paused player does not advance");
    }

    /* PV1 の収集: "vid-*.pv1" だけ、名前順に並ぶ。 */
    {
        const char *names[] = {
            "README.txt", "vid-03.pv1", "vid-01.bmp", "vid-01.pv1",
            "vid-02.pv1", "vid-10.pv1", "vid-2.pv1",
        };
        int out[VIDEO_MAX_FRAMES];
        int n = video_collect_pv1(names, 7, out, VIDEO_MAX_FRAMES);
        check(n == 5, "collects 5 pv1 frames");
        check(out[0] == 3, "first pv1 frame is vid-01.pv1");
        check(out[1] == 4, "second pv1 frame is vid-02.pv1");
        check(out[2] == 1, "third pv1 frame is vid-03.pv1");
        check(out[3] == 5, "fourth pv1 frame is vid-10.pv1");
        check(out[4] == 6, "fifth pv1 frame is vid-2.pv1");
    }

    /* PV1 ヘッダの読み取りとキーフレームの位置。 */
    {
        uint8_t raw[4 * 2 * 2];
        uint16_t px[8] = { 0x1111, 0x2222, 0x3333, 0x4444,
                           0x5555, 0x6666, 0x7777, 0x7FFF };
        for (int i = 0; i < 8; i++) {
            raw[i * 2] = (uint8_t)(px[i] & 0xFF);
            raw[i * 2 + 1] = (uint8_t)(px[i] >> 8);
        }
        uint8_t file[4096];
        size_t flen = make_pv1(file, sizeof(file), 4, 2, PV1_TYPE_KEY,
                               raw, sizeof(raw));
        check(flen > PV1_HEADER_LEN, "pv1 key file is built");
        pv1_header hdr;
        check(pv1_parse_header(file, flen, &hdr) == 0,
              "pv1 header parses");
        check(hdr.width == 4 && hdr.height == 2,
              "pv1 header size is 4x2");
        check(hdr.type == PV1_TYPE_KEY, "pv1 header type is key");
        check(hdr.raw_len == sizeof(raw), "pv1 header raw_len matches");
        check(hdr.comp_len == flen - PV1_HEADER_LEN,
              "pv1 header comp_len matches");
        check(pv1_keyframe_for(0) == 0, "keyframe for 0 is 0");
        check(pv1_keyframe_for(7) == 0, "keyframe for 7 is 0");
        check(pv1_keyframe_for(8) == 8, "keyframe for 8 is 8");
        check(pv1_keyframe_for(15) == 8, "keyframe for 15 is 8");
        check(pv1_keyframe_for(16) == 16, "keyframe for 16 is 16");

        /* 壊れたヘッダは弾く。 */
        uint8_t bad[4096];
        memcpy(bad, file, flen);
        bad[0] = 'X';
        check(pv1_parse_header(bad, flen, &hdr) != 0,
              "bad magic is rejected");
        memcpy(bad, file, flen);
        bad[8] = 3;
        check(pv1_parse_header(bad, flen, &hdr) != 0,
              "bad type is rejected");
        check(pv1_parse_header(file, 10, &hdr) != 0,
              "short file is rejected");
        check(pv1_parse_header(NULL, flen, &hdr) != 0,
              "NULL data is rejected");
        memcpy(bad, file, flen);
        bad[4] = 0;
        bad[5] = 0;
        check(pv1_parse_header(bad, flen, &hdr) != 0,
              "zero width is rejected");
        memcpy(bad, file, flen);
        bad[4] = 65;
        bad[5] = 1;
        check(pv1_parse_header(bad, flen, &hdr) != 0,
              "too wide frame is rejected");

        /* キーフレームの往復: 圧縮して展開すると元の画素に
         * 戻る。 */
        uint16_t got[8];
        uint16_t ow = 0, oh = 0;
        check(pv1_decode_key(file, flen, got, 8, &ow, &oh) == 0,
              "pv1 key decodes");
        check(ow == 4 && oh == 2, "pv1 key size is reported");
        int same = 1;
        for (int i = 0; i < 8; i++) {
            if (got[i] != px[i]) {
                same = 0;
            }
        }
        check(same, "pv1 key pixels round-trip");
        uint16_t small[4];
        check(pv1_decode_key(file, flen, small, 4, NULL, NULL) != 0,
              "small output buffer is rejected");
        check(pv1_decode_key(file, flen, got, 8, NULL, NULL) == 0,
              "NULL size outputs are fine");
    }

    /* PV1 差分フレーム: 変わった画素だけ上書きする。 */
    {
        uint16_t base[8] = { 0x1111, 0x2222, 0x3333, 0x4444,
                             0x5555, 0x6666, 0x7777, 0x7FFF };
        /* 画素 2 と 5 を変える操作列 (7 組 = 14 バイト)。 */
        uint8_t ops[14];
        size_t o = 0;
        ops[o++] = 2; ops[o++] = 0; /* skip 2 */
        ops[o++] = 1; ops[o++] = 0; /* count 1 */
        ops[o++] = 0xAA; ops[o++] = 0xAA; /* 新しい画素 2 */
        ops[o++] = 2; ops[o++] = 0; /* skip 2 (画素 3, 4) */
        ops[o++] = 1; ops[o++] = 0; /* count 1 */
        ops[o++] = 0xBB; ops[o++] = 0xBB; /* 新しい画素 5 */
        ops[o++] = 0xFF; ops[o++] = 0xFF; /* 終わり */
        uint8_t file[4096];
        size_t flen = make_pv1(file, sizeof(file), 4, 2, PV1_TYPE_DIFF,
                               ops, o);
        check(flen > PV1_HEADER_LEN, "pv1 diff file is built");
        pv1_header hdr;
        check(pv1_parse_header(file, flen, &hdr) == 0,
              "pv1 diff header parses");
        check(hdr.type == PV1_TYPE_DIFF, "pv1 diff header type is diff");
        uint16_t cur[8];
        memcpy(cur, base, sizeof(cur));
        uint8_t scratch[64];
        check(pv1_decode_diff(file, flen, cur, 8, scratch,
                              sizeof(scratch), NULL, NULL) == 0,
              "pv1 diff decodes");
        check(cur[0] == 0x1111 && cur[1] == 0x2222,
              "pv1 diff keeps untouched pixels");
        check(cur[2] == 0xAAAA, "pv1 diff overwrites pixel 2");
        check(cur[3] == 0x4444 && cur[4] == 0x5555,
              "pv1 diff skips unchanged pixels");
        check(cur[5] == 0xBBBB, "pv1 diff overwrites pixel 5");
        check(cur[6] == 0x7777 && cur[7] == 0x7FFF,
              "pv1 diff keeps the tail");
        /* 作業領域が足りなければ失敗する。 */
        memcpy(cur, base, sizeof(cur));
        uint8_t tiny[2];
        check(pv1_decode_diff(file, flen, cur, 8, tiny, sizeof(tiny),
                              NULL, NULL) != 0,
              "small scratch is rejected");
        /* キーフレームを差分として読むと失敗する。 */
        uint8_t kraw[4 * 2 * 2];
        memset(kraw, 0, sizeof(kraw));
        uint8_t kfile[4096];
        size_t kflen = make_pv1(kfile, sizeof(kfile), 4, 2,
                                PV1_TYPE_KEY, kraw, sizeof(kraw));
        check(pv1_decode_diff(kfile, kflen, cur, 8, scratch,
                              sizeof(scratch), NULL, NULL) != 0,
              "key file is not a diff");
        check(pv1_decode_key(file, flen, cur, 8, NULL, NULL) != 0,
              "diff file is not a key");
    }

    /* PV1 の音声トラック: 映像データのあとに生 PCM が付く。
     * 音声のない古いファイルもそのまま読める。 */
    {
        check(PV1_AUDIO_RATE == 8000, "pv1 audio rate is 8000 Hz");
        check(PV1_AUDIO_SAMPLES_PER_FRAME == 2000,
              "pv1 audio is 2000 samples per frame");
        check(PV1_AUDIO_SAMPLES_PER_FRAME <= PV1_AUDIO_MAX_SAMPLES,
              "pv1 audio fits the playback buffer");
        uint8_t raw[4 * 2 * 2];
        for (int i = 0; i < 16; i++) {
            raw[i] = (uint8_t)i;
        }
        uint8_t file[8192];
        size_t flen = make_pv1(file, sizeof(file), 4, 2, PV1_TYPE_KEY,
                               raw, sizeof(raw));
        check(flen > PV1_HEADER_LEN, "pv1 audio test file is built");
        check(pv1_audio_len(file, flen) == 0,
              "pv1 without audio has no audio");
        check(pv1_audio_data(file, flen) == NULL,
              "pv1 without audio has no audio pointer");
        /* 音声を 5 サンプル付ける。 */
        uint8_t tone[5] = { 128, 200, 128, 56, 128 };
        memcpy(file + flen, tone, sizeof(tone));
        size_t alen = flen + sizeof(tone);
        pv1_header hdr;
        check(pv1_parse_header(file, alen, &hdr) == 0,
              "pv1 with audio still parses");
        check(pv1_audio_len(file, alen) == 5,
              "pv1 audio length is read");
        const uint8_t *ap = pv1_audio_data(file, alen);
        check(ap != NULL && ap[0] == 128 && ap[1] == 200 &&
                  ap[2] == 128 && ap[3] == 56 && ap[4] == 128,
              "pv1 audio samples round-trip");
        /* 音声が付いても映像の復号は変わらない。 */
        uint16_t got[8];
        check(pv1_decode_key(file, alen, got, 8, NULL, NULL) == 0,
              "pv1 key decodes with audio attached");
        /* 壊れたヘッダでは音声も読めない。 */
        uint8_t bad[8192];
        memcpy(bad, file, alen);
        bad[0] = 'X';
        check(pv1_audio_len(bad, alen) != 0 &&
                  pv1_audio_len(bad, alen) < 0,
              "pv1 audio rejects a bad header");
        check(pv1_audio_data(bad, alen) == NULL,
              "pv1 audio pointer rejects a bad header");
        /* 4096 サンプルを超える音声は付けられない。 */
        uint8_t big[8192];
        size_t blen = make_pv1(big, sizeof(big), 4, 2, PV1_TYPE_KEY,
                               raw, sizeof(raw));
        memset(big + blen, 128, 4097);
        check(pv1_audio_len(big, blen + 4097) < 0,
              "pv1 audio over 4096 samples is rejected");
        check(pv1_audio_data(big, blen + 4097) == NULL,
              "pv1 audio pointer rejects over-long audio");
        /* ちょうど 4096 サンプルは読める。 */
        memset(big + blen, 128, 4096);
        check(pv1_audio_len(big, blen + 4096) == 4096,
              "pv1 audio of exactly 4096 samples is read");
    }

    /* PV1 の 320x240: キーフレームの往復。 */
    {
        static uint16_t px320[PV1_MAX_PIXELS];
        static uint8_t raw320[PV1_MAX_PIXELS * 2];
        static uint8_t file320[8192];
        static uint16_t got320[PV1_MAX_PIXELS];
        for (size_t i = 0; i < PV1_MAX_PIXELS; i++) {
            px320[i] = (i % 7 == 0) ? (uint16_t)0xF800
                                    : (uint16_t)0x07E0;
            raw320[i * 2] = (uint8_t)(px320[i] & 0xFF);
            raw320[i * 2 + 1] = (uint8_t)(px320[i] >> 8);
        }
        size_t flen = make_pv1(file320, sizeof(file320), 320, 240,
                               PV1_TYPE_KEY, raw320, sizeof(raw320));
        check(flen > PV1_HEADER_LEN, "pv1 320x240 key file is built");
        check(flen <= 16384, "pv1 320x240 key fits one VFS file");
        pv1_header hdr;
        check(pv1_parse_header(file320, flen, &hdr) == 0,
              "pv1 320x240 header parses");
        check(hdr.width == 320 && hdr.height == 240,
              "pv1 320x240 header size is 320x240");
        uint16_t ow = 0, oh = 0;
        check(pv1_decode_key(file320, flen, got320, PV1_MAX_PIXELS,
                             &ow, &oh) == 0,
              "pv1 320x240 key decodes");
        check(ow == 320 && oh == 240, "pv1 320x240 size is reported");
        int same = 1;
        for (size_t i = 0; i < PV1_MAX_PIXELS; i++) {
            if (got320[i] != px320[i]) {
                same = 0;
                break;
            }
        }
        check(same, "pv1 320x240 pixels round-trip");
        /* 160x120 用の小さな出力では 320x240 は読めない。 */
        uint16_t small[160 * 120];
        check(pv1_decode_key(file320, flen, small, 160 * 120,
                             NULL, NULL) != 0,
              "pv1 320x240 rejects a 160x120 buffer");
    }

    printf("video tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
