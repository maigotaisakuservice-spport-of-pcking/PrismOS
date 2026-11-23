/* PrismOS — PC スピーカー (PIT チャンネル 2 + ポート 0x61) */
#include "speaker.h"

#include "io.h"

#define PIT_CH0 0x40
#define PIT_CH2 0x42
#define PIT_CMD 0x43
#define SPEAKER_PORT 0x61
#define PIT_HZ 1193182u

uint16_t speaker_pit0_count(void) {
    /* ラッチコマンド (チャンネル 0、カウンタを止めずに読む)。 */
    prism_outb(PIT_CMD, 0x00);
    uint8_t lo = prism_inb(PIT_CH0);
    uint8_t hi = prism_inb(PIT_CH0);
    return (uint16_t)((uint16_t)hi << 8 | lo);
}

void speaker_wait_ticks(uint32_t ticks) {
    /* カウンタは 16 ビットで 65536 ティックごとに折り返すため、
     * 差分だけでは 65536 ティック以上の待ちを測れない。
     * 60000 ティックずつに分けて待つ。 */
    while (ticks > 0) {
        uint32_t chunk = ticks > 60000 ? 60000 : ticks;
        uint16_t start = speaker_pit0_count();
        uint32_t waited = 0;
        while (waited < chunk) {
            uint16_t now = speaker_pit0_count();
            /* カウンタは減少する。16 ビットの折り返しも
             * 符号なし演算でそのまま扱える。 */
            waited = (uint32_t)(uint16_t)(start - now);
        }
        ticks -= chunk;
    }
}

void speaker_off(void) {
    uint8_t v = prism_inb(SPEAKER_PORT);
    prism_outb(SPEAKER_PORT, (uint8_t)(v & ~0x03));
}

void speaker_tone(uint32_t freq_hz, uint32_t ms) {
    if (freq_hz == 0 || ms == 0) {
        return;
    }
    uint32_t divisor = PIT_HZ / freq_hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 65535) {
        divisor = 65535;
    }
    /* チャンネル 2、モード 3 (矩形波)、下位→上位の順で書く。 */
    prism_outb(PIT_CMD, 0xB6);
    prism_outb(PIT_CH2, (uint8_t)(divisor & 0xFF));
    prism_outb(PIT_CH2, (uint8_t)((divisor >> 8) & 0xFF));
    /* ビット 0 = タイマ 2 のゲート、ビット 1 = スピーカー有効。 */
    uint8_t v = prism_inb(SPEAKER_PORT);
    prism_outb(SPEAKER_PORT, (uint8_t)(v | 0x03));
    speaker_wait_ticks(ms * PIT_HZ / 1000);
    speaker_off();
}
