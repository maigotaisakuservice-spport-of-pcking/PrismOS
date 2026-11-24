/* PrismOS — HD Audio 録音の採取ロジックのホストテスト。
 *
 * カーネルと同じ kernel/hda_cap.c をホストで動かす:
 * BDL 構築・目標バイト数・DMA 位置からの取得量・ピーク走査・
 * 読み込み可否。QEMU では音声バックエンドが採取 DMA を
 * 進めないため、DMA 完了そのものは現環境では観測できない
 * が、ここにある計算は hda_record が使う実コードと同じもの
 * である。 */
#include <stdint.h>
#include <stdio.h>

#include "hda_cap.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

int main(void) {
    /* 定数: 36 x 16KB = 576KB。3 秒分 (576000) が収まる。 */
    CHECK(HDA_CAP_ENTRIES == 36);
    CHECK(HDA_CAP_ENTRY_BYTES == 16384);
    CHECK(HDA_CAP_BYTES == 36u * 16384u);
    CHECK(HDA_CAP_BYTES == 589824u);

    /* 目標バイト数: 48000Hz x 4 バイト/サンプル。 */
    CHECK(hda_cap_target_bytes(1000) == 192000u);
    CHECK(hda_cap_target_bytes(2000) == 384000u);
    CHECK(hda_cap_target_bytes(3000) == 576000u);
    CHECK(hda_cap_target_bytes(500) == 96000u);
    /* 0 と 3000 超は 2000 ミリ秒扱い。 */
    CHECK(hda_cap_target_bytes(0) == 384000u);
    CHECK(hda_cap_target_bytes(3001) == 384000u);
    CHECK(hda_cap_target_bytes(60000) == 384000u);
    /* 4 バイト境界: 1 ミリ秒は 192 バイト、7 ミリ秒は 1344。 */
    CHECK(hda_cap_target_bytes(1) == 192u);
    CHECK(hda_cap_target_bytes(7) == 1344u);
    /* 3000 ミリ秒でも上限 (576KB) を超えない。 */
    CHECK(hda_cap_target_bytes(3000) < HDA_CAP_BYTES);

    /* 取得量: LPIB の進みと目標の小さい方、4 バイトに切下げ。 */
    CHECK(hda_cap_got_bytes(0, 384000) == 0);
    CHECK(hda_cap_got_bytes(100, 384000) == 100);
    CHECK(hda_cap_got_bytes(101, 384000) == 100);
    CHECK(hda_cap_got_bytes(3, 384000) == 0);
    CHECK(hda_cap_got_bytes(384000, 384000) == 384000);
    CHECK(hda_cap_got_bytes(500000, 384000) == 384000);
    CHECK(hda_cap_got_bytes(192000, 192000) == 192000);
    CHECK(hda_cap_got_bytes(0xFFFFFFFFu, 192000) == 192000);

    /* ピーク走査。 */
    {
        int16_t pcm[] = {0, 100, -100, 32767, -32768, 5, -5};
        CHECK(hda_cap_peak(pcm, 7) == 32768u);
        CHECK(hda_cap_peak(pcm, 4) == 32767u);
        CHECK(hda_cap_peak(pcm, 2) == 100u);
        int16_t zeros[8] = {0};
        CHECK(hda_cap_peak(zeros, 8) == 0);
        CHECK(hda_cap_peak(pcm, 0) == 0);
        CHECK(hda_cap_peak(NULL, 10) == 0);
        int16_t one[] = {-1};
        CHECK(hda_cap_peak(one, 1) == 1u);
        int16_t neg[] = {-30000, -1, -2};
        CHECK(hda_cap_peak(neg, 3) == 30000u);
    }

    /* BDL 構築: 36 エントリが pcm 先頭からの連続領域を指す。 */
    {
        static uint32_t bdl[256];
        for (int i = 0; i < 256; i++) {
            bdl[i] = 0xDEADBEEFu;
        }
        hda_cap_build_bdl(bdl, 0x10000000ULL);
        CHECK(bdl[0] == 0x10000000u);
        CHECK(bdl[1] == 0);
        CHECK(bdl[2] == 16384u);
        CHECK(bdl[3] == 0);
        CHECK(bdl[4] == 0x10004000u);
        CHECK(bdl[5] == 0);
        CHECK(bdl[6] == 16384u);
        CHECK(bdl[7] == 0);
        /* 最終エントリ (35): 0x10000000 + 35*16384。 */
        CHECK(bdl[35 * 4 + 0] == 0x10000000u + 35u * 16384u);
        CHECK(bdl[35 * 4 + 1] == 0);
        CHECK(bdl[35 * 4 + 2] == 16384u);
        CHECK(bdl[35 * 4 + 3] == 0);
        /* 36 エントリより後ろは 0 (残りの 256-144 ワード)。 */
        CHECK(bdl[144] == 0);
        CHECK(bdl[200] == 0);
        CHECK(bdl[255] == 0);
        /* 64 ビット物理アドレス: 上位が入る。 */
        hda_cap_build_bdl(bdl, 0x12F0000000ULL);
        CHECK(bdl[0] == 0xF0000000u);
        CHECK(bdl[1] == 0x12u);
        CHECK(bdl[4] == 0xF0004000u);
        CHECK(bdl[5] == 0x12u);
    }

    /* 読み込み可否。 */
    {
        int16_t src[4] = {1, 2, 3, 4};
        CHECK(hda_cap_load_ok(src, 0) == 0);
        CHECK(hda_cap_load_ok(src, HDA_CAP_BYTES) == 0);
        CHECK(hda_cap_load_ok(src, HDA_CAP_BYTES + 1) == -1);
        CHECK(hda_cap_load_ok(NULL, 0) == -1);
        CHECK(hda_cap_load_ok(NULL, 100) == -1);
    }

    printf("hda_cap tests: %d checks, %d failures\n", checks,
           failures);
    if (failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
