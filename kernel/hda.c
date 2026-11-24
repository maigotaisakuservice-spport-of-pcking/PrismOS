/* PrismOS — Intel HD Audio の実装。 */

#include "hda.h"

#include "hda_cap.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "speaker.h"
#include "string.h"

/* コントローラレジスタ (BAR0 からのオフセット) */
#define HDA_GCAP 0x00
#define HDA_GCTL 0x08
#define HDA_STATESTS 0x0E
#define HDA_ICOI 0x60
#define HDA_ICII 0x64
#define HDA_ICSI 0x68
/* ストリーム記述子は 0x80 から 0x20 バイトずつ */
#define HDA_SD_BASE 0x80
#define HDA_SD_CTL 0x00
#define HDA_SD_STS 0x03
#define HDA_SD_LPIB 0x04
#define HDA_SD_CBL 0x08
#define HDA_SD_LVI 0x0C
#define HDA_SD_FMT 0x12
#define HDA_SD_BDPL 0x18
#define HDA_SD_BDPU 0x1C

#define HDA_TIMEOUT 1000000

static volatile uint8_t *hda_regs;
static uint64_t hda_hhdm;
static int hda_found;
static int hda_stream_idx; /* 出力ストリームの番号 */
static uint32_t hda_codec_vendor;
static int hda_codec_addr;
static int hda_dac_nid;
static int hda_pin_nid;
static uint64_t hda_bdl_phys;
static uint64_t hda_pcm_phys; /* 2 ページ = 2 バッファ */

/* 録音側: 入力ストリーム数、ADC、マイクピン、録音バッファ。 */
static int hda_in_streams;
static int hda_afg_nid;
static int hda_adc_nid;
static int hda_mic_pin_nid;
static uint64_t hda_cap_bdl_phys;
static uint64_t hda_cap_pcm_phys;
static uint64_t hda_cap_play_bdl_phys;

/* 採取バッファの大きさは kernel/hda_cap.h (HDA_CAP_BYTES)。 */

static uint16_t reg16(uint32_t off) {
    return *(volatile uint16_t *)(hda_regs + off);
}
static uint32_t reg32(uint32_t off) {
    return *(volatile uint32_t *)(hda_regs + off);
}
static void reg16w(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(hda_regs + off) = v;
}
static void reg32w(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(hda_regs + off) = v;
}

static void print_hex_small(uint32_t v) {
    const char *hex = "0123456789abcdef";
    char b[9];
    for (int i = 0; i < 8; i++) {
        b[i] = hex[(v >> (28 - 4 * i)) & 0xF];
    }
    b[8] = '\0';
    serial_write(b);
}

/* Immediate Command でバーブを送り応答を得る。 */
static int hda_verb(uint32_t codec, uint32_t nid, uint32_t verb,
                    uint32_t *resp) {
    uint32_t cmd = (codec << 28) | (nid << 20) | verb;
    /* 前回分が残っていればクリアする。 */
    reg16w(HDA_ICSI, 0x2);
    reg32w(HDA_ICOI, cmd);
    reg16w(HDA_ICSI, 0x1); /* ICB: 実行 */
    for (int i = 0; i < HDA_TIMEOUT; i++) {
        uint16_t st = reg16(HDA_ICSI);
        if ((st & 0x1) == 0) {
            if (st & 0x2) { /* IRV: 応答有効 */
                if (resp != NULL) {
                    *resp = reg32(HDA_ICII);
                }
                reg16w(HDA_ICSI, 0x2);
                return 0;
            }
            return -1;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static uint32_t verb_get_param(uint32_t codec, uint32_t nid,
                               uint32_t param) {
    uint32_t r = 0;
    if (hda_verb(codec, nid, (0xF00 << 8) | param, &r) != 0) {
        return 0xFFFFFFFFu;
    }
    return r;
}

int hda_init(uint64_t hhdm_offset) {
    uint8_t bus, slot, func;
    hda_found = 0;
    hda_hhdm = hhdm_offset;
    if (!pci_find_class(0x04, 0x03, 0x00, &bus, &slot, &func)) {
        return 0;
    }
    uint32_t bar0 = pci_bar32_mem(bus, slot, func, 0);
    uint64_t bar_phys = bar0 & 0xFFFFFFF0u;
    if (bar_phys == 0) {
        return 0;
    }
    if ((bar0 & 0x6) == 0x4) {
        uint32_t hi = pci_config_read32(bus, slot, func, 0x14);
        bar_phys |= (uint64_t)hi << 32;
    }
    pci_enable_bus_master(bus, slot, func);
    if (paging_map_mmio(bar_phys, 0x4000, hhdm_offset) != 0) {
        return 0;
    }
    hda_regs = (volatile uint8_t *)(uintptr_t)(bar_phys +
                                               hhdm_offset);
    uint16_t gcap = reg16(HDA_GCAP);
    /* GCAP: OSS [15:12], ISS [11:8], BSS [7:4], 64OK [0]。
     * 出力ストリームは入力ストリームの後ろに並ぶ。 */
    int iss = (gcap >> 8) & 0xF;
    int oss = (gcap >> 12) & 0xF;
    if (oss < 1) {
        serial_write_line("[hda] no output streams");
        return 0;
    }
    hda_stream_idx = iss; /* 入力の後が出力ストリーム */
    hda_in_streams = iss;
    serial_write("[hda] GCAP=");
    {
        char b[5];
        const char *hex = "0123456789abcdef";
        for (int i = 0; i < 4; i++) {
            b[i] = hex[(gcap >> (12 - 4 * i)) & 0xF];
        }
        b[4] = '\0';
        serial_write(b);
    }
    serial_write_line("");

    /* コントローラリセット: CRST を落として上げ直す。 */
    reg32w(HDA_GCTL, 0);
    for (int i = 0; i < HDA_TIMEOUT; i++) {
        if ((reg32(HDA_GCTL) & 1) == 0) {
            break;
        }
        __asm__ volatile("pause");
    }
    reg32w(HDA_GCTL, 1);
    int ready = 0;
    for (int i = 0; i < HDA_TIMEOUT; i++) {
        if (reg32(HDA_GCTL) & 1) {
            ready = 1;
            break;
        }
        __asm__ volatile("pause");
    }
    if (!ready) {
        serial_write_line("[hda] controller reset failed");
        return 0;
    }
    /* コーデックの目覚めを少し待つ。 */
    for (int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }
    uint16_t statests = reg16(HDA_STATESTS);
    if (statests == 0) {
        serial_write_line("[hda] no codec found");
        return 0;
    }
    int codec = 0;
    while ((statests & (1 << codec)) == 0) {
        codec++;
    }
    hda_codec_addr = codec;

    /* ルートノード → 機能グループ (AFG) → ウィジェット列挙。 */
    uint32_t root_nc = verb_get_param(codec, 0, 0x04);
    if (root_nc == 0xFFFFFFFFu) {
        serial_write_line("[hda] codec does not answer");
        return 0;
    }
    hda_codec_vendor = verb_get_param(codec, 0, 0x00);
    int afg = (int)((root_nc >> 16) & 0xFF);
    hda_afg_nid = afg;
    uint32_t afg_nc = verb_get_param(codec, afg, 0x04);
    int wstart = (int)((afg_nc >> 16) & 0xFF);
    int wcount = (int)(afg_nc & 0xFF);
    hda_dac_nid = -1;
    hda_pin_nid = -1;
    hda_adc_nid = -1;
    hda_mic_pin_nid = -1;
    for (int i = 0; i < wcount; i++) {
        int nid = wstart + i;
        uint32_t wcaps = verb_get_param(codec, nid, 0x09);
        if (wcaps == 0xFFFFFFFFu) {
            continue;
        }
        int type = (int)((wcaps >> 20) & 0xF);
        if (type == 0 && hda_dac_nid < 0) {
            hda_dac_nid = nid; /* Audio Output (DAC) */
        }
        if (type == 1 && hda_adc_nid < 0) {
            hda_adc_nid = nid; /* Audio Input (ADC) */
        }
        if (type == 4) {
            if (hda_pin_nid < 0) {
                hda_pin_nid = nid; /* Pin Complex (出力用) */
            }
            /* 入力可能なピン (Pin Caps のビット 5) を
             * マイク候補にする。出力ピンとは別を優先。 */
            uint32_t pcaps = verb_get_param(codec, nid, 0x0C);
            if (pcaps != 0xFFFFFFFFu && (pcaps & (1u << 5)) &&
                nid != hda_pin_nid && hda_mic_pin_nid < 0) {
                hda_mic_pin_nid = nid;
            }
        }
    }
    if (hda_mic_pin_nid < 0 && hda_pin_nid >= 0) {
        /* 別のピンが無ければ出力ピンの入力能力を見る。 */
        uint32_t pcaps =
            verb_get_param(codec, hda_pin_nid, 0x0C);
        if (pcaps != 0xFFFFFFFFu && (pcaps & (1u << 5))) {
            hda_mic_pin_nid = hda_pin_nid;
        }
    }
    if (hda_dac_nid < 0 || hda_pin_nid < 0) {
        serial_write_line("[hda] no DAC/pin widget found");
        return 0;
    }
    /* 電源 D0、ピン出力有効化、接続選択、アンプ解除。 */
    hda_verb(codec, afg, (0x705 << 8) | 0, NULL);
    hda_verb(codec, hda_dac_nid, (0x705 << 8) | 0, NULL);
    hda_verb(codec, hda_pin_nid, (0x705 << 8) | 0, NULL);
    /* ピンの接続先一覧から DAC を選ぶ。 */
    uint32_t conn = 0;
    if (hda_verb(codec, hda_pin_nid, (0xF02 << 8) | 0, &conn) == 0) {
        int entries = (int)(conn & 0xFF);
        int sel = -1;
        for (int e = 0; e < 4; e++) {
            int enid = (int)((conn >> (8 * e)) & 0xFF);
            if (enid == hda_dac_nid) {
                sel = e;
            }
        }
        (void)entries;
        if (sel > 0) {
            hda_verb(codec, hda_pin_nid, (0x701 << 8) | (uint32_t)sel,
                     NULL);
        }
    }
    hda_verb(codec, hda_pin_nid, (0x707 << 8) | 0xC0, NULL);
    /* 出力アンプ: ミュート解除、ゲイン 0x30、左右とも設定。 */
    hda_verb(codec, hda_dac_nid,
             (0x3 << 16) | (1 << 15) | (1 << 13) | (1 << 12) | 0x30,
             NULL);
    hda_verb(codec, hda_pin_nid,
             (0x3 << 16) | (1 << 15) | (1 << 13) | (1 << 12) | 0x30,
             NULL);

    /* BDL と PCM バッファ (2 × 4KB) を用意する。 */
    hda_bdl_phys = pmm_alloc_page();
    hda_pcm_phys = pmm_alloc_pages(2);
    if (!hda_bdl_phys || !hda_pcm_phys) {
        serial_write_line("[hda] no memory for buffers");
        return 0;
    }
    uint32_t *bdl = (uint32_t *)(uintptr_t)(hda_bdl_phys + hhdm_offset);
    for (int i = 0; i < 16; i++) {
        bdl[i] = 0;
    }
    bdl[0] = (uint32_t)(hda_pcm_phys & 0xFFFFFFFFu);
    bdl[1] = (uint32_t)(hda_pcm_phys >> 32);
    bdl[2] = 4096;
    bdl[3] = 1; /* IOC */
    bdl[4] = (uint32_t)((hda_pcm_phys + 4096) & 0xFFFFFFFFu);
    bdl[5] = (uint32_t)((hda_pcm_phys + 4096) >> 32);
    bdl[6] = 4096;
    bdl[7] = 1;

    hda_found = 1;
    serial_write_line("[hda] codec ready (output path configured)");
    serial_write("[hda] adc nid ");
    print_hex_small((uint32_t)hda_adc_nid);
    serial_write(" mic pin nid ");
    print_hex_small((uint32_t)hda_mic_pin_nid);
    serial_write(" in streams ");
    print_hex_small((uint32_t)hda_in_streams);
    serial_write_line("");
    return 1;
}

int hda_present(void) {
    return hda_found;
}

const char *hda_codec_name(void) {
    return hda_found ? "HD Audio codec" : "";
}

static uint32_t sd_base(void) {
    return HDA_SD_BASE + (uint32_t)hda_stream_idx * 0x20;
}

uint32_t hda_stream_pos(void) {
    if (!hda_found) {
        return 0;
    }
    return reg32(sd_base() + HDA_SD_LPIB);
}

int hda_beep(void) {
    if (!hda_found) {
        return -1;
    }
    /* PCM を作る: 48kHz 16bit ステレオ、440Hz の正弦波を
     * 整数テーブルで合成する。 */
    static const int16_t sine16[16] = {
        0, 12539, 23170, 30273, 32767, 30273, 23170, 12539,
        0, -12539, -23170, -30273, -32767, -30273, -23170, -12539
    };
    int16_t *pcm = (int16_t *)(uintptr_t)(hda_pcm_phys + hda_hhdm);
    /* 440Hz は 48000/440 ≈ 109 サンプル周期。テーブルを
     * 線形に引く近似で十分 (試聴用トーン)。 */
    for (int i = 0; i < 4096; i++) {
        int16_t v = sine16[(i / 7) % 16];
        pcm[2 * i] = v;
        pcm[2 * i + 1] = v;
    }
    /* 前の再生を止めてストリームをリセットする。
     * SDnCTL: ビット 0 = SRST、ビット 1 = RUN。 */
    uint32_t base = sd_base();
    reg32w(base + HDA_SD_CTL, 0);
    reg32w(base + HDA_SD_CTL, 0x1); /* SRST */
    for (int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }
    reg32w(base + HDA_SD_CTL, 0);
    for (int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }
    /* ストリーム設定: タグ 1、48kHz/16bit/ステレオ。 */
    reg32w(base + HDA_SD_BDPL, (uint32_t)(hda_bdl_phys & 0xFFFFFFFFu));
    reg32w(base + HDA_SD_BDPU, (uint32_t)(hda_bdl_phys >> 32));
    reg32w(base + HDA_SD_CBL, 8192);
    reg16w(base + HDA_SD_LVI, 1);
    reg16w(base + HDA_SD_FMT, 0x0011);
    /* SET_CHANNEL_STREAMID は 0x706 (0x200 は書式専用)。 */
    hda_verb(hda_codec_addr, hda_dac_nid,
             (0x706 << 8) | (1 << 4), NULL);
    hda_verb(hda_codec_addr, hda_dac_nid,
             (0x2 << 16) | 0x0011, NULL);
    /* タグ 1 + RUN を 1 回の書き込みで設定する。 */
    hda_regs[base + HDA_SD_STS] = 0x1C; /* 状態フラグをクリア */
    reg32w(base + HDA_SD_CTL, (1u << 20) | 0x2);
    serial_write_line("[hda] playing tone");
    return 0;
}

/* --- 録音 --- */

static uint32_t in_sd_base(void) {
    return HDA_SD_BASE; /* 入力ストリーム 0 */
}

uint32_t hda_capture_pos(void) {
    if (!hda_found) {
        return 0;
    }
    return reg32(in_sd_base() + HDA_SD_LPIB);
}

int hda_capture_supported(void) {
    return hda_found && hda_in_streams >= 1 && hda_adc_nid >= 0;
}

static int hda_capture_alloc(void) {
    if (hda_cap_pcm_phys != 0) {
        return 0;
    }
    hda_cap_bdl_phys = pmm_alloc_page();
    hda_cap_play_bdl_phys = pmm_alloc_page();
    hda_cap_pcm_phys =
        pmm_alloc_pages(HDA_CAP_BYTES / 4096);
    if (!hda_cap_bdl_phys || !hda_cap_pcm_phys ||
        !hda_cap_play_bdl_phys) {
        serial_write_line("[hda] no memory for capture buffers");
        return -1;
    }
    uint32_t *bdl =
        (uint32_t *)(uintptr_t)(hda_cap_bdl_phys + hda_hhdm);
    hda_cap_build_bdl(bdl, hda_cap_pcm_phys);
    return 0;
}

const int16_t *hda_capture_data(void) {
    if (hda_cap_pcm_phys == 0) {
        return NULL;
    }
    return (const int16_t *)(uintptr_t)(hda_cap_pcm_phys +
                                        hda_hhdm);
}

int hda_capture_load(const int16_t *src, uint32_t bytes) {
    if (hda_cap_load_ok(src, bytes) != 0 ||
        hda_capture_alloc() != 0) {
        return -1;
    }
    int16_t *dst =
        (int16_t *)(uintptr_t)(hda_cap_pcm_phys + hda_hhdm);
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < bytes; i++) {
        d[i] = s[i];
    }
    return 0;
}

/* ストリームのリセット列 (beep と同じ手順)。 */
static void hda_sd_reset(uint32_t base) {
    reg32w(base + HDA_SD_CTL, 0);
    reg32w(base + HDA_SD_CTL, 0x1); /* SRST */
    for (int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }
    reg32w(base + HDA_SD_CTL, 0);
    for (int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }
}

int hda_record(uint32_t millis, uint32_t *bytes_out,
               uint32_t *peak_out) {
    if (!hda_capture_supported()) {
        return -1;
    }
    if (hda_capture_alloc() != 0) {
        return -1;
    }
    if (millis == 0 || millis > 3000) {
        millis = 2000;
    }
    uint32_t target = hda_cap_target_bytes(millis);

    /* ADC 側の設定: 電源 D0、ピンの入力有効化、接続選択、
     * ストリーム ID (タグ 2)、書式。 */
    int codec = hda_codec_addr;
    hda_verb(codec, hda_afg_nid, (0x705 << 8) | 0, NULL);
    hda_verb(codec, hda_adc_nid, (0x705 << 8) | 0, NULL);
    if (hda_mic_pin_nid >= 0) {
        hda_verb(codec, hda_mic_pin_nid, (0x705 << 8) | 0,
                 NULL);
        /* Pin Widget Control: IN 有効 + VREF 80%。 */
        hda_verb(codec, hda_mic_pin_nid, (0x707 << 8) | 0x23,
                 NULL);
        /* ADC の接続先一覧にマイクピンがあれば選ぶ。 */
        uint32_t conn = 0;
        if (hda_verb(codec, hda_adc_nid, (0xF02 << 8) | 0,
                     &conn) == 0) {
            for (int e = 0; e < 4; e++) {
                int enid = (int)((conn >> (8 * e)) & 0xFF);
                if (enid == hda_mic_pin_nid && e > 0) {
                    hda_verb(codec, hda_adc_nid,
                             (0x701 << 8) | (uint32_t)e, NULL);
                }
            }
        }
    }
    hda_verb(codec, hda_adc_nid, (0x706 << 8) | (2 << 4),
             NULL);
    hda_verb(codec, hda_adc_nid, (0x2 << 16) | 0x0011, NULL);

    uint32_t base = in_sd_base();
    hda_sd_reset(base);
    reg32w(base + HDA_SD_BDPL,
           (uint32_t)(hda_cap_bdl_phys & 0xFFFFFFFFu));
    reg32w(base + HDA_SD_BDPU,
           (uint32_t)(hda_cap_bdl_phys >> 32));
    reg32w(base + HDA_SD_CBL, HDA_CAP_BYTES);
    reg16w(base + HDA_SD_LVI, HDA_CAP_ENTRIES - 1);
    reg16w(base + HDA_SD_FMT, 0x0011);
    hda_regs[base + HDA_SD_STS] = 0x1C;
    reg32w(base + HDA_SD_CTL, (2u << 20) | 0x2); /* タグ 2 + RUN */

    /* 目標時間まで LPIB の進みを見ながら待つ。 */
    uint64_t t0 = speaker_pit0_count();
    uint64_t want = (uint64_t)millis * 1193u;
    uint32_t max_pos = 0;
    for (;;) {
        uint32_t pos = reg32(base + HDA_SD_LPIB);
        if (pos > max_pos && pos <= HDA_CAP_BYTES) {
            max_pos = pos;
        }
        if (speaker_pit0_count() - t0 >= want) {
            break;
        }
        __asm__ volatile("pause");
    }
    reg32w(base + HDA_SD_CTL, 0); /* 停止 */

    uint32_t got = hda_cap_got_bytes(max_pos, target);
    const int16_t *pcm = hda_capture_data();
    uint32_t peak = hda_cap_peak(pcm, got / 2);
    if (bytes_out != NULL) {
        *bytes_out = got;
    }
    if (peak_out != NULL) {
        *peak_out = peak;
    }
    serial_write("[hda] captured ");
    print_hex_small(got);
    serial_write(" bytes, peak ");
    print_hex_small(peak);
    serial_write_line("");
    return got > 0 ? 0 : -1;
}

int hda_play_capture(uint32_t bytes) {
    if (!hda_found || hda_cap_pcm_phys == 0 ||
        bytes == 0 || bytes > HDA_CAP_BYTES) {
        return -1;
    }
    /* 再生用 BDL を録音バッファに向ける。 */
    uint32_t *bdl =
        (uint32_t *)(uintptr_t)(hda_cap_play_bdl_phys +
                                hda_hhdm);
    for (int i = 0; i < 256; i++) {
        bdl[i] = 0;
    }
    uint32_t entries = (bytes + HDA_CAP_ENTRY_BYTES - 1) /
                       HDA_CAP_ENTRY_BYTES;
    for (uint32_t e = 0; e < entries; e++) {
        uint64_t p = hda_cap_pcm_phys +
                     (uint64_t)e * HDA_CAP_ENTRY_BYTES;
        uint32_t left = bytes - e * HDA_CAP_ENTRY_BYTES;
        bdl[e * 4 + 0] = (uint32_t)(p & 0xFFFFFFFFu);
        bdl[e * 4 + 1] = (uint32_t)(p >> 32);
        bdl[e * 4 + 2] =
            left < HDA_CAP_ENTRY_BYTES ? left
                                        : HDA_CAP_ENTRY_BYTES;
        bdl[e * 4 + 3] = 0;
    }
    uint32_t base = sd_base();
    hda_sd_reset(base);
    reg32w(base + HDA_SD_BDPL,
           (uint32_t)(hda_cap_play_bdl_phys & 0xFFFFFFFFu));
    reg32w(base + HDA_SD_BDPU,
           (uint32_t)(hda_cap_play_bdl_phys >> 32));
    reg32w(base + HDA_SD_CBL, bytes);
    reg16w(base + HDA_SD_LVI, (uint16_t)(entries - 1));
    reg16w(base + HDA_SD_FMT, 0x0011);
    hda_verb(hda_codec_addr, hda_dac_nid, (0x706 << 8) | (1 << 4),
             NULL);
    hda_verb(hda_codec_addr, hda_dac_nid, (0x2 << 16) | 0x0011,
             NULL);
    hda_regs[base + HDA_SD_STS] = 0x1C;
    reg32w(base + HDA_SD_CTL, (1u << 20) | 0x2);
    serial_write_line("[hda] playing capture");
    return 0;
}

/* デバッグ: 全ストリーム記述子の状態を出す。 */
void hda_debug_dump(void) {
    if (!hda_found) {
        return;
    }
    for (int i = 0; i < 8; i++) {
        uint32_t b = HDA_SD_BASE + (uint32_t)i * 0x20;
        serial_write(" sd");
        {
            char ch[2] = {(char)('0' + i), '\0'};
            serial_write(ch);
        }
        serial_write(" ctl=");
        print_hex_small(reg32(b + HDA_SD_CTL));
        serial_write(" sts=");
        print_hex_small(hda_regs[b + HDA_SD_STS]);
        serial_write(" lpib=");
        print_hex_small(reg32(b + HDA_SD_LPIB));
        serial_write(" cbl=");
        print_hex_small(reg32(b + HDA_SD_CBL));
        serial_write_line("");
    }
}
