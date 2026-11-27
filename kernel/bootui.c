/* PrismOS — 起動時グラフィカル画面 (bootui) の実装。
 * 描画は fb の UI テキスト (セル metrics) を使い、デスクトップ
 * と同じ見た目にそろえる。全画面を描き直す単純なモデルで、
 * 入力のたびに呼んでも実用上問題ない大きさに収める。 */
#include "bootui.h"

#include "fb.h"
#include "string.h"

static int bu_lang; /* 0 = 英語、1 = 日本語 */

#define COL_BG_TOP 0x0B1026
#define COL_BG_BOT 0x16224A
#define COL_PANEL 0x141D38
#define COL_PANEL_EDGE 0x4CC2FF
#define COL_FIELD 0x0A0F22
#define COL_TEXT 0xFFFFFF
#define COL_MUTED 0x9AA7BD
#define COL_ERROR 0xFF8080
#define COL_OK 0x7CE3A1

static uint32_t rgb(uint32_t v) {
    return fb_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8),
                  (uint8_t)v);
}

int bootui_available(void) {
    return fb_ready();
}

void bootui_set_language(int lang) {
    bu_lang = lang ? 1 : 0;
}

static const char *pick(const char *en, const char *ja) {
    return bu_lang ? ja : en;
}

/* 背景 (縦グラデーション) + ロゴ + プリズムドット列。 */
static void draw_background(void) {
    uint64_t w = fb_width();
    uint64_t h = fb_height();
    for (uint64_t band = 0; band < 32; band++) {
        uint64_t y0 = h * band / 32;
        uint64_t y1 = h * (band + 1) / 32;
        uint32_t r = 0x0B + (0x16 - 0x0B) * (uint32_t)band / 31;
        uint32_t g = 0x10 + (0x22 - 0x10) * (uint32_t)band / 31;
        uint32_t b = 0x26 + (0x4A - 0x26) * (uint32_t)band / 31;
        fb_fill_rect(0, y0, w, y1 - y0, fb_rgb((uint8_t)r,
                                              (uint8_t)g,
                                              (uint8_t)b));
    }
    /* ロゴ文字とドット列 (起動ロゴと同じ 8 色)。 */
    const char *logo = "PrismOS";
    uint64_t lw = fb_text_ui_width(logo, 2);
    fb_draw_text_ui((w - lw) / 2, 72, logo, rgb(COL_PANEL_EDGE),
                    rgb(COL_BG_TOP), 2);
    static const uint32_t dot_rgb[8] = {
        0xE5484D, 0xF76B15, 0xFFB224, 0x46A758,
        0x0090FF, 0x3E63DD, 0x8E4EC6, 0xE93D82
    };
    uint64_t dots_w = 8 * 26;
    uint64_t dx = (w - dots_w) / 2;
    for (int d = 0; d < 8; d++) {
        uint64_t cx = dx + (uint64_t)d * 26 + 8;
        for (int dy = -5; dy <= 5; dy++) {
            int span = 0;
            while ((span + 1) * (span + 1) + dy * dy <= 25) {
                span++;
            }
            fb_fill_rect(cx - (uint64_t)span, (uint64_t)(128 + dy),
                         (uint64_t)(2 * span + 1), 1, rgb(dot_rgb[d]));
        }
    }
}

/* パネルの外枠。戻り値はパネル左上 (px, py)。 */
static void draw_panel(uint64_t pw, uint64_t ph, uint64_t *px,
                       uint64_t *py) {
    uint64_t w = fb_width();
    uint64_t h = fb_height();
    *px = (w - pw) / 2;
    *py = (h - ph) / 2 + 30;
    /* パネルは角丸 + 1px 輪郭 (デスクトップと共通の作法)。 */
    fb_fill_round_rect((int64_t)*px, (int64_t)*py, (int64_t)pw,
                       (int64_t)ph, 8, rgb(COL_PANEL));
    fb_stroke_round_rect((int64_t)*px, (int64_t)*py, (int64_t)pw,
                         (int64_t)ph, 8, rgb(COL_PANEL_EDGE));
}

static void draw_centered(uint64_t cx, uint64_t y, const char *s,
                          uint32_t col, int unit) {
    uint64_t tw = fb_text_ui_width(s, unit);
    fb_draw_text_ui(cx - tw / 2, y, s, col, rgb(COL_PANEL), unit);
}

/* ---- プロンプト画面の状態 ---- */
static char pr_title[96];
static char pr_sub[128];
static char pr_label[64];
static char pr_field[72];
static int pr_field_is_dots;
static int pr_field_dots;
static char pr_msg[128];
static int pr_msg_error;
static int pr_have_prompt;

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (src == NULL) {
        src = "";
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

#define PANEL_W 620
#define PANEL_H 330

static void prompt_redraw(void) {
    if (!fb_ready() || !pr_have_prompt) {
        return;
    }
    draw_background();
    uint64_t px, py;
    draw_panel(PANEL_W, PANEL_H, &px, &py);
    uint64_t cx = px + PANEL_W / 2;
    draw_centered(cx, py + 26, pr_title, rgb(COL_TEXT), 1);
    if (pr_sub[0] != '\0') {
        draw_centered(cx, py + 62, pr_sub, rgb(COL_MUTED), 1);
    }
    if (pr_label[0] != '\0') {
        fb_draw_text_ui(px + 44, py + 118, pr_label, rgb(COL_MUTED),
                        rgb(COL_PANEL), 1);
    }
    /* 入力欄 */
    fb_fill_round_rect((int64_t)(px + 42), (int64_t)(py + 146),
                       (int64_t)(PANEL_W - 84), 44, 4,
                       rgb(COL_FIELD));
    fb_stroke_round_rect((int64_t)(px + 42), (int64_t)(py + 146),
                         (int64_t)(PANEL_W - 84), 44, 4,
                         rgb(COL_PANEL_EDGE));
    if (pr_field_is_dots) {
        char dots[40];
        int n = pr_field_dots;
        if (n > 32) {
            n = 32;
        }
        for (int i = 0; i < n; i++) {
            dots[i] = '*';
        }
        dots[n] = '\0';
        fb_draw_text_ui(px + 58, py + 158, n > 0 ? dots : " ",
                        rgb(COL_TEXT), rgb(COL_FIELD), 1);
    } else if (pr_field[0] != '\0') {
        fb_draw_text_ui(px + 58, py + 158, pr_field, rgb(COL_TEXT),
                        rgb(COL_FIELD), 1);
    }
    if (pr_msg[0] != '\0') {
        draw_centered(cx, py + 214, pr_msg,
                      pr_msg_error ? rgb(COL_ERROR) : rgb(COL_OK), 1);
    }
    draw_centered(cx, py + PANEL_H - 44,
                  pick("Enter: continue", "Enter: 続行"),
                  rgb(COL_MUTED), 1);
    fb_flush();
}

void bootui_prompt(const char *title_en, const char *title_ja,
                   const char *sub_en, const char *sub_ja,
                   const char *label_en, const char *label_ja) {
    copy_str(pr_title, sizeof(pr_title), pick(title_en, title_ja));
    copy_str(pr_sub, sizeof(pr_sub), pick(sub_en, sub_ja));
    copy_str(pr_label, sizeof(pr_label), pick(label_en, label_ja));
    pr_field[0] = '\0';
    pr_field_is_dots = 1;
    pr_field_dots = 0;
    pr_msg[0] = '\0';
    pr_msg_error = 0;
    pr_have_prompt = 1;
    prompt_redraw();
}

void bootui_field_dots(int len) {
    pr_field_is_dots = 1;
    pr_field_dots = len;
    prompt_redraw();
}

void bootui_field_text(const char *text) {
    pr_field_is_dots = 0;
    copy_str(pr_field, sizeof(pr_field), text);
    prompt_redraw();
}

void bootui_message(const char *en, const char *ja, int is_error) {
    copy_str(pr_msg, sizeof(pr_msg), pick(en, ja));
    pr_msg_error = is_error;
    prompt_redraw();
}

/* ---- ユーザーチップ ----
 * パネルの外 (上) に横一列で描く。prompt_redraw とは別に、
 * ログイン画面専用の描画として prompt の後に重ねる。 */
static char chip_names[8][32];
static int chip_count;
static int chip_selected;

void bootui_login_users(const char *const *names, int count,
                        int selected) {
    chip_count = count > 8 ? 8 : count;
    for (int i = 0; i < chip_count; i++) {
        copy_str(chip_names[i], sizeof(chip_names[i]), names[i]);
    }
    chip_selected = selected;
    if (!fb_ready() || !pr_have_prompt) {
        return;
    }
    /* プロンプト再描画の上にチップ列を重ねる代わりに、
     * パネル内タイトル下へ描く。 */
    prompt_redraw();
    uint64_t w = fb_width();
    uint64_t h = fb_height();
    uint64_t py = (h - PANEL_H) / 2 + 30;
    uint64_t chip_w = 132;
    uint64_t total = (uint64_t)chip_count * chip_w +
                     (uint64_t)(chip_count - 1) * 10;
    uint64_t x = (w - total) / 2;
    uint64_t y = py + 88;
    for (int i = 0; i < chip_count; i++) {
        uint32_t edge = (i == chip_selected) ? rgb(COL_PANEL_EDGE)
                                              : rgb(0x3A4763);
        fb_fill_round_rect((int64_t)x, (int64_t)y, (int64_t)chip_w,
                           28, 4, rgb(COL_FIELD));
        fb_stroke_round_rect((int64_t)x, (int64_t)y,
                             (int64_t)chip_w, 28, 4, edge);
        /* 頭文字アイコン (円 + 頭文字) */
        char init[2] = { chip_names[i][0], '\0' };
        if (init[0] >= 'a' && init[0] <= 'z') {
            init[0] = (char)(init[0] - 'a' + 'A');
        }
        fb_fill_rect(x + 5, y + 5, 18, 18, rgb(COL_PANEL_EDGE));
        fb_draw_text_ui(x + 9, y + 6, init, rgb(0x0A0F22),
                        rgb(COL_PANEL_EDGE), 1);
        /* 名前 (幅に合わせて切る) */
        char nm[16];
        copy_str(nm, sizeof(nm), chip_names[i]);
        fb_draw_text_ui(x + 29, y + 6, nm, rgb(COL_TEXT),
                        rgb(COL_FIELD), 1);
        x += chip_w + 10;
    }
    fb_flush();
}

/* ---- メニュー画面 (回復) ---- */
static void draw_menu_screen(const char *title,
                             const char *const *items, int count,
                             int selected,
                             const char *const *lines, int nlines) {
    if (!fb_ready()) {
        return;
    }
    draw_background();
    uint64_t px, py;
    draw_panel(PANEL_W, 430, &px, &py);
    uint64_t cx = px + PANEL_W / 2;
    draw_centered(cx, py + 24, title, rgb(COL_TEXT), 1);
    for (int i = 0; i < count && i < 8; i++) {
        uint64_t y = py + 70 + (uint64_t)i * 40;
        if (i == selected) {
            fb_fill_rect(px + 30, y - 4, PANEL_W - 60, 34,
                         rgb(COL_PANEL_EDGE));
            fb_draw_text_ui(px + 48, y, items[i], rgb(0x0A0F22),
                            rgb(COL_PANEL_EDGE), 1);
        } else {
            fb_fill_rect(px + 30, y - 4, PANEL_W - 60, 34,
                         rgb(COL_FIELD));
            fb_draw_text_ui(px + 48, y, items[i], rgb(COL_TEXT),
                            rgb(COL_FIELD), 1);
        }
    }
    if (lines != NULL) {
        for (int i = 0; i < nlines && i < 8; i++) {
            fb_draw_text_ui(px + 44, py + 70 + (uint64_t)count * 40 +
                                     14 + (uint64_t)i * 22,
                            lines[i], rgb(COL_MUTED), rgb(COL_PANEL),
                            1);
        }
    }
    fb_flush();
}

static char menu_title[96];
static char menu_items[8][96];
static int menu_count;
static int menu_selected;
static char menu_lines[8][96];
static int menu_nlines;

void bootui_menu(const char *title_en, const char *title_ja,
                 const char *const *items_en,
                 const char *const *items_ja, int count, int selected) {
    copy_str(menu_title, sizeof(menu_title),
             pick(title_en, title_ja));
    menu_count = count > 8 ? 8 : count;
    for (int i = 0; i < menu_count; i++) {
        copy_str(menu_items[i], sizeof(menu_items[i]),
                 pick(items_en[i], items_ja[i]));
    }
    menu_selected = selected;
    menu_nlines = 0;
    const char *ptrs[8];
    for (int i = 0; i < menu_count; i++) {
        ptrs[i] = menu_items[i];
    }
    draw_menu_screen(menu_title, ptrs, menu_count, menu_selected,
                     NULL, 0);
}

void bootui_text_lines(const char *const *lines, int count) {
    menu_nlines = count > 8 ? 8 : count;
    for (int i = 0; i < menu_nlines; i++) {
        copy_str(menu_lines[i], sizeof(menu_lines[i]), lines[i]);
    }
    const char *ptrs[8];
    for (int i = 0; i < menu_count; i++) {
        ptrs[i] = menu_items[i];
    }
    const char *lptrs[8];
    for (int i = 0; i < menu_nlines; i++) {
        lptrs[i] = menu_lines[i];
    }
    draw_menu_screen(menu_title, ptrs, menu_count, menu_selected,
                     lptrs, menu_nlines);
}

int bootui_menu_hit(int x, int y) {
    if (!fb_ready()) {
        return -1;
    }
    uint64_t w = fb_width();
    uint64_t h = fb_height();
    uint64_t px = (w - PANEL_W) / 2;
    uint64_t py = (h - 430) / 2 + 30;
    if (x < (int)(px + 30) || x >= (int)(px + PANEL_W - 30)) {
        return -1;
    }
    for (int i = 0; i < menu_count; i++) {
        int y0 = (int)(py + 70 + (uint64_t)i * 40) - 4;
        if (y >= y0 && y < y0 + 34) {
            return i;
        }
    }
    return -1;
}

void bootui_draw_cursor(int x, int y) {
    /* デスクトップと同じ白い矢印 (簡易版)。 */
    static const char shape[8][7] = {
        "X.....", "XX....", "XXX...", "XXXX..",
        "XXXXX.", "XXXXXX", "XXX...", "XX...."
    };
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 6; c++) {
            if (shape[r][c] == 'X') {
                fb_fill_rect((uint64_t)(x + c), (uint64_t)(y + r),
                             2, 2, rgb(0xFFFFFF));
            }
        }
    }
    fb_flush();
}
