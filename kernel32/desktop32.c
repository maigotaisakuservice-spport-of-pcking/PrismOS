/* PrismOS — 32bit 版 GUI デスクトップ (Bochs VBE)。
 *
 * `desktop` コマンドで入る。Bochs VBE (DISPI) で
 * 1024x768x32 のリニアフレームバッファを設定し、共有の
 * fb.c で壁紙・タスクバー・端末ウィンドウを描く。マウスは
 * PS/2 をポーリングで読み (割り込みなし)、タイトルバーの
 * ドラッグとタスクバーのボタンが効く。端末ウィンドウは
 * テキストモードと同じシェル (main32_exec_line) をその
 * まま動かすので、GUI の中でも全コマンドが使える。
 * 終了はタスクバーの "Text mode" ボタンか `text` コマンド。 */
#include <stddef.h>
#include <stdint.h>

#include "fb.h"
#include "kernel32.h"
#include "pci.h"
#include "rtc.h"
#include "string.h"

static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define DISPI_INDEX 0x01CE
#define DISPI_DATA 0x01CF
#define DISPI_ID 0
#define DISPI_XRES 1
#define DISPI_YRES 2
#define DISPI_BPP 3
#define DISPI_ENABLE 4
#define DISPI_VIRT_WIDTH 6
#define DISPI_VIRT_HEIGHT 7

#define SCR_W 1024
#define SCR_H 768
#define TASKBAR_H 40
#define WIN_W 920
#define WIN_H 620
#define TITLE_H 28
#define CELL_W 12 /* 6 * scale 2 */
#define CELL_H 16 /* 8 * scale 2 */
#define COLS 74
#define ROWS 35

static uint16_t dispi_read(uint16_t reg) {
    outw(DISPI_INDEX, reg);
    return inw(DISPI_DATA);
}

static void dispi_write(uint16_t reg, uint16_t v) {
    outw(DISPI_INDEX, reg);
    outw(DISPI_DATA, v);
}

static int vbe_probed;
static int vbe_ok;
static uint8_t vga_bus, vga_slot, vga_func;

int desktop32_available(void) {
    if (!vbe_probed) {
        vbe_probed = 1;
        if (pci_find_class(0x03, 0x00, 0x00, &vga_bus, &vga_slot,
                           &vga_func) != 0) {
            uint16_t id = dispi_read(DISPI_ID);
            if (id >= 0xB0C0 && id <= 0xB0C5) {
                vbe_ok = 1;
            }
        }
    }
    return vbe_ok;
}

/* ---- 端末グリッド (con_sink の受け先) ---- */
static char grid[ROWS][COLS];
static int grid_x, grid_y;
static int grid_dirty;

static void grid_scroll(void) {
    for (int r = 0; r + 1 < ROWS; r++) {
        memcpy(grid[r], grid[r + 1], COLS);
    }
    memset(grid[ROWS - 1], ' ', COLS);
}

static void grid_putc(char c) {
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        grid_x = 0;
        grid_y++;
    } else if (c == '\b') {
        if (grid_x > 0) {
            grid_x--;
        }
    } else if (c >= ' ' && c <= '~') {
        if (grid_y < ROWS && grid_x < COLS) {
            grid[grid_y][grid_x] = c;
        }
        grid_x++;
        if (grid_x >= COLS) {
            grid_x = 0;
            grid_y++;
        }
    }
    if (grid_y >= ROWS) {
        grid_scroll();
        grid_y = ROWS - 1;
    }
    grid_dirty = 1;
}

/* ---- マウス (PS/2 ポーリング) ---- */
/* 初期位置は左上 (0,0): エミュレータの相対マウス移動と
 * 座標が一致し、自動テストから決定的に操作できる。 */
static int mouse_x = 0, mouse_y = 0;
static int mouse_btn; /* bit0 = 左 */
static int mouse_moved;
static uint8_t pkt[3];
static int pkt_n;

static void mouse_byte(uint8_t b) {
    if (pkt_n == 0 && (b & 0x08) == 0) {
        return; /* パケット先頭の同期ビット待ち */
    }
    pkt[pkt_n++] = b;
    if (pkt_n < 3) {
        return;
    }
    pkt_n = 0;
    int dx = pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
    int dy = pkt[2] - ((pkt[0] & 0x20) ? 256 : 0);
    mouse_x += dx;
    mouse_y -= dy;
    if (mouse_x < 0) {
        mouse_x = 0;
    }
    if (mouse_y < 0) {
        mouse_y = 0;
    }
    if (mouse_x >= SCR_W) {
        mouse_x = SCR_W - 1;
    }
    if (mouse_y >= SCR_H) {
        mouse_y = SCR_H - 1;
    }
    mouse_btn = pkt[0] & 0x07;
    mouse_moved = 1;
}

static int ps2_wait_read(int spins) {
    for (int i = 0; i < spins; i++) {
        if ((inb(0x64) & 0x01) != 0) {
            return inb(0x60);
        }
    }
    return -1;
}

static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) {
            return;
        }
    }
}

static void mouse_write(uint8_t v) {
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, v);
    (void)ps2_wait_read(100000); /* ACK (0xFA) を読み捨てる */
}

static void mouse_init(void) {
    ps2_wait_write();
    outb(0x64, 0xA8); /* マウスポート有効化 */
    ps2_wait_write();
    outb(0x64, 0x20); /* 設定バイト読み */
    int cfg = ps2_wait_read(100000);
    if (cfg >= 0) {
        ps2_wait_write();
        outb(0x64, 0x60);
        ps2_wait_write();
        outb(0x60, (uint8_t)(cfg & ~0x20)); /* マウスクロック許可 */
    }
    mouse_write(0xF6); /* 既定値に設定 */
    mouse_write(0xF4); /* データ送信開始 */
}

/* ---- 描画 ---- */
static uint32_t *lfb;
static int win_x = 52, win_y = 44;
static int win_shown = 1;
static int wallpaper_variant;

static uint32_t col_wall_a, col_wall_b, col_task, col_title,
    col_win_bg, col_win_fg, col_btn;

static void render(void) {
    /* 壁紙: 上下のグラデーション帯 */
    for (int band = 0; band < 16; band++) {
        uint32_t c = wallpaper_variant == 0
                         ? (band < 8 ? col_wall_a : col_wall_b)
                         : (band < 8 ? col_wall_b : col_wall_a);
        fb_fill_rect(0, (uint64_t)(band * (SCR_H / 16)), SCR_W,
                     SCR_H / 16, c);
    }
    /* タスクバー */
    fb_fill_rect(0, SCR_H - TASKBAR_H, SCR_W, TASKBAR_H, col_task);
    fb_fill_rect(8, SCR_H - TASKBAR_H + 6, 76, TASKBAR_H - 12,
                 col_btn);
    fb_draw_text(20, SCR_H - TASKBAR_H + 12, "Prism", 0xFFFFFF,
                 col_btn, 2);
    if (!win_shown) {
        fb_fill_rect(100, SCR_H - TASKBAR_H + 6, 110,
                     TASKBAR_H - 12, col_btn);
        fb_draw_text(112, SCR_H - TASKBAR_H + 12, "Terminal",
                     0xFFFFFF, col_btn, 2);
    }
    fb_fill_rect(SCR_W - 240, SCR_H - TASKBAR_H + 6, 110,
                 TASKBAR_H - 12, col_btn);
    fb_draw_text(SCR_W - 228, SCR_H - TASKBAR_H + 12, "Text mode",
                 0xFFFFFF, col_btn, 2);
    /* 時計 */
    prism_rtc_time t;
    char clock[16];
    int p = 0;
    clock[0] = '\0';
    if (rtc_read(&t) == 0) {
        clock[p++] = (char)('0' + t.hour / 10);
        clock[p++] = (char)('0' + t.hour % 10);
        clock[p++] = ':';
        clock[p++] = (char)('0' + t.minute / 10);
        clock[p++] = (char)('0' + t.minute % 10);
        clock[p++] = ':';
        clock[p++] = (char)('0' + t.second / 10);
        clock[p++] = (char)('0' + t.second % 10);
        clock[p] = '\0';
    }
    fb_draw_text(SCR_W - 110, SCR_H - TASKBAR_H + 12, clock,
                 0xFFFFFF, col_task, 2);
    /* 端末ウィンドウ */
    if (win_shown) {
        fb_fill_rect((uint64_t)win_x, (uint64_t)win_y, WIN_W,
                     WIN_H, col_win_bg);
        fb_fill_rect((uint64_t)win_x, (uint64_t)win_y, WIN_W,
                     TITLE_H, col_title);
        fb_draw_text((uint64_t)(win_x + 10), (uint64_t)(win_y + 6),
                     "Terminal - PrismOS shell", 0xFFFFFF,
                     col_title, 2);
        /* 閉じる箱 (隠す) */
        fb_fill_rect((uint64_t)(win_x + WIN_W - 26),
                     (uint64_t)(win_y + 4), 20, 20, 0xC0392B);
        fb_draw_text((uint64_t)(win_x + WIN_W - 21),
                     (uint64_t)(win_y + 6), "x", 0xFFFFFF, 0xC0392B,
                     2);
        int tx = win_x + 10;
        int ty = win_y + TITLE_H + 8;
        for (int r = 0; r < ROWS; r++) {
            char row[COLS + 1];
            memcpy(row, grid[r], COLS);
            row[COLS] = '\0';
            /* 末尾の空白は落としてよい (背景は塗り済み) */
            fb_draw_text((uint64_t)tx, (uint64_t)(ty + r * CELL_H),
                         row, col_win_fg, col_win_bg, 2);
        }
    }
    fb_flush();
}

/* カーソル: フロントバッファへ直接描く (退避/復元つき) */
#define CUR_W 12
#define CUR_H 18
static uint32_t cur_under[CUR_W * CUR_H];
static int cur_drawn, cur_sx, cur_sy;

static const char *cursor_shape[CUR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ",
    "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
    "X.......X   ", "X........X  ", "X.....XX    ", "X..X..X     ",
    "X.X X..X    ", "XX  X..X    ", "     X..X   ", "     X..X   ",
    "      XX    ", "            ",
};

static void cursor_hide(void) {
    if (!cur_drawn) {
        return;
    }
    for (int y = 0; y < CUR_H; y++) {
        for (int x = 0; x < CUR_W; x++) {
            int px = cur_sx + x, py = cur_sy + y;
            if (px < SCR_W && py < SCR_H) {
                lfb[py * SCR_W + px] = cur_under[y * CUR_W + x];
            }
        }
    }
    cur_drawn = 0;
}

static void cursor_show(void) {
    cur_sx = mouse_x;
    cur_sy = mouse_y;
    for (int y = 0; y < CUR_H; y++) {
        for (int x = 0; x < CUR_W; x++) {
            int px = cur_sx + x, py = cur_sy + y;
            if (px < SCR_W && py < SCR_H) {
                cur_under[y * CUR_W + x] = lfb[py * SCR_W + px];
            }
        }
    }
    for (int y = 0; y < CUR_H; y++) {
        for (int x = 0; x < CUR_W; x++) {
            char s = cursor_shape[y][x];
            if (s == ' ') {
                continue;
            }
            int px = cur_sx + x, py = cur_sy + y;
            if (px < SCR_W && py < SCR_H) {
                lfb[py * SCR_W + px] =
                    (s == 'X') ? 0x000000 : 0xFFFFFF;
            }
        }
    }
    cur_drawn = 1;
}

/* ---- 本体 ---- */
static int running;

void desktop32_run(prism_shell_state *state) {
    if (running) {
        main32_con_write("desktop: already running\n");
        return;
    }
    if (!desktop32_available()) {
        main32_con_write("desktop: unavailable\n");
        return;
    }
    running = 1;
    uint32_t bar = pci_bar32_mem(vga_bus, vga_slot, vga_func, 0);
    lfb = (uint32_t *)(uintptr_t)(bar & ~0xFu);
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, SCR_W);
    dispi_write(DISPI_YRES, SCR_H);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, SCR_W);
    dispi_write(DISPI_VIRT_HEIGHT, SCR_H);
    dispi_write(DISPI_ENABLE, 0x01 | 0x40); /* 有効 + LFB */
    fb_set_hhdm(0);
    fb_init(lfb, SCR_W, SCR_H, SCR_W * 4, 32, 8, 16, 8, 8, 8, 0);
    col_wall_a = fb_rgb(0x1D, 0x4E, 0x89);
    col_wall_b = fb_rgb(0x2E, 0x86, 0xC1);
    col_task = fb_rgb(0x1B, 0x1B, 0x1B);
    col_title = fb_rgb(0x24, 0x56, 0xA6);
    col_win_bg = fb_rgb(0x10, 0x10, 0x18);
    col_win_fg = fb_rgb(0xE8, 0xE8, 0xE8);
    col_btn = fb_rgb(0x33, 0x33, 0x33);
    memset(grid, ' ', sizeof(grid));
    grid_x = 0;
    grid_y = 0;
    grid_dirty = 1;
    main32_con_write("[desktop32] framebuffer 1024x768x32\n");
    mouse_init();
    main32_set_mouse_sink(mouse_byte);
    main32_set_con_sink(grid_putc);
    render();
    cursor_show();
    main32_con_write("prismos> ");

    static char line[256];
    size_t len = 0;
    int prev_btn = 0;
    int dragging = 0, drag_dx = 0, drag_dy = 0;
    int last_sec = -1;
    int exit_now = 0;
    while (!exit_now) {
        int c = main32_poll_char();
        if (c != 0) {
            if (c == '\r' || c == '\n') {
                main32_con_write("\n");
                line[len] = '\0';
                if (strcmp(line, "text") == 0) {
                    exit_now = 1;
                } else if (len > 0) {
                    main32_exec_line(state, line);
                    main32_con_write("prismos> ");
                } else {
                    main32_con_write("prismos> ");
                }
                len = 0;
            } else if (c == '\b' || c == 0x7F) {
                if (len > 0) {
                    len--;
                    main32_con_write("\b \b");
                }
            } else if (c >= ' ' && c <= '~' && len + 1 < sizeof(line)) {
                line[len++] = (char)c;
                main32_con_putc((char)c);
            }
        }
        if (mouse_moved) {
            mouse_moved = 0;
            cursor_hide();
            if (dragging && (mouse_btn & 1)) {
                win_x = mouse_x - drag_dx;
                win_y = mouse_y - drag_dy;
                if (win_x < 0) {
                    win_x = 0;
                }
                if (win_y < 0) {
                    win_y = 0;
                }
                if (win_x > SCR_W - 120) {
                    win_x = SCR_W - 120;
                }
                if (win_y > SCR_H - TASKBAR_H - 60) {
                    win_y = SCR_H - TASKBAR_H - 60;
                }
                render();
            }
            if ((mouse_btn & 1) && !prev_btn) {
                int mx = mouse_x, my = mouse_y;
                char note[64];
                int p = 0;
                const char *pre = "[desktop32] click ";
                while (pre[p] != '\0') {
                    note[p] = pre[p];
                    p++;
                }
                /* 座標を 10 進で足す */
                char num[12];
                int ni = 0;
                int v = mx;
                if (v == 0) {
                    num[ni++] = '0';
                }
                char rev[12];
                int ri = 0;
                while (v > 0) {
                    rev[ri++] = (char)('0' + v % 10);
                    v /= 10;
                }
                while (ri > 0) {
                    num[ni++] = rev[--ri];
                }
                num[ni] = '\0';
                int q = 0;
                while (num[q] != '\0') {
                    note[p++] = num[q++];
                }
                note[p++] = ',';
                ni = 0;
                v = my;
                if (v == 0) {
                    num[ni++] = '0';
                }
                ri = 0;
                while (v > 0) {
                    rev[ri++] = (char)('0' + v % 10);
                    v /= 10;
                }
                while (ri > 0) {
                    num[ni++] = rev[--ri];
                }
                num[ni] = '\0';
                q = 0;
                while (num[q] != '\0') {
                    note[p++] = num[q++];
                }
                note[p++] = '\n';
                note[p] = '\0';
                /* シリアルだけに出す (グリッドには出さない) */
                main32_set_con_sink(NULL);
                main32_con_write(note);
                main32_set_con_sink(grid_putc);
                if (win_shown && mx >= win_x && mx < win_x + WIN_W &&
                    my >= win_y && my < win_y + TITLE_H) {
                    if (mx >= win_x + WIN_W - 26) {
                        win_shown = 0;
                        render();
                    } else {
                        dragging = 1;
                        drag_dx = mx - win_x;
                        drag_dy = my - win_y;
                    }
                } else if (my >= SCR_H - TASKBAR_H) {
                    if (mx >= SCR_W - 240 && mx < SCR_W - 130) {
                        exit_now = 1; /* Text mode ボタン */
                    } else if (!win_shown && mx >= 100 &&
                               mx < 210) {
                        win_shown = 1;
                        render();
                    }
                } else if (!win_shown || mx < win_x ||
                           mx >= win_x + WIN_W || my < win_y ||
                           my >= win_y + WIN_H) {
                    wallpaper_variant ^= 1;
                    render();
                }
            }
            if (!(mouse_btn & 1)) {
                dragging = 0;
            }
            prev_btn = mouse_btn & 1;
            cursor_show();
        }
        if (grid_dirty) {
            grid_dirty = 0;
            cursor_hide();
            render();
            cursor_show();
        }
        prism_rtc_time t;
        if (rtc_read(&t) == 0 && t.second != last_sec) {
            last_sec = t.second;
            cursor_hide();
            render();
            cursor_show();
        }
    }
    cursor_hide();
    main32_set_con_sink(NULL);
    main32_set_mouse_sink(NULL);
    dispi_write(DISPI_ENABLE, 0);
    main32_text_mode();
    running = 0;
    main32_con_write("[desktop32] back to text mode\n");
}
