/* PrismOS — HD Audio 録音の純粋な採取ロジック (hda_cap.h 参照)。
 * ハードウェアに触れないため、ホストテストでも同じコードが
 * そのまま動く。 */
#include "hda_cap.h"

#include <stddef.h>

uint32_t hda_cap_target_bytes(uint32_t millis) {
    if (millis == 0 || millis > 3000) {
        millis = 2000;
    }
    uint32_t target = 48000u * 4u * millis / 1000u;
    if (target > HDA_CAP_BYTES) {
        target = HDA_CAP_BYTES;
    }
    target &= ~3u;
    return target;
}

uint32_t hda_cap_got_bytes(uint32_t max_pos, uint32_t target) {
    uint32_t got = max_pos < target ? max_pos : target;
    got &= ~3u;
    return got;
}

uint32_t hda_cap_peak(const int16_t *pcm, uint32_t samples) {
    uint32_t peak = 0;
    if (pcm == NULL) {
        return 0;
    }
    for (uint32_t i = 0; i < samples; i++) {
        int v = pcm[i];
        if (v < 0) {
            v = -v;
        }
        if ((uint32_t)v > peak) {
            peak = (uint32_t)v;
        }
    }
    return peak;
}

void hda_cap_build_bdl(uint32_t *bdl, uint64_t pcm_phys) {
    for (int i = 0; i < 256; i++) {
        bdl[i] = 0;
    }
    for (int e = 0; e < HDA_CAP_ENTRIES; e++) {
        uint64_t p = pcm_phys + (uint64_t)e * HDA_CAP_ENTRY_BYTES;
        bdl[e * 4 + 0] = (uint32_t)(p & 0xFFFFFFFFu);
        bdl[e * 4 + 1] = (uint32_t)(p >> 32);
        bdl[e * 4 + 2] = HDA_CAP_ENTRY_BYTES;
        bdl[e * 4 + 3] = 0; /* IOC なし (LPIB をポーリング) */
    }
}

int hda_cap_load_ok(const int16_t *src, uint32_t bytes) {
    if (src == NULL || bytes > HDA_CAP_BYTES) {
        return -1;
    }
    return 0;
}
