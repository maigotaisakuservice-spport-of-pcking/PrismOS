/* PrismOS — ARM 用 virtio-input キーボードの実装。
 * レガシー virtio-mmio の eventq (キュー 0) に 8 バイトの
 * イベントバッファを複数張り、used になったものを読んで
 * 再投入する。イベントは {type:u16, code:u16, value:u32}。
 * EV_KEY (1) の押下 (value 1) だけを文字に直す。離す
 * イベントとリピート (value 2) は捨てる (Shift の状態だけ
 * 押下・解放の両方で追う)。 */
#include "virtio_input.h"

#include "uart.h"
#include "virtio.h"

#define VIRTIO_ID_INPUT 18
#define EV_KEY 1
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54

#define INPUT_BUFS 8

static virtio_dev g_dev;
static int g_ready;
static int g_shift;

/* イベントバッファ (デバイスが書くので静的に置く)。 */
static uint8_t g_bufs[INPUT_BUFS][8];

/* evdev キーコード → ASCII (小文字側)。0 は文字なし。 */
static const char key_map[64] = {
    [2] = '1',  [3] = '2',  [4] = '3',  [5] = '4',  [6] = '5',
    [7] = '6',  [8] = '7',  [9] = '8',  [10] = '9', [11] = '0',
    [12] = '-', [13] = '=', [14] = '\b', [15] = '\t',
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '[', [27] = ']', [28] = '\n',
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l',
    [39] = ';', [40] = '\'', [41] = '`', [43] = '\\',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = 'm', [51] = ',', [52] = '.', [53] = '/',
    [57] = ' ',
};

static const char key_map_shift[64] = {
    [2] = '!',  [3] = '@',  [4] = '#',  [5] = '$',  [6] = '%',
    [7] = '^',  [8] = '&',  [9] = '*',  [10] = '(', [11] = ')',
    [12] = '_', [13] = '+', [14] = '\b', [15] = '\t',
    [16] = 'Q', [17] = 'W', [18] = 'E', [19] = 'R', [20] = 'T',
    [21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
    [26] = '{', [27] = '}', [28] = '\n',
    [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
    [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L',
    [39] = ':', [40] = '"', [41] = '~', [43] = '|',
    [44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B',
    [49] = 'N', [50] = 'M', [51] = '<', [52] = '>', [53] = '?',
    [57] = ' ',
};

static void post_buffer(int i) {
    virtio_seg seg;
    seg.buf = (void *)g_bufs[i];
    seg.len = 8;
    seg.write = 1;
    virtio_queue_add(&g_dev, 0, &seg, 1, (void *)(uintptr_t)i);
}

int arm_virtio_input_init(void) {
    if (virtio_mmio_probe(VIRTIO_ID_INPUT, &g_dev) != 0) {
        return -1;
    }
    if (virtio_queue_init(&g_dev, 0) != 0) {
        return -2;
    }
    for (int i = 0; i < INPUT_BUFS; i++) {
        post_buffer(i);
    }
    virtio_driver_ok(&g_dev);
    virtio_queue_notify(&g_dev, 0);
    g_ready = 1;
    arm_uart_write_line("[boot] input: virtio keyboard ready");
    return 0;
}

int arm_virtio_input_poll_char(void) {
    if (!g_ready) {
        return 0;
    }
    uint32_t id = 0;
    uint32_t len = 0;
    void *cookie = NULL;
    if (virtio_queue_poll(&g_dev, 0, &id, &len, &cookie) != 0) {
        return 0;
    }
    int idx = (int)(uintptr_t)cookie;
    const uint8_t *ev = g_bufs[idx];
    uint16_t type = (uint16_t)(ev[0] | (ev[1] << 8));
    uint16_t code = (uint16_t)(ev[2] | (ev[3] << 8));
    uint32_t value = (uint32_t)ev[4] | ((uint32_t)ev[5] << 8) |
                     ((uint32_t)ev[6] << 16) | ((uint32_t)ev[7] << 24);
    post_buffer(idx);
    virtio_queue_notify(&g_dev, 0);
    if (type != EV_KEY) {
        return 0;
    }
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        g_shift = (value != 0);
        return 0;
    }
    if (value != 1 || code >= 64) {
        return 0;
    }
    char c = g_shift ? key_map_shift[code] : key_map[code];
    return (uint8_t)c;
}
