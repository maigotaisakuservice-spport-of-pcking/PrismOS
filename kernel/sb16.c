/* PrismOS — Sound Blaster 16 (ISA, ポート 0x220) */
#include "sb16.h"

#include "io.h"
#include "pmm.h"
#include "speaker.h"

#define SB_BASE 0x220
#define SB_MIXER_ADDR (SB_BASE + 4)
#define SB_MIXER_DATA (SB_BASE + 5)
#define SB_RESET (SB_BASE + 6)
#define SB_READ_DATA (SB_BASE + 10)
#define SB_WRITE (SB_BASE + 12)
#define SB_STATUS (SB_BASE + 14)
#define SB_IRQ_STATUS (SB_BASE + 15)

#define DMA_MASK 0x0A
#define DMA_CLEAR_FF 0x0C
#define DMA_MODE 0x0B
#define DMA_CH1_ADDR 0x02
#define DMA_CH1_COUNT 0x03
#define DMA_CH1_PAGE 0x83

#define SB_DMA_BUF_SIZE 4096

static uint64_t sb_hhdm;
static int sb_have_hhdm;
static int sb_checked;
static int sb_ok;
static uint64_t sb_dma_phys;
static uint8_t *sb_dma_buf;
static uint32_t sb_total; /* 非同期再生の全サンプル数 */
static int sb_active;     /* 非同期再生が進行中 */
static uint32_t sb_prev_played; /* 巻き戻し検出用 */

void sb16_set_hhdm(uint64_t hhdm_offset) {
    sb_hhdm = hhdm_offset;
    sb_have_hhdm = 1;
}

/* DSP がコマンドを受け取れるまで待つ (最大で約 10 万回読む)。 */
static int sb_wait_write_ready(void) {
    for (int i = 0; i < 100000; i++) {
        if ((prism_inb(SB_STATUS) & 0x80) == 0) {
            return 0;
        }
    }
    return -1;
}

static int sb_write_cmd(uint8_t cmd) {
    if (sb_wait_write_ready() != 0) {
        return -1;
    }
    prism_outb(SB_WRITE, cmd);
    return 0;
}

/* DSP をリセットし、0xAA が返るか確認する。 */
static int sb_reset_dsp(void) {
    prism_outb(SB_RESET, 1);
    /* 約 3 マイクロ秒待つ: ポート読み取りを数回繰り返す。 */
    for (int i = 0; i < 10; i++) {
        (void)prism_inb(SB_RESET);
    }
    prism_outb(SB_RESET, 0);
    for (int i = 0; i < 100000; i++) {
        if ((prism_inb(SB_STATUS) & 0x80) != 0) {
            if (prism_inb(SB_READ_DATA) == 0xAA) {
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

static void sb_mixer_write(uint8_t reg, uint8_t value) {
    prism_outb(SB_MIXER_ADDR, reg);
    prism_outb(SB_MIXER_DATA, value);
}

static int sb_init_once(void) {
    if (sb_checked) {
        return sb_ok ? 0 : -1;
    }
    sb_checked = 1;
    if (!sb_have_hhdm) {
        return -1;
    }
    if (sb_reset_dsp() != 0) {
        return -1;
    }
    /* 音量を最大にする (ミキサの既定が絞られている場合に備える)。 */
    sb_mixer_write(0x22, 0xFF); /* マスター音量 */
    sb_mixer_write(0x04, 0xFF); /* 音声 (Voice) 音量 */
    sb_dma_phys = pmm_alloc_pages(1);
    if (sb_dma_phys == 0) {
        return -1;
    }
    sb_dma_buf = (uint8_t *)(uintptr_t)(sb_dma_phys + sb_hhdm);
    sb_ok = 1;
    return 0;
}

int sb16_present(void) {
    return sb_init_once() == 0;
}

static int sb_volume = 100;

int sb16_get_volume(void) {
    return sb_volume;
}

void sb16_set_volume(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    sb_volume = percent;
    if (sb_ok) {
        /* ミキサのマスター音量 (0x30=左, 0x31=右、
         * 上位5ビットが 0-31 のレベル)。 */
        uint8_t level = (uint8_t)((percent * 31) / 100);
        uint8_t val = (uint8_t)(level << 3);
        prism_outb(SB_MIXER_ADDR, 0x30);
        prism_outb(SB_MIXER_DATA, val);
        prism_outb(SB_MIXER_ADDR, 0x31);
        prism_outb(SB_MIXER_DATA, val);
    }
}

int sb16_start_8bit(const uint8_t *samples, size_t count,
                    uint32_t rate) {
    if (samples == NULL || count == 0 || count > SB_DMA_BUF_SIZE) {
        return -1;
    }
    if (rate < 4000 || rate > 48000) {
        return -1;
    }
    if (sb_init_once() != 0) {
        return -1;
    }
    sb16_set_volume(sb_volume); /* ミキサへ現在の音量を反映 */
    /* 転送バッファにコピーする。DMA バッファは 4096 バイトで
     * ページ境界に揃っているため、64KB 境界をまたがない。
     * 音量は無音点 (128) からの距離に利得として掛ける。 */
    for (size_t i = 0; i < count; i++) {
        int d = (int)samples[i] - 128;
        sb_dma_buf[i] = (uint8_t)(128 + d * sb_volume / 100);
    }

    /* ISA DMA チャンネル 1 を組む (メモリ読み取り、単発)。 */
    uint32_t phys = (uint32_t)sb_dma_phys;
    prism_outb(DMA_MASK, 0x05); /* チャンネル 1 をマスク */
    prism_outb(DMA_CLEAR_FF, 0x00);
    prism_outb(DMA_MODE, 0x49); /* 単発・増加・読み取り・ch1 */
    prism_outb(DMA_CH1_ADDR, (uint8_t)(phys & 0xFF));
    prism_outb(DMA_CH1_ADDR, (uint8_t)((phys >> 8) & 0xFF));
    prism_outb(DMA_CH1_PAGE, (uint8_t)((phys >> 16) & 0xFF));
    uint32_t len = (uint32_t)count - 1;
    prism_outb(DMA_CH1_COUNT, (uint8_t)(len & 0xFF));
    prism_outb(DMA_CH1_COUNT, (uint8_t)((len >> 8) & 0xFF));
    prism_outb(DMA_MASK, 0x01); /* チャンネル 1 を許可 */

    /* DSP: スピーカーを入れて、サンプルレートを決め、8 ビット
     * 単発 DMA 出力を始める。 */
    if (sb_write_cmd(0xD1) != 0) { /* スピーカー on */
        return -1;
    }
    uint8_t tc = (uint8_t)(256 - 1000000u / rate);
    if (sb_write_cmd(0x40) != 0 || sb_write_cmd(tc) != 0) {
        return -1;
    }
    if (sb_write_cmd(0x14) != 0 || /* 8 ビット単発 DMA 出力 */
        sb_write_cmd((uint8_t)(len & 0xFF)) != 0 ||
        sb_write_cmd((uint8_t)((len >> 8) & 0xFF)) != 0) {
        return -1;
    }
    sb_total = (uint32_t)count;
    sb_prev_played = 0;
    sb_active = 1;
    return 0;
}

/* DMA チャンネル 1 の残り転送数を読む。終わると
 * 0xFFFF に巻き戻る。 */
static uint32_t sb_remaining(void) {
    prism_outb(DMA_CLEAR_FF, 0x00);
    uint32_t lo = prism_inb(DMA_CH1_COUNT);
    uint32_t hi = prism_inb(DMA_CH1_COUNT);
    return lo | (hi << 8);
}

int sb16_is_playing(void) {
    if (!sb_active) {
        return 0;
    }
    uint32_t rem = sb_remaining();
    if (rem == 0xFFFF) {
        return 0;
    }
    /* QEMU の SB16 は停止命令が来るまで同じブロックを
     * 繰り返すため、終端で残量が初期値へ巻き戻る。
     * 再生位置が前回より大きく戻ったら 1 周したとみなす。 */
    uint32_t played = sb_total - 1 - rem;
    if (played + 64 < sb_prev_played) {
        return 0;
    }
    sb_prev_played = played;
    return 1;
}

uint32_t sb16_played(void) {
    if (!sb_active) {
        return 0;
    }
    uint32_t rem = sb_remaining();
    if (rem == 0xFFFF) {
        return sb_total;
    }
    return sb_total - 1 - rem;
}

uint32_t sb16_total(void) {
    return sb_active ? sb_total : 0;
}

void sb16_pause(void) {
    if (sb_active) {
        (void)sb_write_cmd(0xD0); /* DMA 出力を止める */
    }
}

void sb16_resume(void) {
    if (sb_active) {
        (void)sb_write_cmd(0xD4); /* DMA 出力を再開する */
    }
}

void sb16_stop(void) {
    if (!sb_active) {
        return;
    }
    (void)sb_write_cmd(0xD0); /* DMA 停止 */
    (void)sb_write_cmd(0xD3); /* スピーカー off */
    prism_outb(DMA_MASK, 0x05); /* チャンネル 1 をマスク */
    (void)prism_inb(SB_IRQ_STATUS); /* 割り込み状態を確認 (解除) */
    sb_active = 0;
    sb_total = 0;
}

int sb16_play_8bit(const uint8_t *samples, size_t count,
                   uint32_t rate) {
    if (sb16_start_8bit(samples, count, rate) != 0) {
        return -1;
    }
    /* 転送が終わるまで待つ (DSP がサンプルレートで進める)。 */
    speaker_wait_ticks((uint32_t)count * 1193182u / rate);
    sb16_stop();
    return 0;
}
