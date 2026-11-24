/* PrismOS - desktop (screen apps). */
#include "desktop.h"
#include "appguard.h"
#include "cards.h"
#include "compat_run.h"
#include "fb.h"
#include "jfont.h"
#include "../shell/doc.h"
#include "../shell/docx.h"
#include "../shell/pptx.h"
#include "../shell/slides.h"
#include "heap.h"
#include "io.h"
#include "ime.h"
#include "keyboard.h"
#include "mailproto.h"
#include "../shell/mime.h"
#include "../shell/png.h"
#include "../shell/js.h"
#include "mouse.h"
#include "vbe.h"
#include "vinput.h"
#include "net.h"
#include "netstack.h"
#include "persist.h"
#include "rtc.h"
#include "sb16.h"
#include "search.h"
#include "security.h"
#include "drvload.h"
#include "serial.h"
#include "settings.h"
#include "shell.h"
#include "account.h"
#include "acpi.h"
#include "bio.h"
#include "crypto.h"
#include "totp.h"
#include "users.h"
#include "version.h"
#include "shooter.h"
#include "video.h"
#include "wallpaper.h"
#include "wav.h"
#include "string.h"
#include "../shell/install.h"
#include "../shell/mail.h"
#include "../shell/sheet.h"
#include "../shell/vfs.h"
#include "../shell/storepkg.h"
#include "../shell/pdf.h"
#include "../shell/charset.h"
#include "store.h"

#define TASKBAR_H 48
#define TASKBAR_W 120 /* 縦タスクバー (左右配置) の幅 */
#define DOCK_H 44
#define TITLE_H 28
/* セル寸法とコンソールのグリフ倍率は、文字サイズ設定
 * (prism_settings.text_scale) で実行時に決まる。
 * desk_settings はこの下で定義されるため、寸法関数は
 * 前方宣言だけここに置く。 */
static int text_scale_now(void);
static int cell_w_impl(void);
static int cell_h_impl(void);
static int text_cols(void);
static int text_rows(void);
#define CELL_W (cell_w_impl())
#define CELL_H (cell_h_impl())
#define TEXT_SCALE \
    (text_scale_now() >= 3 ? 4 : text_scale_now() >= 2 ? 3 : 2)
#define WIN_MARGIN 16

typedef enum {
    DESK_TERMINAL = 0,
    DESK_FILES = 1,
    DESK_SETTINGS = 2,
    DESK_BROWSER = 3,
    DESK_NOTES = 4,
    DESK_CALENDAR = 5,
    DESK_PHOTOS = 6,
    DESK_MUSIC = 7,
    DESK_CARDS = 8,
    DESK_SHOOTER = 9,
    DESK_SHEET = 10,
    DESK_VIDEO = 11,
    DESK_INSTALLER = 12,
    DESK_MAIL = 13,
    DESK_DOCS = 14,
    DESK_SLIDES = 15,
    DESK_CONTACTS = 16,
    DESK_EDITOR = 17,
    DESK_PAINT = 18,
    DESK_STORE = 19,
    DESK_NOTIF = 20
} desktop_mode;

#define DESK_APP_COUNT 21

/* Which apps the Dock shows. A minimal install (settings
 * install_profile=minimal) shows only the terminal, the file
 * manager, settings and the browser; a standard install
 * shows all sixteen. */
static const desktop_mode desk_standard_apps[DESK_APP_COUNT] = {
    DESK_TERMINAL, DESK_FILES, DESK_SETTINGS, DESK_BROWSER,
    DESK_NOTES, DESK_CALENDAR, DESK_PHOTOS, DESK_MUSIC,
    DESK_CARDS, DESK_SHOOTER, DESK_SHEET, DESK_VIDEO,
    DESK_INSTALLER, DESK_MAIL, DESK_DOCS, DESK_SLIDES,
    DESK_CONTACTS, DESK_EDITOR, DESK_PAINT, DESK_STORE,
    DESK_NOTIF,
};
static const desktop_mode desk_minimal_apps[4] = {
    DESK_TERMINAL, DESK_FILES, DESK_SETTINGS, DESK_BROWSER,
};

static int desk_ok;
static int console_ok;
static desktop_mode desk_mode = DESK_TERMINAL;
static prism_settings desk_settings;

/* 文字サイズ (アクセシビリティ): 1 = 標準、2 = 大。
 * 大ではセルが 24x32 になり、行数・列数が減って
 * レイアウトが追従する。 */
static int text_scale_now(void) {
    if (desk_settings.text_scale >= 3) {
        return 3;
    }
    return desk_settings.text_scale >= 2 ? 2 : 1;
}
static int cell_w_impl(void) {
    return text_scale_now() >= 3 ? 36
           : text_scale_now() >= 2 ? 24
                                   : 12;
}
static int cell_h_impl(void) {
    return text_scale_now() >= 3 ? 48
           : text_scale_now() >= 2 ? 32
                                   : 16;
}

static int save_desk_settings(void);
static int settings_saved;
static int settings_save_failed;

/* 変更したら即保存する (自動保存)。保存に失敗した
 * ときは settings_save_failed を立てて、画面に
 * 「保存できませんでした」と明示する。 */
static void settings_autosave(void) {
    if (save_desk_settings() == 0) {
        settings_saved = 1;
        settings_save_failed = 0;
    } else {
        settings_saved = 0;
        settings_save_failed = 1;
    }
}

/* Save the desktop-owned settings fields. The stored
 * settings are reloaded first and only the desktop's
 * fields are overlaid, so this save cannot roll back
 * security or update changes made from the shell since
 * the desktop loaded its copy (the shell reloads before
 * its own saves for the same reason). desk_settings is
 * replaced by the merged result. Returns 0 on success. */
static int save_desk_settings(void) {
    prism_settings merged;
    settings_defaults(&merged);
    size_t len = 0;
    const char *saved = vfs_read_ptr("settings.txt", &len);
    if (saved != NULL && len > 0) {
        settings_parse(&merged, saved, len);
    }
    settings_overlay_desktop(&merged, &desk_settings);
    desk_settings = merged;
    char buf[SETTINGS_TEXT_CAP];
    size_t n = settings_serialize(&merged, buf, sizeof(buf));
    if (n >= sizeof(buf)) {
        /* 切り捨てた設定は保存しない (監査 M1)。失敗は
         * settings_autosave が画面に明示する。 */
        return -1;
    }
    if (shell_session_is_guest()) {
        /* ゲストの変更はセッション内だけ (監査 L5)。VFS に
         * 書くと、後の実ユーザーの同期でディスクへ永続化
         * されてしまう。次回セッションは desktop_init が
         * ディスクの設定を読み直すので、変更は消える。 */
        serial_write_line("settings: guest session, not saved");
        shell_notify_language_changed(merged.language);
        return 0;
    }
    {
        int rc = vfs_write("settings.txt", buf, n);
        if (rc == 0) {
            /* 言語が変わっていたら実行中のシェルへ知らせる
             * (help の言語が再起動なしで切り替わる)。 */
            shell_notify_language_changed(merged.language);
        }
        return rc;
    }
}
/* アプリ許可リスト (フェーズ14)。allowlist.txt から読む。
 * 無ければ全部許可。Terminal / Files / Settings は常に許可。 */
static int desk_allowed[SEC_APP_COUNT];
/* 許可リストで止めたときに画面に出すお知らせ (次の切り替えで消える)。 */
static char desk_notice[96];

static void desk_load_allowlist(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr("allowlist.txt", &len);
    allowlist_parse(text, len, desk_allowed);
}

void desktop_reload_allowlist(void) {
    desk_load_allowlist();
}

static const char *mode_name(desktop_mode m);

/* 標準アプリの並びでの位置 (-1 = なし)。 */
static int app_std_index(desktop_mode m) {
    for (int i = 0; i < DESK_APP_COUNT; i++) {
        if (desk_standard_apps[i] == m) {
            return i;
        }
    }
    return -1;
}

/* アンインストール済み (隠し) かどうか。先頭 3 つ
 * (端末 / ファイル / 設定) は外せない (項目17)。 */
static int app_is_hidden(desktop_mode m) {
    int idx = app_std_index(m);
    if (idx < 3) {
        return 0;
    }
    return (desk_settings.hidden_apps >> idx) & 1;
}

static int desk_visible_count(void) {
    int n = desk_settings.install_profile ? 4 : DESK_APP_COUNT;
    int c = 0;
    for (int i = 0; i < n; i++) {
        desktop_mode m = desk_settings.install_profile
                             ? desk_minimal_apps[i]
                             : desk_standard_apps[i];
        if (!app_is_hidden(m)) {
            c++;
        }
    }
    return c;
}

static desktop_mode desk_visible_mode(int i) {
    int n = desk_settings.install_profile ? 4 : DESK_APP_COUNT;
    for (int k = 0; k < n; k++) {
        desktop_mode m = desk_settings.install_profile
                             ? desk_minimal_apps[k]
                             : desk_standard_apps[k];
        if (!app_is_hidden(m)) {
            if (i == 0) {
                return m;
            }
            i--;
        }
    }
    return DESK_TERMINAL;
}

/* アプリを外す / 戻す (項目17)。設定に保存する。 */
static int app_set_hidden(desktop_mode m, int hidden) {
    int idx = app_std_index(m);
    if (idx < 3) {
        return -1; /* 端末 / ファイル / 設定は外せない */
    }
    if (hidden) {
        desk_settings.hidden_apps |= (1 << idx);
    } else {
        desk_settings.hidden_apps &= ~(1 << idx);
    }
    settings_autosave();
    serial_write(hidden ? "apps: uninstalled "
                        : "apps: reinstalled ");
    serial_write_line(mode_name(m));
    return 0;
}

static int desk_mode_visible(desktop_mode m) {
    int n = desk_visible_count();
    for (int i = 0; i < n; i++) {
        if (desk_visible_mode(i) == m) {
            return 1;
        }
    }
    return 0;
}
/* The console grid holds one Unicode codepoint per cell.
 * A full-width character takes two cells: its codepoint in
 * the first and CONSOLE_WIDE_TAIL in the second. Shell
 * output arrives as UTF-8 bytes; desktop_console_putc
 * decodes them with a small pending-byte state. */
#define CONSOLE_WIDE_TAIL 0xFFFFFFFFu
static uint32_t *console_grid; /* cols * rows codepoints */
static uint8_t console_pending[4];
static int console_pending_len;
static int console_pending_need;
static int console_cols;
static int console_rows;
static int console_col;
static int console_row;
static int console_cells; /* 確保済みグリッドのセル数 */
/* スクロールバック (項目16): 画面から流れ出た行を
 * ためる履歴と、表示オフセット (0 = 最新)。 */
#define CONSOLE_HIST_LINES 200
static uint32_t *console_hist;
static int console_hist_count; /* 履歴に入った行の総数 */
static int console_hist_cols;
static int console_view_offset;

/* 文字サイズの変更に合わせてコンソールの行列数を
 * 再計算する。グリッドの確保量を超えない範囲で広げる。 */
static void console_refit(void) {
    if (!console_ok || console_cells <= 0) {
        return;
    }
    int cols = text_cols();
    int rows = text_rows();
    if (cols <= 0 || rows <= 0) {
        return;
    }
    while (cols > 1 && cols * rows > console_cells) {
        rows--;
    }
    while (rows > 1 && cols * rows > console_cells) {
        cols--;
    }
    if (cols != console_cols) {
        /* 列数が変わったら履歴は幅が合わないので捨てる
         * (履歴の行幅は確保時のまま)。 */
        console_hist_count = 0;
        console_view_offset = 0;
    }
    console_cols = cols;
    console_rows = rows;
    if (console_col >= console_cols) {
        console_col = console_cols - 1;
    }
    if (console_row >= console_rows) {
        console_row = console_rows - 1;
    }
}
static int files_selected;
static int files_viewing; /* -1 = list, else file index */
/* Browser state: fetch state, scroll offset in lines, and
 * the address bar (host, host/path, or with an http:// or
 * https:// prefix). The renderer state lives with the
 * browser code below. */
static int browser_state; /* 0 = not fetched, 1 = ok, -1 = failed */
static int browser_status;
static int browser_scroll;
static char browser_url[256] = "example.com";
static int browser_url_len = 11;
/* Notes: editable text (max 16384, the file size limit). */
static char *notes_buf;
static size_t notes_len;
static size_t notes_cursor;
static int notes_loaded;
static int notes_dirty;
static int notes_saved;
/* Calendar: months away from the current month. */
static int cal_offset;
/* Photos: which .bmp file is selected, and which one is shown
 * (-1 = the list). */
static int photos_selected;
static int photos_viewing;
static char photos_status[96];
/* 表示の拡張 (バッチ4 項目11): ズーム段階 (0=全体、
 * 1=2倍、2=4倍)、回転 (0-3 = 90度単位)、スライド
 * ショー (3秒ごとに次へ)。 */
static int photos_zoom;
static int photos_rot;
static int photos_slideshow;
static int photos_slide_second;
static int photos_slide_count;
static uint8_t photos_rot_buf[PRISM_VFS_MAX_FILE_SIZE];

/* 画像を回転した画素列を作る。成功したら 0 を返し、
 * *out_w/*out_h に回転後の寸法を入れる。 */
static int photos_rotate_build(const wallpaper_img *img, int rot,
                               int *out_w, int *out_h) {
    int w = img->width;
    int h = img->height;
    int ow = (rot % 2 == 1) ? h : w;
    int oh = (rot % 2 == 1) ? w : h;
    if (ow <= 0 || oh <= 0 ||
        (size_t)ow * (size_t)oh * 3 > sizeof(photos_rot_buf)) {
        return -1;
    }
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            int sx = 0;
            int sy = 0;
            if (rot == 1) { /* 90度 時計回り */
                sx = y;
                sy = h - 1 - x;
            } else if (rot == 2) {
                sx = w - 1 - x;
                sy = h - 1 - y;
            } else if (rot == 3) {
                sx = w - 1 - y;
                sy = x;
            } else {
                sx = x;
                sy = y;
            }
            int raw_y = img->top_down ? sy : (h - 1 - sy);
            const uint8_t *sp = img->pixels +
                                (size_t)raw_y * (size_t)img->stride +
                                (size_t)sx * 3;
            uint8_t *dp = photos_rot_buf +
                          ((size_t)y * (size_t)ow + (size_t)x) * 3;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
        }
    }
    *out_w = ow;
    *out_h = oh;
    return 0;
}
/* Music: which .wav file is selected, whether the audio device
 * was found, and the last playback result. */
static int music_selected;
static int music_checked;
static int music_has_device;
static int music_state; /* 0 = idle, 1 = played ok, -1 = failed */
/* Cards (blackjack): the deck, hands, and the game phase
 * (0 = not started, 1 = player turn, 2 = finished). */
static int cards_deck[PRISM_DECK_SIZE];
static int cards_pos;
static int cards_player[12];
static int cards_player_n;
static int cards_dealer[12];
static int cards_dealer_n;
static int cards_phase;
static int cards_wins;
static int cards_losses;
static int cards_pushes;
/* Mouse cursor position and previous button state. */
static int mouse_x;
static int mouse_y;
static int mouse_prev_buttons;
/* ドラッグ&ドロップ (項目18): Files の一覧でファイルを
 * つかみ、フォルダ行・ゴミ箱行・Dock の対応アプリへ
 * 落とせる。 */
static int drag_pending;
static int drag_active;
static char drag_name[40];
static int drag_start_x;
static int drag_start_y;
/* Notifications: a small stack of toasts in the top-right
 * corner. desktop_notify() queues one; each lives for a few
 * seconds of desktop_tick time and is drawn by
 * draw_notifications() on top of everything except the
 * cursor. */
#define NOTIFY_MAX 4
#define NOTIFY_TEXT_MAX 64
static char notify_text[NOTIFY_MAX][NOTIFY_TEXT_MAX];
/* 通知の履歴 (消えた通知も後から見返せるように、
 * 通知アプリで一覧する)。 */
#define NOTIFY_HIST_MAX 24
static char notify_hist[NOTIFY_HIST_MAX][NOTIFY_TEXT_MAX];
static int notify_hist_count;

/* 通知履歴の永続化 (以前は RAM のみで再起動で消えた)。
 * notify.txt に 1 行 1 件、古い順に保存する。 */
#define NOTIFY_FILE "notify.txt"
static void notify_hist_save(void) {
    static char buf[NOTIFY_HIST_MAX * (NOTIFY_TEXT_MAX + 1)];
    size_t p = 0;
    int total = notify_hist_count < NOTIFY_HIST_MAX
                    ? notify_hist_count
                    : NOTIFY_HIST_MAX;
    int start = notify_hist_count - total;
    for (int i = 0; i < total; i++) {
        const char *s = notify_hist[(start + i) % NOTIFY_HIST_MAX];
        while (*s != '\0' && p + 1 < sizeof(buf)) {
            buf[p++] = (*s == '\n') ? ' ' : *s;
            s++;
        }
        if (p + 1 < sizeof(buf)) {
            buf[p++] = '\n';
        }
    }
    if (vfs_write(NOTIFY_FILE, buf, p) == 0) {
        persist_maybe_sync();
    }
}

static void notify_hist_load(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr(NOTIFY_FILE, &len);
    if (text == NULL) {
        return;
    }
    size_t i = 0;
    int count = 0;
    while (i < len && count < NOTIFY_HIST_MAX) {
        size_t j = 0;
        while (i < len && text[i] != '\n' &&
               j + 1 < NOTIFY_TEXT_MAX) {
            notify_hist[count][j++] = text[i++];
        }
        notify_hist[count][j] = '\0';
        while (i < len && text[i] != '\n') {
            i++;
        }
        if (i < len) {
            i++;
        }
        if (j > 0) {
            count++;
        }
    }
    notify_hist_count = count;
}
static int notif_scroll;
static int notify_ttl[NOTIFY_MAX]; /* seconds left, 0 = empty */
static int notify_count;
/* Clipboard (text). One shared buffer: Notes copies/cuts a
 * line or the Files app copies a file name, and Notes (or a
 * window) pastes it back. */
#define CLIP_MAX 1024
static char desk_clipboard[CLIP_MAX];
static size_t desk_clipboard_len;
/* App launcher panel (start-menu equivalent): a grid of all
 * apps plus the window entries, toggled from the taskbar. */
static int launcher_active;
static int launcher_selected;
/* Context menu (right click). kind 0 = desktop background,
 * 1 = Files list. Items are drawn from menu_items(). */
static int menu_active;
static int menu_kind;
static int menu_x;
static int menu_y;
static int menu_file_row;
/* Floating windows over the terminal screen: Notes, a
 * calculator and a clock. They can overlap, be dragged by
 * the title bar, raised by clicking, and closed from the
 * title bar's [x]. Only in terminal mode (the mode itself
 * stays a full-screen app model; windows are an overlay). */
#define WIN_COUNT 3
#define WIN_NOTES 0
#define WIN_CALC 1
#define WIN_CLOCK 2
static const char *const WIN_NAMES[WIN_COUNT] = {
    "Notes", "Calculator", "Clock"
};
static int win_open[WIN_COUNT];
static int win_px[WIN_COUNT];
static int win_py[WIN_COUNT];
static int win_z[WIN_COUNT]; /* higher = more front */
static int win_focus;        /* window index or -1 */
static int win_drag;         /* window being dragged or -1 */
static int win_drag_dx;
static int win_drag_dy;
static int win_z_top;
/* Calculator state (integer only). */
static char calc_display[24];
static int calc_len;
static int64_t calc_acc;
static int calc_op; /* 0 none, '+', '-', '*', '/' */
static int calc_fresh; /* next digit starts a new number */
/* Shooter (3D game): the game state, a one-frame hit marker,
 * and the last RTC second the enemy AI ran on. */
static shooter_game shooter_game_state;
static int shooter_hitmark;
static int shooter_last_ai_second;
/* Sheet (spreadsheet): the grid, the selected cell, and the
 * edit buffer for the cell being typed into. */
static sheet_state sheet_grid;
static int sheet_sel_col;
static int sheet_sel_row;

/* Video: the "vid-*.pv1" (compressed) or "vid-*.bmp" frames
 * in the file system (VFS indices, name order), the playback
 * state, and the tick calibration for 4 fps (ticks per
 * second, measured from the RTC's second changes). PV1
 * frames are decoded into video_pixels (RGB565); the work
 * buffers are static storage, not stack. */
static video_player video_player_state;
static int video_frames[VIDEO_MAX_FRAMES];
static int video_frame_count;
static int64_t video_next_tick;
static int64_t video_ticks_per_sec;
static int64_t video_tick_at_last_second;
static int video_last_second;
static int video_calibrated;
static uint16_t video_pixels[PV1_MAX_PIXELS];
static uint8_t video_scratch[PV1_MAX_PIXELS * 2];
static int video_is_pv1;
static int video_decoded_frame; /* frame in video_pixels (-1 = none) */
static uint16_t video_pv1_w;
static uint16_t video_pv1_h;
/* Installer: the current step, the chosen target and profile,
 * the disk as read when the app was opened, and the install
 * progress / result. Target: 0 = automatic, 1 = data
 * partition, 2 = whole disk. Profile: -1 = not chosen yet,
 * 0 = standard, 1 = minimal. */
static install_step inst_step;
static int inst_target;
static int inst_profile;
static int inst_has_disk;
static install_disk_kind inst_kind;
static install_partition inst_parts[INSTALL_PARTITION_COUNT];
static int inst_part_count;
static char inst_model[48];
static uint32_t inst_sectors;
static int inst_progress; /* 0-100 while installing */
static char inst_phase[64];
static char inst_error[96];
static int inst_verified;
/* Mail: the message files in the file system (VFS indices,
 * ordered by message number), the selected message, the
 * message being read (-1 = the list), the read scroll
 * offset, and the compose form (field 0 = To, 1 = Subject,
 * 2 = Body, 3 = Attach (a VFS file name), 4 = Send).
 * The status line reports the last
 * send / receive result. */
static int mail_list_idx[PRISM_VFS_MAX_FILES];
static int mail_list_count;
static int mail_selected;
static int mail_viewing = -1;
static int mail_scroll;
static int mail_composing;
static int mail_field;
static char mail_to[96];
static char mail_subject[96];
static char mail_attach_name[32];
/* Folder view: "" means Inbox. "Sent" lists the
 * sent-NNNN.txt files; "Trash" and custom names filter
 * by mail-folders.txt. */
static char mail_folder[24];
static int mail_move_mode;
static char mail_filter[48];
static int mail_search_mode;
static char mail_search_buf[48];
static size_t mail_search_len;
static char mail_move_buf[24];
static size_t mail_move_len;
static char mail_body[4096];
static size_t mail_body_len;
static char mail_status[96];
/* Embedded wallpaper image (kernel/wallpaper.bmp via wallpaper.S). */
extern const uint8_t prism_wallpaper_data[];
extern const uint8_t prism_wallpaper_end[];
static wallpaper_img wallpaper_image;
static int wallpaper_ok;
/* 自分の壁紙 (写真アプリで選んだ 24bit BMP)。wallpaper_img の
 * pixels は元データを指すだけなので、バイト列を専用バッファに
 * 保持してから解析する。 */
static uint8_t wp_custom_buf[PRISM_VFS_MAX_APP_FILE_SIZE];
static wallpaper_img wp_custom_img;
static int wp_custom_ok;

/* desk_settings.wallpaper_file の画像を読み直す。ファイルが
 * 無い・読めない形式なら wp_custom_ok = 0 (呼び出し側は標準の
 * 壁紙にフォールバックする)。 */
static void wp_custom_load(void) {
    wp_custom_ok = 0;
    if (desk_settings.wallpaper_file[0] == '\0') {
        return;
    }
    size_t len = 0;
    const char *data = vfs_read_ptr(desk_settings.wallpaper_file, &len);
    if (data == NULL || len == 0 || len > sizeof(wp_custom_buf)) {
        return;
    }
    memcpy(wp_custom_buf, data, len);
    wp_custom_ok =
        wallpaper_parse(wp_custom_buf, len, &wp_custom_img) == 0;
}

/* Search / window list (F8): query, matches, selection. */
#define SEARCH_MAX_RESULTS 32
typedef struct {
    int kind; /* 0 = app, 1 = file, 2 = setting */
    int index;
} search_result;
static search_result search_results[SEARCH_MAX_RESULTS];
static int search_count;
static int search_selected;
static int search_active;
static char search_query[32];
static int search_len;
/* 設定画面のページとショートカット編集の状態。
 * settings_page: 0 = 見た目、1 = ショートカット、
 * 2 = アカウント、3 = セキュリティ、4 = 更新。
 * bind_capture が 1 の間は、次に押された特殊キーを
 * bind_selected のアクションへ割り当てる。 */
static int settings_page;
static int bind_selected;
static int bind_capture;

/* ---- アカウント/セキュリティ/更新ページの状態 ----
 * 裏側の仕組みはシェルと同じものを使う (users.txt、
 * settings.txt のセキュリティ/更新項目、shell.c の
 * 更新中核)。管理操作はすべて管理者 PIN の確認
 * ダイアログを通す。 */
/* アカウントページのモード。 */
enum {
    ACCT_MODE_LIST = 0,
    ACCT_MODE_ADD,       /* 追加フォーム */
    ACCT_MODE_PASSWD,    /* PIN 変更フォーム */
    ACCT_MODE_DEL,       /* 削除の確認 */
    ACCT_MODE_TOTP_SHOW, /* 生成した TOTP 秘密の表示 */
};
static int acct_mode;
static int acct_selected; /* 0 = 主管理者、1..N = 追加ユーザー、
                             N+1 = [追加] 行 */
static int acct_field;    /* フォームの入力欄 */
static char acct_name[PRISM_USER_NAME_MAX + 1];
static int acct_name_len;
static char acct_pin1[17];
static int acct_pin1_len;
static char acct_pin2[17];
static int acct_pin2_len;
static int acct_role_admin; /* 追加フォームの役割: 1 = 管理者 */
static char acct_target[PRISM_USER_NAME_MAX + 1]; /* 操作対象 */
static char acct_status[128];
static char acct_totp_secret[32];

/* セキュリティページの編集中の値 (保存するまでは仮の値)。 */
static int sec_selected; /* 0..4 = 項目、5 = 保存行、6.. = アプリ */
static int sec_usb;      /* 仮の値: 1 = 外部メディア禁止 */
static int sec_pin_min;
static int sec_pin_digit;
static int sec_lock_attempts;
static int sec_lock_seconds;
static int sec_dirty;
static int sec_pin_app;   /* 許可リスト切り替え対象のアプリ */
static int sec_pin_allow; /* 切り替え後の許可値 */
static char sec_status[128];

/* 更新ページ。 */
static int upd_selected; /* 0 = 確認、1 = 自動適用、2 = チャンネル、
                            3 = 適用、4 = ロールバック */
static int upd_check_rc; /* 最後の確認結果 (未実施は 99) */
static shell_update_info upd_info;
static int upd_confirm; /* 1 = 適用の二度押し待ち、
                           2 = ロールバックの二度押し待ち */
static char upd_status[160];

/* 管理者 PIN 確認ダイアログ (3 ページ共用)。 */
enum {
    PINOP_NONE = 0,
    PINOP_ACCT_ADD,
    PINOP_ACCT_PASSWD,
    PINOP_ACCT_DEL,
    PINOP_ACCT_TOTP_SET,
    PINOP_ACCT_TOTP_OFF,
    PINOP_SEC_SAVE,
    PINOP_SEC_ALLOW,
    PINOP_UPD_AUTO,
    PINOP_UPD_CHANNEL,
    PINOP_UPD_APPLY,
    PINOP_UPD_ROLLBACK,
    PINOP_NET_APPLY,
    PINOP_NET_FEED,
    PINOP_ADM_FDE_ENABLE,
    PINOP_ADM_FDE_DISABLE,
    PINOP_ADM_SNAP_RESTORE,
    PINOP_ADM_SNAP_DELETE,
    PINOP_ADM_DRV_TOGGLE,
    PINOP_ADM_FW_ADD,
    PINOP_ADM_FW_REMOVE,
    PINOP_ADM_FW_CLEAR,
    PINOP_CLK_SET,
    PINOP_CLK_NTP,
};
static int pin_dialog_open;
static int pin_dialog_op;
static char pin_dialog_buf[17];
static int pin_dialog_len;
static int pin_dialog_error;
static char clock_text[6] = "--:--";
static char date_text[12] = "----/--/--";

/* date_text を RTC の日付 (YYYY/MM/DD) で更新する。 */
static void clock_update_date(const prism_rtc_time *t) {
    int yr = 2000 + t->year;
    int p = 0;
    date_text[p++] = (char)('0' + (yr / 1000) % 10);
    date_text[p++] = (char)('0' + (yr / 100) % 10);
    date_text[p++] = (char)('0' + (yr / 10) % 10);
    date_text[p++] = (char)('0' + yr % 10);
    date_text[p++] = '/';
    date_text[p++] = (char)('0' + (t->month / 10) % 10);
    date_text[p++] = (char)('0' + t->month % 10);
    date_text[p++] = '/';
    date_text[p++] = (char)('0' + (t->day / 10) % 10);
    date_text[p++] = (char)('0' + t->day % 10);
    date_text[p] = '\0';
}

static uint8_t last_clock_second = 255;
static unsigned long tick_counter;
/* ダブルクリック判定用: RTC で較正した tick/秒と、
 * Files での直近のクリック行・時刻。 */
static unsigned long desk_ticks_per_sec;
static int desk_cal_second = -1;
static unsigned long desk_tick_at_last_second;
static int files_last_click_row = -1;
static unsigned long files_last_click_tick;

static uint32_t rgb(uint32_t v) {
    return fb_rgb((uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                  (uint8_t)(v & 0xFF));
}

/* Colours, from the current mode. */
static uint32_t col_window_bg(void) {
    if (desk_settings.high_contrast) {
        return rgb(0x000000);
    }
    return desk_settings.dark_mode ? rgb(0x0D1430) : rgb(0xFFFFFF);
}
static uint32_t col_title_bg(void) {
    if (desk_settings.high_contrast) {
        return rgb(0x000000);
    }
    return desk_settings.dark_mode ? rgb(0x141C40) : rgb(0xDCE4F0);
}
static uint32_t col_text(void) {
    if (desk_settings.high_contrast) {
        return rgb(0xFFFFFF);
    }
    return desk_settings.dark_mode ? rgb(0xEBF0FF) : rgb(0x101626);
}
static uint32_t col_taskbar(void) {
    if (desk_settings.high_contrast) {
        return rgb(0x000000);
    }
    return desk_settings.dark_mode ? rgb(0x080C1E) : rgb(0xFFFFFF);
}
static uint32_t col_dock(void) {
    if (desk_settings.high_contrast) {
        return rgb(0x000000);
    }
    return desk_settings.dark_mode ? rgb(0x101630) : rgb(0xEBEEF4);
}
static uint32_t col_accent(void) {
    if (desk_settings.high_contrast) {
        return rgb(0xFFD60A);
    }
    return rgb(desk_settings.accent_rgb);
}
static uint32_t col_dim(void) {
    if (desk_settings.high_contrast) {
        return rgb(0xD8D8D8);
    }
    return desk_settings.dark_mode ? rgb(0x8A94B8) : rgb(0x5A6478);
}

static uint64_t taskbar_y(void) {
    return desk_settings.taskbar_top ? 0 : fb_height() - TASKBAR_H;
}
static int taskbar_vertical(void) {
    return desk_settings.taskbar_side != 0;
}
static uint64_t dock_y(void) {
    if (taskbar_vertical()) {
        return fb_height() - DOCK_H;
    }
    return desk_settings.taskbar_top ? TASKBAR_H
                                     : fb_height() - TASKBAR_H - DOCK_H;
}
/* Dock geometry, shared by the drawing and the click hit
 * test so the two can never disagree (they once did below
 * 1280 px). Position: 0 = bottom strip (above the taskbar),
 * 1 = left edge (vertical). Size scales the buttons.
 * When autohide is on, the Dock is only drawn while the
 * mouse is near its edge (or a panel is open). */
static int dock_strip_h(void) {
    return desk_settings.dock_size ? DOCK_H + 12 : DOCK_H;
}
static int dock_btn_w(void) {
    int want = desk_settings.dock_size ? 92 : 76;
    /* アプリが増えて画面幅に収まらないときは、ボタン幅を
     * 縮めて全アプリが画面内に収まるようにする。 */
    int count = desk_visible_count();
    if (count > 0 && desk_settings.dock_pos != 1) {
        int fit = ((int)fb_width() - 16 - (count - 1) * 4) / count;
        if (fit < want) {
            want = fit < 40 ? 40 : fit;
        }
    }
    return want;
}
static int dock_btn_h(void) {
    return desk_settings.dock_size ? 40 : 32;
}
static int dock_visible_now(void) {
    if (!desk_settings.dock_show) {
        return 0;
    }
    if (!desk_settings.dock_autohide) {
        return 1;
    }
    if (launcher_active || menu_active) {
        return 1;
    }
    if (desk_settings.dock_pos == 1) {
        return mouse_x < dock_btn_w() + 14;
    }
    return mouse_y > (int)dock_y() - 14;
}
/* Fills in the Dock button layout: for the bottom strip,
 * buttons run left to right starting at (start, cross);
 * for the left strip they run top to bottom starting at
 * (cross, start). Returns the visible app count. */
static int dock_layout(int64_t *start, int64_t *cross) {
    int count = desk_visible_count();
    int gap = 4;
    if (desk_settings.dock_pos == 1) {
        *cross = desk_settings.taskbar_side == 1 ? TASKBAR_W + 6 : 6;
        *start = (int64_t)(desk_settings.taskbar_top ? TASKBAR_H : 0) + 8;
    } else {
        int bw = dock_btn_w();
        int total = count * bw + (count - 1) * gap;
        *start = ((int64_t)fb_width() - total) / 2;
        *cross = (int64_t)dock_y() +
                 (dock_strip_h() - dock_btn_h()) / 2;
    }
    return count;
}
/* The app window covers about 78% of the width and 72% of the
 * height, centred in the space between the taskbar and the
 * Dock, so the wallpaper (gradient, rays, prism) stays visible
 * around it instead of being hidden behind the window. */
static uint64_t win_x(void) {
    uint64_t base = 0;
    uint64_t area = fb_width();
    if (desk_settings.taskbar_side == 1) {
        base = TASKBAR_W;
        area = fb_width() - TASKBAR_W;
    } else if (desk_settings.taskbar_side == 2) {
        area = fb_width() - TASKBAR_W;
    }
    uint64_t w = area * 78 / 100;
    return base + (area - w) / 2;
}
static uint64_t win_y(void) {
    if (taskbar_vertical()) {
        uint64_t avail = fb_height() - DOCK_H;
        uint64_t h = avail * 72 / 100;
        return (avail - h) / 2;
    }
    uint64_t top = desk_settings.taskbar_top ? (TASKBAR_H + DOCK_H) : 0;
    uint64_t avail = desk_settings.taskbar_top
                         ? fb_height() - top
                         : fb_height() - TASKBAR_H - DOCK_H;
    uint64_t h = avail * 72 / 100;
    return top + (avail - h) / 2;
}
static uint64_t win_w(void) {
    uint64_t area = fb_width();
    if (taskbar_vertical()) {
        area = fb_width() - TASKBAR_W;
    }
    return area * 78 / 100;
}
static uint64_t win_h(void) {
    if (taskbar_vertical()) {
        return (fb_height() - DOCK_H) * 72 / 100;
    }
    uint64_t top = desk_settings.taskbar_top ? (TASKBAR_H + DOCK_H) : 0;
    uint64_t avail = desk_settings.taskbar_top
                         ? fb_height() - top
                         : fb_height() - TASKBAR_H - DOCK_H;
    return avail * 72 / 100;
}
static uint64_t text_x(void) {
    return win_x() + 8;
}
static uint64_t text_y(void) {
    return win_y() + TITLE_H + 8;
}
static int text_cols(void) {
    uint64_t w = win_w();
    if (w < 16) {
        return 0;
    }
    return (int)((w - 16) / CELL_W);
}
static int text_rows(void) {
    uint64_t h = win_h();
    if (h < TITLE_H + 16) {
        return 0;
    }
    return (int)((h - TITLE_H - 16) / CELL_H);
}

static void draw_text_small(uint64_t x, uint64_t y, const char *s,
                            uint32_t fg, uint32_t bg) {
    fb_draw_text(x, y, s, fg, bg, text_scale_now());
}

static void lerp_rgb(uint32_t a, uint32_t b, int step, int steps,
                     uint8_t *r, uint8_t *g, uint8_t *bl) {
    int ar = (int)((a >> 16) & 0xFF);
    int ag = (int)((a >> 8) & 0xFF);
    int ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF);
    int bg = (int)((b >> 8) & 0xFF);
    int bb = (int)(b & 0xFF);
    *r = (uint8_t)(ar + (br - ar) * step / steps);
    *g = (uint8_t)(ag + (bg - ag) * step / steps);
    *bl = (uint8_t)(ab + (bb - ab) * step / steps);
}

static void draw_wallpaper(void) {
    /* 自分の画像が選ばれていて読み込めていればそれを最優先。
     * 読めなければ標準画像→描画の順にフォールバックする。 */
    if (desk_settings.wallpaper_image == 2 && wp_custom_ok) {
        wallpaper_draw(&wp_custom_img);
        return;
    }
    /* The real image first, when the setting asks for it and the
     * embedded BMP parsed. Otherwise the drawn gradient. */
    if (desk_settings.wallpaper_image && wallpaper_ok) {
        wallpaper_draw(&wallpaper_image);
        return;
    }
    uint64_t w = fb_width();
    uint64_t h = fb_height();
    uint32_t top = desk_settings.dark_mode ? 0x0B1026 : 0xF2F4F8;
    uint32_t bottom = desk_settings.dark_mode ? 0x261A40 : 0xCDD6E8;
    const int bands = 48;
    for (int i = 0; i < bands; i++) {
        uint8_t r, g, b;
        lerp_rgb(top, bottom, i, bands - 1, &r, &g, &b);
        uint64_t y0 = h * (uint64_t)i / (uint64_t)bands;
        uint64_t y1 = h * (uint64_t)(i + 1) / (uint64_t)bands;
        fb_fill_rect(0, y0, w, y1 - y0, fb_rgb(r, g, b));
    }
    /* Light rays from a glass prism: three diagonal bands in
     * the accent colours, drawn as thick lines. The prism sits
     * in the strip above the app window (the window covers the
     * middle of the screen), and the rays run from it to the
     * right edge, passing behind the window. */
    int64_t cx = (int64_t)(w / 2);
    int64_t cy = 52;
    static const uint32_t ray_colours[3] = {0x4CC2FF, 0x7C5CFF, 0xFF6FB5};
    for (int i = 0; i < 3; i++) {
        int64_t y_end = 190 + i * 60;
        for (int t = -1; t <= 1; t++) {
            fb_draw_line(cx + 40, cy + 20 + t, (int64_t)w - 40,
                         y_end + t, rgb(ray_colours[i]));
        }
    }
    /* The prism itself: a triangle outline in the accent colour. */
    uint32_t acc = col_accent();
    fb_draw_line(cx, cy - 42, cx - 38, cy + 30, acc);
    fb_draw_line(cx - 38, cy + 30, cx + 38, cy + 30, acc);
    fb_draw_line(cx + 38, cy + 30, cx, cy - 42, acc);
}

/* GUI の表示言語: 設定 language (0=英語, 1=日本語) で
 * 主要な表示文字列を切り替える (深層チェック A-1)。 */
static const char *L(const char *en, const char *ja) {
    return desk_settings.language ? ja : en;
}

static const char *mode_name(desktop_mode m) {
    if (m == DESK_FILES) {
        return L("Files", "ファイル");
    }
    if (m == DESK_SETTINGS) {
        return L("Settings", "設定");
    }
    if (m == DESK_BROWSER) {
        return L("Browser", "ブラウザ");
    }
    if (m == DESK_NOTES) {
        return L("Notes", "メモ");
    }
    if (m == DESK_CALENDAR) {
        return L("Calendar", "カレンダー");
    }
    if (m == DESK_PHOTOS) {
        return L("Photos", "写真");
    }
    if (m == DESK_MUSIC) {
        return L("Music", "音楽");
    }
    if (m == DESK_CARDS) {
        return L("Cards", "カード");
    }
    if (m == DESK_SHOOTER) {
        return L("Shooter", "シューター");
    }
    if (m == DESK_SHEET) {
        return L("Sheet", "表計算");
    }
    if (m == DESK_VIDEO) {
        return L("Video", "動画");
    }
    if (m == DESK_INSTALLER) {
        return L("Installer", "インストーラ");
    }
    if (m == DESK_MAIL) {
        return L("Mail", "メール");
    }
    if (m == DESK_DOCS) {
        return L("Docs", "文書");
    }
    if (m == DESK_SLIDES) {
        return L("Slides", "スライド");
    }
    if (m == DESK_CONTACTS) {
        return L("Contacts", "連絡先");
    }
    if (m == DESK_EDITOR) {
        return L("Editor", "エディタ");
    }
    if (m == DESK_PAINT) {
        return L("Paint", "ペイント");
    }
    if (m == DESK_STORE) {
        return L("Store", "ストア");
    }
    if (m == DESK_NOTIF) {
        return L("Notifications", "通知履歴");
    }
    return L("Terminal", "ターミナル");
}

static void draw_console_text(void) {
    if (!console_ok) {
        return;
    }
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    /* スクロールバック表示中は、履歴と現グリッドを
     * つないだ仮想行から描く。 */
    int hist_avail = console_hist_count < CONSOLE_HIST_LINES
                         ? console_hist_count
                         : CONSOLE_HIST_LINES;
    if (console_view_offset > hist_avail) {
        console_view_offset = hist_avail;
    }
    for (int row = 0; row < console_rows; row++) {
        for (int col = 0; col < console_cols; col++) {
            uint32_t cp;
            if (console_view_offset > 0 &&
                console_hist != NULL &&
                console_hist_cols == console_cols) {
                /* 結合行: 履歴 (古い順) + 現グリッド。 */
                int ci = hist_avail - console_view_offset + row;
                if (ci < hist_avail) {
                    int slot = (console_hist_count - hist_avail +
                                ci) %
                               CONSOLE_HIST_LINES;
                    if (slot < 0) {
                        slot += CONSOLE_HIST_LINES;
                    }
                    cp = console_hist[slot * console_cols + col];
                } else {
                    cp = console_grid[(ci - hist_avail) *
                                          console_cols +
                                      col];
                }
            } else {
                cp = console_grid[row * console_cols + col];
            }
            if (cp == CONSOLE_WIDE_TAIL) {
                continue;
            }
            if (cp == 0) {
                cp = ' ';
            }
            uint64_t px = x0 + (uint64_t)(col * CELL_W);
            uint64_t py = y0 + (uint64_t)(row * CELL_H);
            int cursor = (row == console_row && col == console_col &&
                          desk_mode == DESK_TERMINAL &&
                          console_view_offset == 0);
            if (cp < 0x80) {
                /* Cursor: inverted block. */
                fb_draw_char(px, py, (char)cp, cursor ? bg : fg,
                             cursor ? fg : bg, TEXT_SCALE);
            } else {
                /* A wide glyph is 16x16: it covers this cell
                 * and the next one (24x16 together). */
                fb_draw_wide_char(px + 4, py, cp, cursor ? bg : fg,
                                  cursor ? fg : bg, 1);
                if (cursor && col + 1 < console_cols) {
                    fb_fill_rect(px + CELL_W, py, CELL_W, CELL_H, fg);
                }
            }
        }
    }
    if (console_view_offset > 0 && desk_mode == DESK_TERMINAL) {
        draw_text_small(x0,
                        y0 + (uint64_t)((console_rows - 1) * CELL_H),
                        L("-- scrollback (PgDn = back) --",
                          "-- スクロールバック中 (PgDn = 戻る) --"),
                        col_accent(), bg);
    }
}

/* Files のディレクトリ移動: files_dir が現在の
 * ディレクトリ ("" = ルート)。行は files_rows に
 * 並ぶ: >=0 は VFS インデックス、-1 は ".."、
 * -2-j はサブディレクトリ (files_dirnames[j])。 */
static void serial_write_num(int v);
static void switch_mode(desktop_mode m);

static char files_dir[32];
static int files_rows[168];
static char files_dirnames[32][28];
static int files_row_count;
/* 一覧の並び順: 0 = 名前、1 = 日付 (新しい順)、
 * 2 = サイズ (大きい順)。'o' キーで切り替える。 */
static int files_sort_mode;

static int files_row_before(int x, int y) {
    const char *nx = vfs_file_name(x);
    const char *ny = vfs_file_name(y);
    if (files_sort_mode == 1) {
        uint32_t mx = vfs_file_mtime(x);
        uint32_t my = vfs_file_mtime(y);
        if (mx != my) {
            return mx > my;
        }
    } else if (files_sort_mode == 2) {
        size_t sx = 0;
        size_t sy = 0;
        (void)vfs_read_ptr(nx, &sx);
        (void)vfs_read_ptr(ny, &sy);
        if (sx != sy) {
            return sx > sy;
        }
    }
    return strcmp(nx != NULL ? nx : "", ny != NULL ? ny : "") < 0;
}

static void files_build_rows(void) {
    files_row_count = 0;
    int ndirs = 0;
    if (files_dir[0] != '\0') {
        files_rows[files_row_count++] = -1;
    }
    size_t pl = 0;
    while (files_dir[pl] != '\0') {
        pl++;
    }
    int count = vfs_file_count();
    for (int i = 0; i < count; i++) {
        const char *n = vfs_file_name(i);
        size_t k = 0;
        if (pl > 0) {
            while (k < pl && n[k] == files_dir[k]) {
                k++;
            }
            if (k != pl || n[pl] != '/') {
                continue;
            }
        }
        const char *rest = n + (pl > 0 ? pl + 1 : 0);
        if (rest[0] == '\0') {
            continue;
        }
        const char *slash = rest;
        while (*slash != '\0' && *slash != '/') {
            slash++;
        }
        if (*slash == '/') {
            size_t dl = (size_t)(slash - rest);
            if (dl >= 28) {
                continue;
            }
            char dn[28];
            for (size_t j = 0; j < dl; j++) {
                dn[j] = rest[j];
            }
            dn[dl] = '\0';
            int seen = 0;
            for (int j = 0; j < ndirs; j++) {
                if (strcmp(files_dirnames[j], dn) == 0) {
                    seen = 1;
                }
            }
            if (!seen && ndirs < 32 &&
                files_row_count < 168) {
                size_t q = 0;
                while (dn[q] != '\0') {
                    files_dirnames[ndirs][q] = dn[q];
                    q++;
                }
                files_dirnames[ndirs][q] = '\0';
                files_rows[files_row_count++] = -2 - ndirs;
                ndirs++;
            }
            continue;
        }
        if (strcmp(rest, ".dir") == 0) {
            continue;
        }
        if (files_row_count < 168) {
            files_rows[files_row_count++] = i;
        }
    }
    /* ファイル行 (>=0) だけを並び替える。".." と
     * フォルダ行は先頭のままにする。 */
    {
        int pos[168];
        int vals[168];
        int nf = 0;
        for (int a = 0; a < files_row_count; a++) {
            if (files_rows[a] >= 0) {
                pos[nf] = a;
                vals[nf] = files_rows[a];
                nf++;
            }
        }
        for (int a = 1; a < nf; a++) {
            int v = vals[a];
            int b = a - 1;
            while (b >= 0 && files_row_before(v, vals[b])) {
                vals[b + 1] = vals[b];
                b--;
            }
            vals[b + 1] = v;
        }
        for (int a = 0; a < nf; a++) {
            files_rows[pos[a]] = vals[a];
        }
    }
    if (files_selected >= files_row_count) {
        files_selected = files_row_count > 0 ? files_row_count - 1 : 0;
    }
    if (files_selected < 0) {
        files_selected = 0;
    }
}

/* ファイルをゴミ箱 (trash/) へ移す。成功 0。 */
static int files_trash_file(const char *name) {
    const char *base = name;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    if (!vfs_exists("trash/.dir")) {
        (void)vfs_write("trash/.dir", "", 0);
    }
    char dst[40];
    for (int attempt = 0; attempt < 100; attempt++) {
        size_t p = 0;
        const char *pre = "trash/";
        while (pre[p] != '\0' && p + 1 < sizeof(dst)) {
            dst[p] = pre[p];
            p++;
        }
        size_t q = 0;
        while (base[q] != '\0' && p + 1 < sizeof(dst)) {
            dst[p++] = base[q++];
        }
        if (attempt > 0 && p + 2 < sizeof(dst)) {
            dst[p++] = '-';
            dst[p++] = (char)('0' + attempt);
        }
        dst[p] = '\0';
        if (!vfs_exists(dst)) {
            size_t len = 0;
            const char *content = vfs_read_ptr(name, &len);
            if (content == NULL) {
                return -1;
            }
            if (vfs_write(dst, content, len) != 0) {
                return -1;
            }
            return vfs_delete(name);
        }
    }
    return -1;
}

/* Files の名前入力プロンプトと確認ダイアログの状態。
 * prompt: 0=なし, 1=名前変更, 2=新規ファイル,
 * 3=新規フォルダ, 4=コピー。confirm: 0=なし,
 * 1=ゴミ箱を空にする, 2=完全に削除。 */
static int files_prompt;
static char files_prompt_buf[28];
static int files_prompt_len;
static int files_confirm;
static char files_confirm_name[32];

static int files_in_trash(void) {
    return strcmp(files_dir, "trash") == 0;
}

/* files_dir の下のパスを組み立てる。 */
static void files_join_path(char *out, size_t cap, const char *name) {
    size_t p = 0;
    size_t q = 0;
    while (files_dir[q] != '\0' && p + 1 < cap) {
        out[p++] = files_dir[q++];
    }
    if (p > 0 && p + 1 < cap) {
        out[p++] = '/';
    }
    q = 0;
    while (name[q] != '\0' && p + 1 < cap) {
        out[p++] = name[q++];
    }
    out[p] = '\0';
}

static int files_name_ok(const char *name) {
    if (name[0] == '\0') {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        strcmp(name, ".dir") == 0) {
        return 0;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/') {
            return 0;
        }
    }
    return 1;
}

static void files_prompt_start(int mode, const char *initial) {
    files_prompt = mode;
    files_prompt_len = 0;
    if (initial != NULL) {
        while (initial[files_prompt_len] != '\0' &&
               files_prompt_len + 1 < (int)sizeof(files_prompt_buf)) {
            files_prompt_buf[files_prompt_len] =
                initial[files_prompt_len];
            files_prompt_len++;
        }
    }
    files_prompt_buf[files_prompt_len] = '\0';
    desktop_redraw();
}

/* プロンプト確定: 対象ファイルは選択行から取る。 */
static void files_prompt_commit(void) {
    int mode = files_prompt;
    files_prompt = 0;
    char newname[28];
    size_t np = 0;
    while (files_prompt_buf[np] != '\0' && np + 1 < sizeof(newname)) {
        newname[np] = files_prompt_buf[np];
        np++;
    }
    newname[np] = '\0';
    if (!files_name_ok(newname)) {
        desktop_notify(L("Invalid name", "使えない名前です"));
        desktop_redraw();
        return;
    }
    char dst[40];
    files_join_path(dst, sizeof(dst), newname);
    if (mode == 3) {
        char marker[48];
        size_t mp = 0;
        while (dst[mp] != '\0' && mp + 1 < sizeof(marker)) {
            marker[mp] = dst[mp];
            mp++;
        }
        const char *sfx = "/.dir";
        size_t sq = 0;
        while (sfx[sq] != '\0' && mp + 1 < sizeof(marker)) {
            marker[mp++] = sfx[sq++];
        }
        marker[mp] = '\0';
        if (vfs_exists(marker) || vfs_exists(dst)) {
            desktop_notify(L("Already exists", "既にあります"));
        } else if (vfs_write(marker, "", 0) == 0) {
            serial_write("files: mkdir ");
            serial_write_line(dst);
            desktop_notify(L("Folder created", "フォルダを作成しました"));
            persist_maybe_sync();
        } else {
            desktop_notify(L("Could not create it", "作成できませんでした"));
        }
        desktop_redraw();
        return;
    }
    if (mode == 2) {
        if (vfs_exists(dst)) {
            desktop_notify(L("Already exists", "既にあります"));
        } else if (vfs_write(dst, "", 0) == 0) {
            serial_write("files: created ");
            serial_write_line(dst);
            desktop_notify(L("File created", "ファイルを作成しました"));
            persist_maybe_sync();
        } else {
            desktop_notify(L("Could not create it", "作成できませんでした"));
        }
        desktop_redraw();
        return;
    }
    /* 1=名前変更, 4=コピー: 選択行のファイルが対象。 */
    files_build_rows();
    if (files_selected < 0 || files_selected >= files_row_count ||
        files_rows[files_selected] < 0) {
        desktop_redraw();
        return;
    }
    const char *src = vfs_file_name(files_rows[files_selected]);
    char srcbuf[32];
    size_t sp = 0;
    while (src[sp] != '\0' && sp + 1 < sizeof(srcbuf)) {
        srcbuf[sp] = src[sp];
        sp++;
    }
    srcbuf[sp] = '\0';
    if (vfs_exists(dst)) {
        desktop_notify(L("Already exists", "既にあります"));
        desktop_redraw();
        return;
    }
    size_t len = 0;
    const char *content = vfs_read_ptr(srcbuf, &len);
    if (content == NULL || vfs_write(dst, content, len) != 0) {
        desktop_notify(L("Failed", "失敗しました"));
        desktop_redraw();
        return;
    }
    if (mode == 1) {
        (void)vfs_delete(srcbuf);
        serial_write("files: renamed ");
        serial_write(srcbuf);
        serial_write(" -> ");
        serial_write_line(dst);
        desktop_notify(L("Renamed", "名前を変更しました"));
    } else {
        serial_write("files: copied ");
        serial_write(srcbuf);
        serial_write(" -> ");
        serial_write_line(dst);
        desktop_notify(L("Copied", "コピーしました"));
    }
    persist_maybe_sync();
    desktop_redraw();
}

/* ゴミ箱から元の場所 (ルート) へ戻す。シェルの
 * trash restore と同じ VFS 操作。 */
static int files_trash_restore(const char *name) {
    char src[40];
    size_t p = 0;
    const char *pre = "trash/";
    while (pre[p] != '\0' && p + 1 < sizeof(src)) {
        src[p] = pre[p];
        p++;
    }
    size_t q = 0;
    while (name[q] != '\0' && p + 1 < sizeof(src)) {
        src[p++] = name[q++];
    }
    src[p] = '\0';
    if (!vfs_exists(src) || vfs_exists(name)) {
        return -1;
    }
    size_t len = 0;
    const char *content = vfs_read_ptr(src, &len);
    if (content == NULL || vfs_write(name, content, len) != 0) {
        return -1;
    }
    (void)vfs_delete(src);
    serial_write("files: restored ");
    serial_write_line(name);
    persist_maybe_sync();
    return 0;
}

static void files_trash_empty(void) {
    int count = vfs_file_count();
    char names[PRISM_VFS_MAX_FILES][32];
    int m = 0;
    for (int i = 0; i < count; i++) {
        const char *n = vfs_file_name(i);
        if (n[0] == 't' && n[1] == 'r' && n[2] == 'a' &&
            n[3] == 's' && n[4] == 'h' && n[5] == '/') {
            size_t q = 0;
            while (n[q] != '\0' && q + 1 < sizeof(names[m])) {
                names[m][q] = n[q];
                q++;
            }
            names[m][q] = '\0';
            m++;
        }
    }
    for (int i = 0; i < m; i++) {
        (void)vfs_delete(names[i]);
    }
    serial_write_line("files: trash emptied");
    persist_maybe_sync();
}

/* 選択中の行を開く (ディレクトリなら移動、
 * ファイルなら表示/実行)。キーとマウスで共用。 */
static void files_activate_row(void) {
    files_build_rows();
    if (files_selected < 0 || files_selected >= files_row_count) {
        return;
    }
    int row = files_rows[files_selected];
    if (row == -1) {
        /* 親へ */
        size_t n = 0;
        while (files_dir[n] != '\0') {
            n++;
        }
        while (n > 0 && files_dir[n - 1] != '/') {
            n--;
        }
        if (n > 0) {
            files_dir[n - 1] = '\0';
        } else {
            files_dir[0] = '\0';
        }
        files_selected = 0;
        desktop_refresh_console();
        return;
    }
    if (row <= -2) {
        const char *dn = files_dirnames[-2 - row];
        size_t p = 0;
        while (files_dir[p] != '\0') {
            p++;
        }
        if (p > 0 && p + 1 < sizeof(files_dir)) {
            files_dir[p++] = '/';
        }
        size_t q = 0;
        while (dn[q] != '\0' && p + 1 < sizeof(files_dir)) {
            files_dir[p++] = dn[q++];
        }
        files_dir[p] = '\0';
        files_selected = 0;
        serial_write("files: cd ");
        serial_write_line(files_dir);
        desktop_refresh_console();
        return;
    }
    const char *name = vfs_file_name(row);
    if (name != NULL && compat_is_app(name)) {
        char tmp[32];
        size_t p = 0;
        while (name[p] != '\0' && p + 1 < sizeof(tmp)) {
            tmp[p] = name[p];
            p++;
        }
        tmp[p] = '\0';
        int code = -1;
        if (compat_run_file(tmp, NULL, 0, &code) == 0) {
            serial_write("compat: ");
            serial_write(tmp);
            serial_write(" exited with code ");
            serial_write_num(code);
            serial_write_line(".");
        }
        switch_mode(DESK_TERMINAL);
        desktop_refresh_console();
        return;
    }
    files_viewing = row;
    desktop_refresh_console();
}

static void draw_files(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    int count = vfs_file_count();
    if (files_viewing >= 0 && files_viewing < count) {
        const char *name = vfs_file_name(files_viewing);
        size_t len = 0;
        const char *content = vfs_read_ptr(name, &len);
        /* PDF はテキストを抽出して表示し、日本語の
         * 旧文字コード (SJIS/EUC/2022) は UTF-8 へ
         * 変換して表示する。 */
        static char view_buf[16384];
        if (content != NULL && len >= 5 &&
            name[0] != '\0') {
            size_t nl = 0;
            while (name[nl] != '\0') {
                nl++;
            }
            if (nl >= 4 && strcmp(name + nl - 4, ".pdf") == 0) {
                int n = pdf_extract_text((const uint8_t *)content,
                                         len, view_buf,
                                         sizeof(view_buf));
                if (n > 0) {
                    content = view_buf;
                    len = (size_t)n;
                }
            } else {
                charset_kind k = charset_detect(
                    (const uint8_t *)content, len);
                if (k == CHARSET_SJIS || k == CHARSET_EUCJP ||
                    k == CHARSET_ISO2022JP) {
                    size_t w = charset_to_utf8(
                        k, (const uint8_t *)content, len,
                        view_buf, sizeof(view_buf) - 1);
                    view_buf[w] = '\0';
                    content = view_buf;
                    len = w;
                }
            }
        }
        char header[48];
        size_t p = 0;
        const char *h = L("Viewing: ", "表示中: ");
        while (*h != '\0' && p + 1 < sizeof(header)) {
            header[p++] = *h++;
        }
        while (*name != '\0' && p + 1 < sizeof(header)) {
            header[p++] = *name++;
        }
        header[p] = '\0';
        draw_text_small(x0, y0, header, acc, bg);
        int row = 2;
        size_t pos = 0;
        while (pos < len && row < text_rows()) {
            char line[128];
            size_t n = 0;
            while (pos < len && content[pos] != '\n' && n + 1 < sizeof(line)) {
                line[n++] = content[pos++];
            }
            if (pos < len) {
                pos++;
            }
            line[n] = '\0';
            fb_draw_text(x0, y0 + (uint64_t)(row * CELL_H), line, fg, bg, 1);
            row++;
        }
        if (row >= text_rows() && pos < len) {
            fb_draw_text(x0, y0 + (uint64_t)(row * CELL_H), "...",
                         col_dim(), bg, 1);
        }
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                        L("Esc = back to list", "Esc = 一覧へ戻る"), col_dim(), bg);
        return;
    }
    files_build_rows();
    {
        char hdr[80];
        size_t hp = 0;
        const char *h1 = L("Files - /", "ファイル - /");
        while (*h1 != '\0' && hp + 1 < sizeof(hdr)) {
            hdr[hp++] = *h1++;
        }
        size_t hq = 0;
        while (files_dir[hq] != '\0' && hp + 1 < sizeof(hdr)) {
            hdr[hp++] = files_dir[hq++];
        }
        const char *h2 = files_in_trash()
            ? L(" - r restores, x deletes, e empties",
                " - rで復元、xで完全削除、eで空に")
            : L(" - Enter opens, d trashes, n/m/r/c edit",
                " - Enterで開く、dでゴミ箱、n/m/r/cで編集");
        while (*h2 != '\0' && hp + 1 < sizeof(hdr)) {
            hdr[hp++] = *h2++;
        }
        const char *h3 = files_sort_mode == 1
            ? L(" [o: by date]", " [o: 日付順]")
            : files_sort_mode == 2
            ? L(" [o: by size]", " [o: サイズ順]")
            : L(" [o: by name]", " [o: 名前順]");
        while (*h3 != '\0' && hp + 1 < sizeof(hdr)) {
            hdr[hp++] = *h3++;
        }
        hdr[hp] = '\0';
        draw_text_small(x0, y0, hdr, col_dim(), bg);
    }
    for (int i = 0; i < files_row_count && i + 2 < text_rows(); i++) {
        int rowv = files_rows[i];
        const char *name;
        size_t len = 0;
        char dirbuf[36];
        if (rowv == -1) {
            name = "..";
        } else if (rowv <= -2) {
            size_t dp = 0;
            const char *dn = files_dirnames[-2 - rowv];
            while (*dn != '\0' && dp + 1 < sizeof(dirbuf)) {
                dirbuf[dp++] = *dn++;
            }
            if (dp + 1 < sizeof(dirbuf)) {
                dirbuf[dp++] = '/';
            }
            dirbuf[dp] = '\0';
            name = dirbuf;
        } else {
            name = vfs_file_name(rowv);
            (void)vfs_read_ptr(name, &len);
        }
        char line[80];
        size_t p = 0;
        while (*name != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *name++;
        }
        /* L("  <len> bytes", "  <長さ> バイト") after the name. */
        line[p++] = ' ';
        line[p++] = ' ';
        line[p++] = '(';
        char num[21];
        int ni = 0;
        size_t v = len;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[21];
            int ri = 0;
            while (v > 0 && ri < 20) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int k = 0; num[k] != '\0' && p + 1 < sizeof(line); k++) {
            line[p++] = num[k];
        }
        const char *tail = L(" bytes)", " バイト)");
        while (*tail != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *tail++;
        }
        /* 更新日 (記録があるファイルのみ)。 */
        if (rowv >= 0) {
            uint32_t mt = vfs_file_mtime(rowv);
            if (mt > 0) {
                prism_rtc_time rt;
                rtc_from_unix(mt, desk_settings.tz_offset, &rt);
                int yr = 2000 + rt.year;
                if (p + 13 < sizeof(line)) {
                    line[p++] = ' ';
                    line[p++] = ' ';
                    line[p++] = (char)('0' + (yr / 1000) % 10);
                    line[p++] = (char)('0' + (yr / 100) % 10);
                    line[p++] = (char)('0' + (yr / 10) % 10);
                    line[p++] = (char)('0' + yr % 10);
                    line[p++] = '/';
                    line[p++] = (char)('0' + (rt.month / 10) % 10);
                    line[p++] = (char)('0' + rt.month % 10);
                    line[p++] = '/';
                    line[p++] = (char)('0' + (rt.day / 10) % 10);
                    line[p++] = (char)('0' + rt.day % 10);
                }
            }
        }
        line[p] = '\0';
        uint64_t py = y0 + (uint64_t)((i + 2) * CELL_H);
        if (i == files_selected) {
            fb_fill_rect(x0, py, win_w() - 16, CELL_H, acc);
            fb_draw_text(x0 + 4, py, line, rgb(0xFFFFFF), acc, 1);
        } else {
            fb_draw_text(x0 + 4, py, line, fg, bg, 1);
        }
    }
    if (files_row_count == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), L("(no files)", "(ファイルなし)"),
                        col_dim(), bg);
    }
    if (files_prompt) {
        const char *pl = files_prompt == 1
            ? L("Rename to: ", "新しい名前: ")
            : files_prompt == 2
            ? L("New file: ", "新規ファイル: ")
            : files_prompt == 3
            ? L("New folder: ", "新規フォルダ: ")
            : L("Copy to: ", "コピー先の名前: ");
        char pline[64];
        size_t pp = 0;
        while (*pl != '\0' && pp + 1 < sizeof(pline)) {
            pline[pp++] = *pl++;
        }
        size_t pq = 0;
        while (files_prompt_buf[pq] != '\0' && pp + 1 < sizeof(pline)) {
            pline[pp++] = files_prompt_buf[pq++];
        }
        if (pp + 1 < sizeof(pline)) {
            pline[pp++] = '_';
        }
        pline[pp] = '\0';
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                        pline, acc, bg);
    }
    if (files_confirm) {
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                        files_confirm == 1
                            ? L("Empty the trash? y = yes, n = no",
                                "ゴミ箱を空にしますか？ y = はい, n = いいえ")
                            : L("Delete permanently? y = yes, n = no",
                                "完全に削除しますか？ y = はい, n = いいえ"),
                        acc, bg);
    }
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("Esc = back to Terminal", "Esc = ターミナルへ戻る"), col_dim(), bg);
}

static void draw_settings_binds(void);
static void draw_settings_accounts(void);
static void draw_settings_security(void);
static void draw_settings_updates(void);
static void draw_settings_clock(void);
static void draw_settings_admin(void);
static void adm_refresh(void);
static void draw_settings_network(void);
static void draw_notiflog(void);
static void draw_pin_dialog(void);

static void draw_settings(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    char hex[8];
    char line[96];
    if (settings_page == 1) {
        draw_settings_binds();
        return;
    }
    if (settings_page == 2) {
        draw_settings_accounts();
        if (pin_dialog_open) {
            draw_pin_dialog();
        }
        return;
    }
    if (settings_page == 3) {
        draw_settings_security();
        if (pin_dialog_open) {
            draw_pin_dialog();
        }
        return;
    }
    if (settings_page == 4) {
        draw_settings_updates();
        if (pin_dialog_open) {
            draw_pin_dialog();
        }
        return;
    }
    if (settings_page == 5) {
        draw_settings_clock();
        if (pin_dialog_open) {
            draw_pin_dialog();
        }
        return;
    }
    if (settings_page == 6) {
        draw_settings_network();
        if (pin_dialog_open) {
            draw_pin_dialog();
        }
        return;
    }
    if (settings_page == 7) {
        draw_settings_admin();
        if (pin_dialog_open) {
            draw_pin_dialog();
        }
        return;
    }
    draw_text_small(x0, y0, L("Settings", "設定"), acc, bg);
    /* Accent choices. */
    for (int i = 0; i < PRISM_ACCENT_COUNT; i++) {
        settings_accent_hex(PRISM_ACCENTS[i], hex);
        size_t p = 0;
        line[p++] = (char)('1' + i);
        line[p++] = ' ';
        line[p++] = ' ';
        const char *names[7] = {L("Cyan", "シアン"), L("Violet", "バイオレット"), L("Pink", "ピンク"), L("Green", "グリーン"), L("Orange", "オレンジ"), L("Red", "レッド"), L("Teal", "ティール")};
        const char *nm = names[i];
        while (*nm != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *nm++;
        }
        line[p++] = ' ';
        for (int k = 0; hex[k] != '\0' && p + 1 < sizeof(line); k++) {
            line[p++] = hex[k];
        }
        if (desk_settings.accent_rgb == PRISM_ACCENTS[i]) {
            const char *mark = "  *";
            while (*mark != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *mark++;
            }
        }
        line[p] = '\0';
        /* 7 色を 2 列で出す (後続の項目と行が重ならない
         * ように)。 */
        fb_draw_text(x0 + (uint64_t)(i % 2) * (win_w() / 2),
                     y0 + (uint64_t)((2 + i / 2) * CELL_H), line,
                     fg, bg, 1);
    }
    size_t p = 0;
    const char *t0 = L("k  Dock: ", "k  Dock表示: ");
    while (*t0 != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *t0++;
    }
    const char *dk = desk_settings.dock_show ? "shown" : "hidden";
    while (*dk != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *dk++;
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(6 * CELL_H), line, fg, bg, 1);
    p = 0;
    const char *t1 = L("t  Taskbar: ", "t  タスクバー: ");
    while (*t1 != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *t1++;
    }
    const char *tp = desk_settings.taskbar_side == 1
        ? "left"
        : desk_settings.taskbar_side == 2
        ? "right"
        : desk_settings.taskbar_top ? "top" : "bottom";
    while (*tp != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *tp++;
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(7 * CELL_H), line, fg, bg, 1);
    p = 0;
    const char *t2 = L("m  Mode: ", "m  モード: ");
    while (*t2 != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *t2++;
    }
    const char *md = desk_settings.dark_mode ? "dark" : "light";
    while (*md != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *md++;
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(8 * CELL_H), line, fg, bg, 1);
    p = 0;
    const char *t3 = L("w  Wallpaper: ", "w  壁紙: ");
    while (*t3 != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *t3++;
    }
    const char *wp = desk_settings.wallpaper_image == 2
                         ? "custom: "
                         : (desk_settings.wallpaper_image ? "image"
                                                          : "drawn");
    while (*wp != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *wp++;
    }
    if (desk_settings.wallpaper_image == 2) {
        const char *fn = desk_settings.wallpaper_file;
        while (*fn != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *fn++;
        }
        if (!wp_custom_ok) {
            const char *miss = L(" (unreadable)", " (読めません)");
            while (*miss != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *miss++;
            }
        }
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(9 * CELL_H), line, fg, bg, 1);
    draw_text_small(x0, y0 + (uint64_t)(10 * CELL_H),
                    L("Changes are saved automatically",
                      "変更は自動で保存されます"), fg, bg);
    if (settings_save_failed) {
        draw_text_small(x0, y0 + (uint64_t)(11 * CELL_H),
                        L("NOT saved (save failed)",
                          "保存できませんでした (未保存)"),
                        rgb(0xC0392B), bg);
    } else if (settings_saved) {
        draw_text_small(x0, y0 + (uint64_t)(11 * CELL_H),
                        L("Saved to settings.txt", "settings.txtに保存しました"), acc, bg);
    }
    /* セキュリティの概要 (読み取り専用。変更は `security` コマンド)。 */
    {
        char sec[96];
        size_t q = 0;
        const char *h1 = L("Security: media=", "セキュリティ: メディア=");
        while (*h1 != '\0' && q + 1 < sizeof(sec)) {
            sec[q++] = *h1++;
        }
        const char *mm = desk_settings.usb_media ? "block" : "allow";
        while (*mm != '\0' && q + 1 < sizeof(sec)) {
            sec[q++] = *mm++;
        }
        const char *h2 = L("  PIN>= ", "  PIN最小 ");
        while (*h2 != '\0' && q + 1 < sizeof(sec)) {
            sec[q++] = *h2++;
        }
        {
            int v = desk_settings.pin_min_len;
            char tmp[4];
            int d = 0;
            if (v == 0) {
                tmp[d++] = '0';
            } else {
                while (v > 0) {
                    tmp[d++] = (char)('0' + (v % 10));
                    v /= 10;
                }
            }
            while (d > 0 && q + 1 < sizeof(sec)) {
                sec[q++] = tmp[--d];
            }
        }
        if (desk_settings.pin_require_digit) {
            const char *h3 = L(" +digit", " +数字");
            while (*h3 != '\0' && q + 1 < sizeof(sec)) {
                sec[q++] = *h3++;
            }
        }
        sec[q] = '\0';
        draw_text_small(x0, y0 + (uint64_t)(12 * CELL_H), sec, fg, bg);
        int denied = 0;
        for (int i = 0; i < SEC_APP_COUNT; i++) {
            if (!desk_allowed[i]) {
                denied++;
            }
        }
        char al[48];
        q = 0;
        const char *h4 = L("Allowlist: ", "許可リスト: ");
        while (*h4 != '\0' && q + 1 < sizeof(al)) {
            al[q++] = *h4++;
        }
        if (denied == 0) {
            const char *h5 = L("all apps", "全アプリ");
            while (*h5 != '\0' && q + 1 < sizeof(al)) {
                al[q++] = *h5++;
            }
        } else {
            int v = denied;
            char tmp[4];
            int d = 0;
            if (v == 0) {
                tmp[d++] = '0';
            } else {
                while (v > 0) {
                    tmp[d++] = (char)('0' + (v % 10));
                    v /= 10;
                }
            }
            while (d > 0 && q + 1 < sizeof(al)) {
                al[q++] = tmp[--d];
            }
            const char *h6 = L(" denied", " 拒否");
            while (*h6 != '\0' && q + 1 < sizeof(al)) {
                al[q++] = *h6++;
            }
        }
        al[q] = '\0';
        draw_text_small(x0, y0 + (uint64_t)(13 * CELL_H), al, fg, bg);
    }
    {
        char dl[96];
        size_t q = 0;
        const char *a1 = L("p  Dock position: ",
                             "p  Dockの位置: ");
        while (*a1 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a1++;
        }
        const char *pv = desk_settings.dock_pos ? "left" : "bottom";
        while (*pv != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *pv++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(14 * CELL_H), dl, fg, bg, 1);
        q = 0;
        const char *a2 = L("z  Dock size: ",
                             "z  Dockの大きさ: ");
        while (*a2 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a2++;
        }
        const char *sv = desk_settings.dock_size ? "large" : "normal";
        while (*sv != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *sv++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(15 * CELL_H), dl, fg, bg, 1);
        q = 0;
        const char *a3 = L("h  Dock auto-hide: ",
                             "h  Dockを自動で隠す: ");
        while (*a3 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a3++;
        }
        const char *av = desk_settings.dock_autohide ? "on" : "off";
        while (*av != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *av++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(16 * CELL_H), dl, fg, bg, 1);
        q = 0;
        const char *a4 = L("g  Language: ", "g  言語: ");
        while (*a4 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a4++;
        }
        const char *lv = desk_settings.language ? "Japanese" : "English";
        while (*lv != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *lv++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(17 * CELL_H), dl, fg, bg, 1);
        q = 0;
        const char *a5 = L("j  Keyboard: ", "j  キーボード: ");
        while (*a5 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a5++;
        }
        const char *kv = desk_settings.kbd_layout ? "JIS" : "US";
        while (*kv != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *kv++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(18 * CELL_H), dl, fg, bg, 1);
        q = 0;
        const char *a6 = L("x  High contrast: ",
                             "x  ハイコントラスト: ");
        while (*a6 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a6++;
        }
        const char *cv = desk_settings.high_contrast ? "on" : "off";
        while (*cv != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *cv++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(19 * CELL_H), dl, fg, bg, 1);
        q = 0;
        const char *a7 = L("b  Text size: ", "b  文字サイズ: ");
        while (*a7 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a7++;
        }
        const char *tv = desk_settings.text_scale >= 3
                             ? L("extra large", "特大")
                             : desk_settings.text_scale >= 2
                             ? L("large", "大")
                             : L("standard", "標準");
        while (*tv != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *tv++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(20 * CELL_H), dl, fg, bg, 1);
        q = 0;
        const char *a8 = L("o  On-screen keyboard: ",
                             "o  画面キーボード: ");
        while (*a8 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a8++;
        }
        const char *ov = desk_settings.osk ? "on" : "off";
        while (*ov != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *ov++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(21 * CELL_H), dl, fg, bg, 1);
        q = 0;
        const char *a9 = L("n  Narrator captions: ",
                             "n  ナレーター字幕: ");
        while (*a9 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *a9++;
        }
        const char *nv = desk_settings.narrator ? "on" : "off";
        while (*nv != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *nv++;
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(22 * CELL_H), dl, fg, bg, 1);
    }
    {
        char rl[64];
        size_t q = 0;
        const char *r1 = L("r  Resolution: ", "r  解像度: ");
        while (*r1 != '\0' && q + 1 < sizeof(rl)) {
            rl[q++] = *r1++;
        }
        char num[16];
        int ni = 0;
        uint32_t v = fb_width();
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[12];
            int ri = 0;
            while (v > 0 && ri < 10) {
                rev[ri++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int k = 0; num[k] != '\0' && q + 1 < sizeof(rl); k++) {
            rl[q++] = num[k];
        }
        rl[q++] = 'x';
        ni = 0;
        v = fb_height();
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[12];
            int ri = 0;
            while (v > 0 && ri < 10) {
                rev[ri++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int k = 0; num[k] != '\0' && q + 1 < sizeof(rl); k++) {
            rl[q++] = num[k];
        }
        rl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(23 * CELL_H), rl, fg, bg, 1);
    }
    {
        /* 入力デバイス (項目26)。 */
        char il[96];
        size_t q = 0;
        const char *i1 = L("e  Mouse speed: ", "e  マウス速度: ");
        while (*i1 != '\0' && q + 1 < sizeof(il)) {
            il[q++] = *i1++;
        }
        char num[16];
        int ni = 0;
        int v = desk_settings.mouse_speed;
        char rev[8];
        int ri = 0;
        if (v == 0) {
            rev[ri++] = '0';
        }
        while (v > 0 && ri < 6) {
            rev[ri++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
        num[ni++] = '%';
        num[ni] = '\0';
        for (int k = 0; num[k] != '\0' && q + 1 < sizeof(il); k++) {
            il[q++] = num[k];
        }
        il[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(24 * CELL_H), il, fg, bg, 1);
        const char *kr = desk_settings.key_repeat == 0
            ? L("i  Key repeat: slow", "i  キーリピート: 遅い")
            : desk_settings.key_repeat == 2
            ? L("i  Key repeat: fast", "i  キーリピート: 速い")
            : L("i  Key repeat: normal",
                "i  キーリピート: 標準");
        fb_draw_text(x0, y0 + (uint64_t)(25 * CELL_H), kr, fg, bg, 1);
        char dl[96];
        q = 0;
        const char *d1 = L("u  Double-click: ",
                           "u  ダブルクリック間隔: ");
        while (*d1 != '\0' && q + 1 < sizeof(dl)) {
            dl[q++] = *d1++;
        }
        ni = 0;
        v = desk_settings.dblclick_ms;
        ri = 0;
        if (v == 0) {
            rev[ri++] = '0';
        }
        while (v > 0 && ri < 6) {
            rev[ri++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
        num[ni++] = 'm';
        num[ni++] = 's';
        num[ni] = '\0';
        for (int k = 0; num[k] != '\0' && q + 1 < sizeof(dl); k++) {
            dl[q++] = num[k];
        }
        dl[q] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(26 * CELL_H), dl, fg, bg, 1);
    }
    draw_text_small(x0, y0 + (uint64_t)(27 * CELL_H),
                    L("Tab = Shortcuts page",
                        "Tab = ショートカットのページ"), fg, bg);
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("Esc = back to Terminal", "Esc = ターミナルへ戻る"), col_dim(), bg);
}

/* 設定画面のショートカットページ: アクションと割り当てキーの
 * 一覧。選択行は反転し、取り込み中は案内を出す。 */
static void draw_settings_binds(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Settings - Shortcuts",
                                "設定 - ショートカット"), acc, bg);
    for (int i = 0; i < PRISM_BIND_COUNT; i++) {
        char line[64];
        size_t p = 0;
        line[p++] = (i == bind_selected) ? '>' : ' ';
        line[p++] = ' ';
        const char *nm = PRISM_ACTION_NAMES[i];
        size_t name_len = 0;
        while (nm[name_len] != '\0') {
            name_len++;
        }
        for (size_t k = 0; k < name_len && p + 1 < sizeof(line); k++) {
            line[p++] = nm[k];
        }
        /* キー名を 14 文字目からそろえる。 */
        while (p < 14 && p + 1 < sizeof(line)) {
            line[p++] = ' ';
        }
        char kname[8];
        if (settings_key_name(desk_settings.binds[i], kname,
                              sizeof(kname)) >= 0) {
            for (int k = 0; kname[k] != '\0' && p + 1 < sizeof(line);
                 k++) {
                line[p++] = kname[k];
            }
        }
        line[p] = '\0';
        uint64_t py = y0 + (uint64_t)((i + 2) * CELL_H);
        if (i == bind_selected) {
            fb_fill_rect(x0, py, win_w() - 16, CELL_H, acc);
            fb_draw_text(x0 + 4, py, line, rgb(0xFFFFFF), acc, 1);
        } else {
            fb_draw_text(x0 + 4, py, line, fg, bg, 1);
        }
    }
    int row = PRISM_BIND_COUNT + 3;
    if (bind_capture) {
        char line[96];
        size_t p = 0;
        const char *h = L("Press a key for ", "キーを押してください: ");
        while (*h != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *h++;
        }
        const char *nm = PRISM_ACTION_NAMES[bind_selected];
        while (*nm != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *nm++;
        }
        const char *t = L(" (Esc = cancel, arrows reserved)", " (Esc = 中止、矢印キーは予約済み)");
        while (*t != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *t++;
        }
        line[p] = '\0';
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), line, acc,
                        bg);
    } else {
        draw_text_small(
            x0, y0 + (uint64_t)(row * CELL_H),
            L("Up/Down = select  Enter = change key  0 = reset all", "上下 = 選択  Enter = キー変更  0 = 全部リセット"),
            fg, bg);
        draw_text_small(x0, y0 + (uint64_t)((row + 1) * CELL_H),
                        L("Changes save automatically  Tab = next page", "変更は自動保存  Tab = 次のページ"),
                        fg, bg);
        if (settings_save_failed) {
            draw_text_small(x0, y0 + (uint64_t)((row + 2) * CELL_H),
                            L("NOT saved (save failed)",
                              "保存できませんでした (未保存)"),
                            rgb(0xC0392B), bg);
        } else if (settings_saved) {
            draw_text_small(x0, y0 + (uint64_t)((row + 2) * CELL_H),
                            L("Saved to settings.txt", "settings.txtに保存しました"), acc, bg);
        }
    }
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("Esc = back to Terminal", "Esc = ターミナルへ戻る"), col_dim(), bg);
}


/* ================================================================
 * 設定アプリ: アカウント / セキュリティ / 更新ページ
 *
 * 裏側の仕組みはシェルと同じものを使う:
 *   アカウント: users.txt と users.c の関数群 (鍵ラップ方式)。
 *   セキュリティ: settings.txt のセキュリティ項目。
 *     保存は合流保存で、シェルと GUI のどちらで変えても
 *     相互に反映される。
 *   更新: shell.c の shell_update_check / apply_pending /
 *     rollback (マニフェスト取得と ML-DSA 署名検証は
 *     そちらだけにある)。
 * 管理操作はすべて管理者 PIN の確認ダイアログを通す
 * (シェルの admin_check と同じ照合)。
 * ================================================================ */

/* ---- 小さな文字列ヘルパー (この3ページ用) ---- */
static void buf_cat(char *buf, size_t cap, size_t *p, const char *s) {
    while (*s != '\0' && *p + 1 < cap) {
        buf[(*p)++] = *s++;
    }
    buf[*p] = '\0';
}

static void buf_cat_uint(char *buf, size_t cap, size_t *p,
                         unsigned v) {
    char tmp[12];
    int d = 0;
    if (v == 0) {
        tmp[d++] = '0';
    }
    while (v > 0) {
        tmp[d++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (d > 0 && *p + 1 < cap) {
        buf[(*p)++] = tmp[--d];
    }
    buf[*p] = '\0';
}

static void buf_cat_masked(char *buf, size_t cap, size_t *p, int n) {
    while (n > 0 && *p + 1 < cap) {
        buf[(*p)++] = '*';
        n--;
    }
    buf[*p] = '\0';
}

/* 現在のページの状態メッセージ欄へ書く。 */
static void set_page_status(const char *s) {
    char *dst = acct_status;
    size_t cap = sizeof(acct_status);
    if (settings_page == 3) {
        dst = sec_status;
        cap = sizeof(sec_status);
    } else if (settings_page == 4) {
        dst = upd_status;
        cap = sizeof(upd_status);
    }
    size_t p = 0;
    while (s[p] != '\0' && p + 1 < cap) {
        dst[p] = s[p];
        p++;
    }
    dst[p] = '\0';
}

/* セキュリティページの仮値を desk_settings の現在値に合わせる。 */
static void sec_stage_from_settings(void) {
    sec_usb = desk_settings.usb_media;
    sec_pin_min = desk_settings.pin_min_len;
    sec_pin_digit = desk_settings.pin_require_digit;
    sec_lock_attempts = desk_settings.lockout_attempts;
    sec_lock_seconds = desk_settings.lockout_seconds;
    sec_dirty = 0;
}

/* settings.txt を読み直し、セキュリティと更新の項目だけを
 * desk_settings に取り込む (シェル側で変えた分を拾う)。
 * 設定アプリを開いたときと、この2ページへ移ったときに呼ぶ。 */
static void settings_refresh_shared(void) {
    prism_settings fresh;
    settings_defaults(&fresh);
    size_t len = 0;
    const char *saved = vfs_read_ptr("settings.txt", &len);
    if (saved != NULL && len > 0) {
        settings_parse(&fresh, saved, len);
    }
    desk_settings.usb_media = fresh.usb_media;
    desk_settings.pin_min_len = fresh.pin_min_len;
    desk_settings.pin_require_digit = fresh.pin_require_digit;
    desk_settings.lockout_attempts = fresh.lockout_attempts;
    desk_settings.lockout_seconds = fresh.lockout_seconds;
    desk_settings.update_channel = fresh.update_channel;
    desk_settings.auto_security = fresh.auto_security;
    desk_settings.tz_offset = fresh.tz_offset;
    size_t i = 0;
    while (fresh.update_host[i] != '\0' &&
           i + 1 < sizeof(desk_settings.update_host)) {
        desk_settings.update_host[i] = fresh.update_host[i];
        i++;
    }
    desk_settings.update_host[i] = '\0';
    i = 0;
    while (fresh.update_path[i] != '\0' &&
           i + 1 < sizeof(desk_settings.update_path)) {
        desk_settings.update_path[i] = fresh.update_path[i];
        i++;
    }
    desk_settings.update_path[i] = '\0';
    sec_stage_from_settings();
}

/* セキュリティ/更新項目の合流保存。save_desk_settings と
 * 同じ作法でファイルを読み直し、デスクトップ所有項目に
 * セキュリティと更新の項目を重ねてから書くので、どちらか
 * 一方の保存がもう一方の変更を巻き戻すことはない。 */
static int save_shared_settings(void) {
    prism_settings merged;
    settings_defaults(&merged);
    size_t len = 0;
    const char *saved = vfs_read_ptr("settings.txt", &len);
    if (saved != NULL && len > 0) {
        settings_parse(&merged, saved, len);
    }
    settings_overlay_desktop(&merged, &desk_settings);
    settings_overlay_security(&merged, &desk_settings);
    merged.update_channel = desk_settings.update_channel;
    merged.auto_security = desk_settings.auto_security;
    merged.tz_offset = desk_settings.tz_offset;
    size_t i = 0;
    while (desk_settings.update_host[i] != '\0' &&
           i + 1 < sizeof(merged.update_host)) {
        merged.update_host[i] = desk_settings.update_host[i];
        i++;
    }
    merged.update_host[i] = '\0';
    i = 0;
    while (desk_settings.update_path[i] != '\0' &&
           i + 1 < sizeof(merged.update_path)) {
        merged.update_path[i] = desk_settings.update_path[i];
        i++;
    }
    merged.update_path[i] = '\0';
    desk_settings = merged;
    char buf[SETTINGS_TEXT_CAP];
    size_t n = settings_serialize(&merged, buf, sizeof(buf));
    if (n >= sizeof(buf)) {
        /* 切り捨てた設定は保存しない (監査 M1)。 */
        return -1;
    }
    if (shell_session_is_guest()) {
        /* ゲストの変更はセッション内だけ (監査 L5)。 */
        serial_write_line("settings: guest session, not saved");
        shell_notify_language_changed(merged.language);
        return 0;
    }
    if (vfs_write("settings.txt", buf, n) != 0) {
        return -1;
    }
    shell_notify_language_changed(merged.language);
    persist_maybe_sync();
    return 0;
}

/* ---- アカウント: users.txt の読み書き (シェルと同じ作法) ---- */
static char acct_text[PRISM_USERS_TEXT_MAX];
static size_t acct_text_len;
static char acct_names[24][PRISM_USER_NAME_MAX + 1];
static int acct_user_count;

static int acct_users_load(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr(PRISM_USERS_FILE, &len);
    if (text == NULL || len == 0) {
        acct_text[0] = '\0';
        acct_text_len = 0;
        return 0;
    }
    if (len + 1 > sizeof(acct_text)) {
        return -1;
    }
    memcpy(acct_text, text, len);
    acct_text[len] = '\0';
    acct_text_len = len;
    return 0;
}

static int acct_users_save(const char *text) {
    if (vfs_write(PRISM_USERS_FILE, text, strlen(text)) != 0) {
        return -1;
    }
    persist_maybe_sync();
    return 0;
}

/* "user " 行から名前を集める (シェルの user list と同じ読み方)。 */
static void acct_refresh(void) {
    acct_user_count = 0;
    if (acct_users_load() != 0) {
        return;
    }
    size_t off = 0;
    while (off < acct_text_len && acct_user_count < 24) {
        size_t st = off;
        while (off < acct_text_len && acct_text[off] != '\n') {
            off++;
        }
        size_t ll = off - st;
        if (off < acct_text_len) {
            off++;
        }
        if (ll > 5 && memcmp(acct_text + st, "user ", 5) == 0) {
            char *nm = acct_names[acct_user_count];
            size_t q = 0;
            size_t src = st + 5;
            while (src < st + ll && acct_text[src] != ' ' &&
                   q < PRISM_USER_NAME_MAX) {
                nm[q++] = acct_text[src++];
            }
            nm[q] = '\0';
            if (q > 0) {
                acct_user_count++;
            }
        }
    }
}

/* 管理者として数えるアカウントの数 (主アカウント + 役割が
 * admin の追加ユーザー)。最後の管理者を消さないための判定。 */
static int acct_admin_count(void) {
    int n = vfs_exists(PRISM_ACCOUNT_FILE) ? 1 : 0;
    for (int i = 0; i < acct_user_count; i++) {
        char role[16];
        if (users_role(acct_text, acct_text_len, acct_names[i],
                       role, sizeof(role)) == 0 &&
            strcmp(role, "admin") == 0) {
            n++;
        }
    }
    return n;
}

static int acct_totp_on(const char *name) {
    char b32[64];
    return users_totp(acct_text, acct_text_len, name, b32,
                      sizeof(b32)) == 0;
}

static void acct_clear_form(void) {
    acct_name[0] = '\0';
    acct_name_len = 0;
    acct_pin1[0] = '\0';
    acct_pin1_len = 0;
    acct_pin2[0] = '\0';
    acct_pin2_len = 0;
    acct_role_admin = 0;
    acct_field = 0;
}

/* user 行 + wrap 行を作り直して保存する (追加と PIN 変更の
 * 共通部、シェルの user add / user passwd と同じ手順)。
 * acct_text は呼び出し側が読み直しておくこと。 */
static int acct_write_record(const char *name, const char *pin,
                             const char *role) {
    uint8_t mk[32];
    if (persist_get_key(mk) != 0) {
        set_page_status(L("The disk key is not active; log in first.",
                          "ディスク鍵が有効ではありません。先にログインしてください"));
        return -1;
    }
    uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
    entropy_bytes(salt, sizeof(salt));
    char uline[256];
    int bad = users_make_line(name, salt, PRISM_ACCOUNT_ITERATIONS,
                              pin, role, uline, sizeof(uline)) != 0;
    uint8_t uk[32];
    char wline[256];
    if (!bad) {
        crypto_kdf(pin, salt, PRISM_ACCOUNT_SALT_LEN,
                   PRISM_ACCOUNT_ITERATIONS, uk);
        bad = users_make_wrap_line(name, mk, uk, wline,
                                   sizeof(wline)) != 0;
        memset(uk, 0, sizeof(uk));
    }
    memset(mk, 0, sizeof(mk));
    if (bad) {
        set_page_status(L("Could not build the user record.",
                          "ユーザーの記録を作れませんでした"));
        return -1;
    }
    static char out[PRISM_USERS_TEXT_MAX];
    if (users_upsert(acct_text, acct_text_len, uline, out,
                     sizeof(out)) != 0) {
        set_page_status(L("users.txt is full.",
                          "users.txtがいっぱいです"));
        return -1;
    }
    static char merged[PRISM_USERS_TEXT_MAX];
    if (users_upsert(out, strlen(out), wline, merged,
                     sizeof(merged)) != 0) {
        set_page_status(L("users.txt is full.",
                          "users.txtがいっぱいです"));
        return -1;
    }
    if (acct_users_save(merged) != 0) {
        set_page_status(L("Could not save users.txt.",
                          "users.txtを保存できませんでした"));
        return -1;
    }
    return 0;
}

/* ユーザー追加の実体。 */
static int acct_do_add(void) {
    if (acct_users_load() != 0) {
        set_page_status(L("users.txt is too large.",
                          "users.txtが大きすぎます"));
        return -1;
    }
    if (users_exists(acct_text, acct_text_len, acct_name)) {
        set_page_status(L("That user already exists.",
                          "そのユーザーは既にあります"));
        return -1;
    }
    if (acct_write_record(acct_name, acct_pin1,
                          acct_role_admin ? "admin" : "user") != 0) {
        return -1;
    }
    audit_log_event("user-add", acct_name);
    serial_write("settings: user added ");
    serial_write_line(acct_name);
    char msg[96];
    size_t p = 0;
    buf_cat(msg, sizeof(msg), &p,
            L("User added: ", "ユーザーを追加しました: "));
    buf_cat(msg, sizeof(msg), &p, acct_name);
    set_page_status(msg);
    return 0;
}

/* PIN 変更の実体。追加ユーザーは既存の役割を保持したまま
 * 記録を作り直す。主管理者はシェルの passwd と同じ手順
 * (.account を作り直し、新しい PIN の鍵に切り替える)。 */
static int acct_do_passwd(void) {
    if (strcmp(acct_target, "admin") == 0) {
        uint8_t salt[PRISM_ACCOUNT_SALT_LEN];
        entropy_bytes(salt, sizeof(salt));
        char text[PRISM_ACCOUNT_TEXT_MAX];
        if (account_make_text(salt, PRISM_ACCOUNT_ITERATIONS,
                              acct_pin1, text, sizeof(text)) != 0) {
            set_page_status(L("Failed to set PIN.",
                              "PINを設定できませんでした"));
            return -1;
        }
        if (vfs_write(PRISM_ACCOUNT_FILE, text, strlen(text)) !=
            0) {
            set_page_status(L("Failed to save account.",
                              "アカウントを保存できませんでした"));
            return -1;
        }
        uint8_t key[32];
        crypto_kdf(acct_pin1, salt, PRISM_ACCOUNT_SALT_LEN,
                   PRISM_ACCOUNT_ITERATIONS, key);
        /* 追加ユーザーの主鍵の包みを新しい主鍵へ包み直す
         * (シェルの passwd と同じ。しないとプライマリ PIN
         * 変更で追加ユーザーがログインできなくなる)。 */
        {
            uint8_t old_mk[32];
            if (acct_users_load() == 0 &&
                persist_get_key(old_mk) == 0) {
                static char new_utext[PRISM_USERS_TEXT_MAX];
                int nwrap = 0;
                if (users_rewrap_all(acct_text, acct_text_len,
                                     old_mk, key, new_utext,
                                     sizeof(new_utext),
                                     &nwrap) == 0 &&
                    nwrap > 0) {
                    (void)vfs_write(PRISM_USERS_FILE, new_utext,
                                    strlen(new_utext));
                    serial_write("settings: re-wrapped the disk "
                                 "key for added users");
                    serial_write_line("");
                }
            }
            memset(old_mk, 0, sizeof(old_mk));
        }
        persist_set_key(key);
        memset(key, 0, sizeof(key));
        persist_maybe_sync();
        audit_log_event("passwd", "PIN changed");
        serial_write_line("settings: admin PIN changed");
        set_page_status(L("PIN changed.", "PINを変更しました"));
        return 0;
    }
    if (acct_users_load() != 0) {
        set_page_status(L("users.txt is too large.",
                          "users.txtが大きすぎます"));
        return -1;
    }
    if (!users_exists(acct_text, acct_text_len, acct_target)) {
        set_page_status(L("No such user.", "そのユーザーはありません"));
        return -1;
    }
    char role[16];
    if (users_role(acct_text, acct_text_len, acct_target, role,
                   sizeof(role)) != 0) {
        strcpy(role, "user");
    }
    if (acct_write_record(acct_target, acct_pin1, role) != 0) {
        return -1;
    }
    audit_log_event("user-passwd", acct_target);
    serial_write("settings: user PIN changed ");
    serial_write_line(acct_target);
    char msg[96];
    size_t p = 0;
    buf_cat(msg, sizeof(msg), &p,
            L("PIN updated: ", "PINを更新しました: "));
    buf_cat(msg, sizeof(msg), &p, acct_target);
    set_page_status(msg);
    return 0;
}

/* ユーザー削除の実体。 */
static int acct_do_del(void) {
    if (acct_users_load() != 0) {
        set_page_status(L("users.txt is too large.",
                          "users.txtが大きすぎます"));
        return -1;
    }
    if (!users_exists(acct_text, acct_text_len, acct_target)) {
        set_page_status(L("No such user.", "そのユーザーはありません"));
        return -1;
    }
    static char out[PRISM_USERS_TEXT_MAX];
    if (users_remove_user(acct_text, acct_text_len, acct_target,
                          out, sizeof(out)) != 0 ||
        acct_users_save(out) != 0) {
        set_page_status(L("Could not save users.txt.",
                          "users.txtを保存できませんでした"));
        return -1;
    }
    audit_log_event("user-del", acct_target);
    serial_write("settings: user deleted ");
    serial_write_line(acct_target);
    char msg[96];
    size_t p = 0;
    buf_cat(msg, sizeof(msg), &p,
            L("User deleted: ", "ユーザーを削除しました: "));
    buf_cat(msg, sizeof(msg), &p, acct_target);
    set_page_status(msg);
    return 0;
}

/* TOTP 秘密の生成と設定 (シェルの totp setup と同じ形式)。 */
static int acct_do_totp_set(void) {
    uint8_t secret[10];
    entropy_bytes(secret, sizeof(secret));
    char b32[24];
    if (base32_encode(secret, sizeof(secret), b32, sizeof(b32)) <
        0) {
        memset(secret, 0, sizeof(secret));
        set_page_status(L("Could not make a secret.",
                          "秘密を作れませんでした"));
        return -1;
    }
    memset(secret, 0, sizeof(secret));
    if (acct_users_load() != 0) {
        set_page_status(L("users.txt is too large.",
                          "users.txtが大きすぎます"));
        return -1;
    }
    char tline[96];
    size_t p = 0;
    buf_cat(tline, sizeof(tline), &p, "totp ");
    buf_cat(tline, sizeof(tline), &p, acct_target);
    buf_cat(tline, sizeof(tline), &p, " ");
    buf_cat(tline, sizeof(tline), &p, b32);
    static char out[PRISM_USERS_TEXT_MAX];
    if (users_upsert(acct_text, acct_text_len, tline, out,
                     sizeof(out)) != 0 ||
        acct_users_save(out) != 0) {
        set_page_status(L("Could not save users.txt.",
                          "users.txtを保存できませんでした"));
        return -1;
    }
    /* 画面で認証アプリへ伝えるため、秘密を表示用に控える。 */
    p = 0;
    buf_cat(acct_totp_secret, sizeof(acct_totp_secret), &p, b32);
    audit_log_event("totp-setup", acct_target);
    serial_write("settings: TOTP set for ");
    serial_write_line(acct_target);
    return 0;
}

/* TOTP の無効化 (totp 行だけを落とす。シェルの totp off と
 * 同じ読み方)。 */
static int acct_do_totp_off(void) {
    if (acct_users_load() != 0) {
        set_page_status(L("users.txt is too large.",
                          "users.txtが大きすぎます"));
        return -1;
    }
    static char out[PRISM_USERS_TEXT_MAX];
    size_t op = 0;
    size_t off = 0;
    int dropped = 0;
    while (off < acct_text_len) {
        size_t st = off;
        while (off < acct_text_len && acct_text[off] != '\n') {
            off++;
        }
        size_t ll = off - st;
        if (off < acct_text_len) {
            off++;
        }
        int drop = 0;
        if (ll > 5 && memcmp(acct_text + st, "totp ", 5) == 0) {
            size_t q = st + 5;
            size_t cn = 0;
            int match = 1;
            while (acct_target[cn] != '\0') {
                if (q + cn >= st + ll ||
                    acct_text[q + cn] != acct_target[cn]) {
                    match = 0;
                    break;
                }
                cn++;
            }
            if (match &&
                (q + cn >= st + ll || acct_text[q + cn] == ' ')) {
                drop = 1;
            }
        }
        if (drop) {
            dropped = 1;
            continue;
        }
        memcpy(out + op, acct_text + st, ll);
        op += ll;
        out[op++] = '\n';
    }
    out[op] = '\0';
    if (!dropped) {
        set_page_status(L("TOTP is not set up.",
                          "TOTPは設定されていません"));
        return -1;
    }
    if (acct_users_save(out) != 0) {
        set_page_status(L("Could not save users.txt.",
                          "users.txtを保存できませんでした"));
        return -1;
    }
    audit_log_event("totp-off", acct_target);
    serial_write("settings: TOTP disabled for ");
    serial_write_line(acct_target);
    char msg[96];
    size_t p = 0;
    buf_cat(msg, sizeof(msg), &p,
            L("TOTP disabled for ", "TOTPを無効にしました: "));
    buf_cat(msg, sizeof(msg), &p, acct_target);
    set_page_status(msg);
    return 0;
}

/* ---- セキュリティ: 保存と許可リストの実体 ---- */

/* 仮値を desk_settings へ移し、合流保存する。 */
static int sec_do_save(void) {
    desk_settings.usb_media = sec_usb;
    desk_settings.pin_min_len = sec_pin_min;
    desk_settings.pin_require_digit = sec_pin_digit;
    desk_settings.lockout_attempts = sec_lock_attempts;
    desk_settings.lockout_seconds = sec_lock_seconds;
    if (save_shared_settings() != 0) {
        set_page_status(L("Could not save settings.txt.",
                          "settings.txtを保存できませんでした"));
        return -1;
    }
    sec_dirty = 0;
    audit_log_event("security", "security settings updated (GUI)");
    serial_write_line("settings: security saved");
    set_page_status(L("Security settings saved.",
                      "セキュリティ設定を保存しました"));
    return 0;
}

/* 許可リストの切り替え (シェルの security allow/deny と
 * 同じファイル・同じ形式)。 */
static int sec_do_allow(void) {
    int allowed[SEC_APP_COUNT];
    size_t len = 0;
    const char *text = vfs_read_ptr("allowlist.txt", &len);
    allowlist_parse(text, len, allowed);
    allowed[sec_pin_app] = sec_pin_allow;
    char buf[256];
    size_t m = allowlist_serialize(allowed, buf, sizeof(buf));
    if (vfs_write("allowlist.txt", buf, m) != 0) {
        set_page_status(L("Could not save allowlist.txt.",
                          "allowlist.txtを保存できませんでした"));
        return -1;
    }
    persist_maybe_sync();
    desk_load_allowlist();
    audit_log_event("security",
                    sec_pin_allow ? "app allowed" : "app denied");
    serial_write("settings: app ");
    serial_write(sec_app_name(sec_pin_app));
    serial_write_line(sec_pin_allow ? " allowed" : " denied");
    char msg[96];
    size_t p = 0;
    buf_cat(msg, sizeof(msg), &p, sec_app_name(sec_pin_app));
    buf_cat(msg, sizeof(msg), &p,
            sec_pin_allow ? L(": allowed", ": 許可しました")
                          : L(": denied", ": 拒否しました"));
    set_page_status(msg);
    return 0;
}

/* ---- 更新: 確認/切り替え/適用/ロールバックの実体 ---- */

static void upd_do_check(void) {
    upd_confirm = 0;
    int rc = shell_update_check(&upd_info);
    upd_check_rc = rc;
    if (rc == SHELL_UPDATE_NO_SERVER) {
        set_page_status(L("No update server configured.",
                          "更新サーバーが設定されていません"));
        serial_write_line("settings: update check: no server");
    } else if (rc == SHELL_UPDATE_UNREACHABLE) {
        set_page_status(
            L("Update check failed: could not reach the server.",
              "更新の確認に失敗しました: サーバーに届きません"));
        serial_write_line("settings: update check: unreachable");
    } else if (rc == SHELL_UPDATE_BAD_SIGNATURE) {
        set_page_status(L("Update check failed: bad signature.",
                          "更新の確認に失敗しました: 署名が正しくありません"));
        serial_write_line("settings: update check: bad signature");
    } else if (rc == SHELL_UPDATE_BAD_MANIFEST) {
        set_page_status(L("Update check failed: bad manifest.",
                          "更新の確認に失敗しました: マニフェストが壊れています"));
        serial_write_line("settings: update check: bad manifest");
    } else if (rc == SHELL_UPDATE_UP_TO_DATE) {
        char msg[96];
        size_t p = 0;
        buf_cat(msg, sizeof(msg), &p,
                L("Already up to date (", "最新の状態です ("));
        buf_cat(msg, sizeof(msg), &p, PRISM_VERSION);
        buf_cat(msg, sizeof(msg), &p, ").");
        set_page_status(msg);
        serial_write_line("settings: update check: up to date");
    } else {
        char msg[128];
        size_t p = 0;
        buf_cat(msg, sizeof(msg), &p,
                L("Update available: ", "更新があります: "));
        buf_cat(msg, sizeof(msg), &p, upd_info.version);
        buf_cat(msg, sizeof(msg), &p, " (");
        buf_cat(msg, sizeof(msg), &p, upd_info.type);
        buf_cat(msg, sizeof(msg), &p, ", ");
        buf_cat(msg, sizeof(msg), &p, upd_info.channel);
        buf_cat(msg, sizeof(msg), &p, ")");
        set_page_status(msg);
        serial_write("settings: update check: available ");
        serial_write_line(upd_info.version);
    }
}

static int upd_do_auto(void) {
    desk_settings.auto_security = !desk_settings.auto_security;
    if (save_shared_settings() != 0) {
        set_page_status(L("Could not save settings.txt.",
                          "settings.txtを保存できませんでした"));
        return -1;
    }
    serial_write_line(desk_settings.auto_security
                          ? "settings: auto security updates on"
                          : "settings: auto security updates off");
    set_page_status(desk_settings.auto_security
                        ? L("Auto security updates: on.",
                            "セキュリティ更新の自動適用: オン")
                        : L("Auto security updates: off.",
                            "セキュリティ更新の自動適用: オフ"));
    return 0;
}

static int upd_do_channel(void) {
    desk_settings.update_channel = !desk_settings.update_channel;
    if (save_shared_settings() != 0) {
        set_page_status(L("Could not save settings.txt.",
                          "settings.txtを保存できませんでした"));
        return -1;
    }
    /* チャンネルが変わると前の確認結果は使えない。 */
    upd_check_rc = 99;
    upd_confirm = 0;
    serial_write_line(desk_settings.update_channel
                          ? "settings: update channel beta"
                          : "settings: update channel stable");
    set_page_status(desk_settings.update_channel
                        ? L("Update channel: beta.",
                            "更新チャンネル: ベータ")
                        : L("Update channel: stable.",
                            "更新チャンネル: 安定版"));
    return 0;
}

static int upd_do_apply(void) {
    int rc = shell_update_apply_pending();
    upd_check_rc = 99;
    upd_confirm = 0;
    if (rc == 0) {
        serial_write_line("settings: update applied");
        set_page_status(L("Update applied.", "更新を適用しました"));
        desktop_notify(L("Update applied", "更新を適用しました"));
    } else {
        serial_write_line("settings: update apply failed");
        set_page_status(
            L("Update failed (rolled back if a restore point existed).",
              "更新に失敗しました (復元ポイントがあれば巻き戻し済みです)"));
    }
    return rc;
}

static int upd_do_rollback(void) {
    char name[DISKFS_SNAP_NAME_MAX + 1];
    int rc = shell_update_rollback(name, sizeof(name));
    upd_confirm = 0;
    if (rc == 0) {
        desktop_notify(L("Update rolled back", "更新を巻き戻しました"));
        serial_write("settings: rolled back to ");
        serial_write_line(name);
        char msg[96];
        size_t p = 0;
        buf_cat(msg, sizeof(msg), &p, L("Rolled back to '", "巻き戻しました: '"));
        buf_cat(msg, sizeof(msg), &p, name);
        buf_cat(msg, sizeof(msg), &p, "'.");
        set_page_status(msg);
    } else if (rc == -1) {
        set_page_status(L("No restore points to roll back to.",
                          "巻き戻せる復元ポイントがありません"));
        serial_write_line("settings: rollback: no restore points");
    } else if (rc == -2) {
        set_page_status(L("No update restore point found.",
                          "更新用の復元ポイントがありません"));
        serial_write_line("settings: rollback: no update point");
    } else {
        set_page_status(L("Rollback failed.", "巻き戻しに失敗しました"));
        serial_write_line("settings: rollback failed");
    }
    return rc;
}

/* 復元ポイント名の控え (表示用)。 */
static char upd_snaps[DISKFS_SNAP_SLOTS][DISKFS_SNAP_NAME_MAX + 1];
static int upd_snap_count;

static void upd_refresh_snaps(void) {
    diskfs_snapshot_info infos[DISKFS_SNAP_SLOTS];
    int count = 0;
    upd_snap_count = 0;
    if (persist_snapshot_list(infos, DISKFS_SNAP_SLOTS, &count) !=
        0) {
        return;
    }
    for (int i = 0; i < count && i < DISKFS_SNAP_SLOTS; i++) {
        size_t p = 0;
        while (infos[i].name[p] != '\0' &&
               p + 1 < sizeof(upd_snaps[0])) {
            upd_snaps[upd_snap_count][p] = infos[i].name[p];
            p++;
        }
        upd_snaps[upd_snap_count][p] = '\0';
        upd_snap_count++;
    }
}

/* ---- 管理者 PIN 確認ダイアログ ---- */

static void pin_dialog_begin(int op) {
    if (!vfs_exists(PRISM_ACCOUNT_FILE)) {
        set_page_status(
            L("No PIN is set. Run `passwd` first in the Terminal.",
              "PINが設定されていません。先にターミナルで passwd を実行してください"));
        return;
    }
    pin_dialog_open = 1;
    pin_dialog_op = op;
    pin_dialog_len = 0;
    pin_dialog_buf[0] = '\0';
    pin_dialog_error = 0;
}

static int net_do_apply(void);
static void net_do_feed_save(void);
static void clk_apply(void);
static void clk_ntp_sync(void);
static void adm_do_fde_enable(void);
static void adm_do_fde_disable(void);
static void adm_do_snap_restore(void);
static void adm_do_snap_delete(void);
static void adm_do_drv_toggle(void);
static void adm_do_fw_add(void);
static void adm_do_fw_remove(void);
static void adm_do_fw_clear(void);

static void pin_dialog_execute(int op) {
    if (op == PINOP_ACCT_ADD) {
        if (acct_do_add() == 0) {
            acct_mode = ACCT_MODE_LIST;
            acct_clear_form();
            acct_refresh();
            if (acct_selected > acct_user_count + 1) {
                acct_selected = acct_user_count + 1;
            }
        }
    } else if (op == PINOP_ACCT_PASSWD) {
        if (acct_do_passwd() == 0) {
            acct_mode = ACCT_MODE_LIST;
            acct_clear_form();
        }
    } else if (op == PINOP_ACCT_DEL) {
        acct_do_del();
        acct_mode = ACCT_MODE_LIST;
        acct_refresh();
        if (acct_selected > acct_user_count + 1) {
            acct_selected = acct_user_count + 1;
        }
    } else if (op == PINOP_ACCT_TOTP_SET) {
        if (acct_do_totp_set() == 0) {
            acct_mode = ACCT_MODE_TOTP_SHOW;
        }
    } else if (op == PINOP_ACCT_TOTP_OFF) {
        acct_do_totp_off();
    } else if (op == PINOP_SEC_SAVE) {
        sec_do_save();
    } else if (op == PINOP_SEC_ALLOW) {
        sec_do_allow();
    } else if (op == PINOP_UPD_AUTO) {
        upd_do_auto();
    } else if (op == PINOP_UPD_CHANNEL) {
        upd_do_channel();
    } else if (op == PINOP_UPD_APPLY) {
        upd_do_apply();
        upd_refresh_snaps();
    } else if (op == PINOP_UPD_ROLLBACK) {
        upd_do_rollback();
        upd_refresh_snaps();
    } else if (op == PINOP_NET_APPLY) {
        net_do_apply();
    } else if (op == PINOP_NET_FEED) {
        net_do_feed_save();
    } else if (op == PINOP_ADM_FDE_ENABLE) {
        adm_do_fde_enable();
    } else if (op == PINOP_ADM_FDE_DISABLE) {
        adm_do_fde_disable();
    } else if (op == PINOP_ADM_SNAP_RESTORE) {
        adm_do_snap_restore();
    } else if (op == PINOP_ADM_SNAP_DELETE) {
        adm_do_snap_delete();
    } else if (op == PINOP_ADM_DRV_TOGGLE) {
        adm_do_drv_toggle();
    } else if (op == PINOP_ADM_FW_ADD) {
        adm_do_fw_add();
    } else if (op == PINOP_ADM_FW_REMOVE) {
        adm_do_fw_remove();
    } else if (op == PINOP_CLK_SET) {
        clk_apply();
    } else if (op == PINOP_CLK_NTP) {
        clk_ntp_sync();
    } else if (op == PINOP_ADM_FW_CLEAR) {
        adm_do_fw_clear();
    }
}

static int pin_dialog_key(int c) {
    if (c == 0x1B) {
        pin_dialog_open = 0;
        pin_dialog_len = 0;
        pin_dialog_buf[0] = '\0';
        desktop_redraw();
        return 1;
    }
    if (c == '\n' || c == '\r') {
        if (shell_admin_pin_verify(pin_dialog_buf)) {
            int op = pin_dialog_op;
            pin_dialog_open = 0;
            memset(pin_dialog_buf, 0, sizeof(pin_dialog_buf));
            pin_dialog_len = 0;
            pin_dialog_execute(op);
        } else {
            pin_dialog_error = 1;
            pin_dialog_len = 0;
            pin_dialog_buf[0] = '\0';
            /* シェルの admin_check と同じく、失敗を監査に
             * 残す (監査 L1)。 */
            audit_log_event("security-fail", "admin PIN rejected");
            serial_write_line("settings: admin PIN rejected");
        }
        desktop_redraw();
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        if (pin_dialog_len > 0) {
            pin_dialog_len--;
            pin_dialog_buf[pin_dialog_len] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    if (c >= 32 && c < 127 && pin_dialog_len < 16) {
        pin_dialog_buf[pin_dialog_len++] = (char)c;
        pin_dialog_buf[pin_dialog_len] = '\0';
        pin_dialog_error = 0;
        desktop_redraw();
    }
    return 1;
}

static void draw_pin_dialog(void) {
    uint32_t panel = col_title_bg();
    uint32_t fg = col_text();
    uint32_t acc = col_accent();
    uint64_t pw = win_w() * 3 / 5;
    uint64_t ph = 7 * (uint64_t)CELL_H;
    uint64_t px = win_x() + (win_w() - pw) / 2;
    uint64_t py = win_y() + TITLE_H + (win_h() - TITLE_H - ph) / 2;
    fb_fill_rect(px, py, pw, ph, acc);
    fb_fill_rect(px + 2, py + 2, pw - 4, ph - 4, panel);
    draw_text_small(px + 16, py + (uint64_t)CELL_H,
                    L("Admin PIN required", "管理者PINが必要です"),
                    acc, panel);
    char line[64];
    size_t p = 0;
    buf_cat(line, sizeof(line), &p, L("PIN: ", "PIN: "));
    buf_cat_masked(line, sizeof(line), &p, pin_dialog_len);
    buf_cat(line, sizeof(line), &p, "_");
    draw_text_small(px + 16, py + (uint64_t)(3 * CELL_H), line, fg,
                    panel);
    if (pin_dialog_error) {
        draw_text_small(px + 16, py + (uint64_t)(4 * CELL_H),
                        L("Wrong PIN.", "PINが違います"), acc,
                        panel);
    }
    draw_text_small(px + 16, py + (uint64_t)(5 * CELL_H),
                    L("Enter = confirm  Esc = cancel",
                      "Enter = 確認  Esc = 中止"),
                    col_dim(), panel);
}

/* ---- 描画: 3ページ共通の末尾 ---- */
static void draw_settings_tail(uint64_t x0, uint64_t y0,
                               const char *status,
                               const char *hints,
                               const char *tab_hint) {
    int rows = text_rows();
    if (hints != NULL) {
        draw_text_small(x0, y0 + (uint64_t)((rows - 4) * CELL_H),
                        hints, col_dim(), col_window_bg());
    }
    if (status != NULL && status[0] != '\0') {
        draw_text_small(x0, y0 + (uint64_t)((rows - 3) * CELL_H),
                        status, col_accent(), col_window_bg());
    }
    draw_text_small(x0, y0 + (uint64_t)((rows - 2) * CELL_H),
                    tab_hint, col_text(), col_window_bg());
    draw_text_small(x0, y0 + (uint64_t)((rows - 1) * CELL_H),
                    L("Esc = back to Terminal", "Esc = ターミナルへ戻る"),
                    col_dim(), col_window_bg());
}

/* 選択行の描画 (ショートカットページと同じ作法)。 */
static void draw_sel_row(uint64_t x0, uint64_t py, const char *line,
                         int selected) {
    if (selected) {
        fb_fill_rect(x0, py, win_w() - 16, CELL_H, col_accent());
        fb_draw_text(x0 + 4, py, line, rgb(0xFFFFFF), col_accent(),
                     1);
    } else {
        fb_draw_text(x0 + 4, py, line, col_text(), col_window_bg(),
                     1);
    }
}

/* ---- 描画: アカウントページ ---- */

/* フォームの1欄 (メール作成と同じ作法。PIN は伏せ字)。 */
static void acct_draw_field(uint64_t x0, uint64_t y0, int row,
                            const char *label, const char *value,
                            int active, int masked) {
    char line[96];
    size_t p = 0;
    buf_cat(line, sizeof(line), &p, label);
    buf_cat(line, sizeof(line), &p, " ");
    if (masked) {
        int n = 0;
        while (value[n] != '\0') {
            n++;
        }
        buf_cat_masked(line, sizeof(line), &p, n);
    } else {
        buf_cat(line, sizeof(line), &p, value);
    }
    if (active) {
        buf_cat(line, sizeof(line), &p, "_");
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), line,
                        rgb(0xFFFFFF), col_accent());
    } else {
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), line,
                        col_text(), col_window_bg());
    }
}

static void draw_acct_form(uint64_t x0, uint64_t y0) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    if (acct_mode == ACCT_MODE_ADD) {
        draw_text_small(x0, y0, L("Add user", "ユーザーを追加"), acc,
                        bg);
        acct_draw_field(x0, y0, 2, L("Name:", "名前:"), acct_name,
                        acct_field == 0, 0);
        acct_draw_field(x0, y0, 3, L("PIN:", "PIN:"), acct_pin1,
                        acct_field == 1, 1);
        acct_draw_field(x0, y0, 4, L("Confirm PIN:", "PIN (確認):"),
                        acct_pin2, acct_field == 2, 1);
        char line[80];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p, L("Role: ", "役割: "));
        buf_cat(line, sizeof(line), &p,
                acct_role_admin ? L("Admin", "管理者")
                                : L("User", "一般ユーザー"));
        buf_cat(line, sizeof(line), &p,
                L("  (Left/Right)", "  (左右キー)"));
        draw_sel_row(x0, y0 + (uint64_t)(5 * CELL_H), line,
                     acct_field == 3);
        draw_sel_row(x0, y0 + (uint64_t)(7 * CELL_H),
                     L("[ Add user ]", "[ ユーザーを追加 ]"),
                     acct_field == 4);
    } else {
        char title[80];
        size_t p = 0;
        buf_cat(title, sizeof(title), &p,
                L("Change PIN: ", "PINを変更: "));
        buf_cat(title, sizeof(title), &p, acct_target);
        draw_text_small(x0, y0, title, acc, bg);
        acct_draw_field(x0, y0, 2, L("New PIN:", "新しいPIN:"),
                        acct_pin1, acct_field == 0, 1);
        acct_draw_field(x0, y0, 3, L("Confirm PIN:", "PIN (確認):"),
                        acct_pin2, acct_field == 1, 1);
        draw_sel_row(x0, y0 + (uint64_t)(5 * CELL_H),
                     L("[ Change PIN ]", "[ PINを変更 ]"),
                     acct_field == 2);
    }
    (void)fg;
    draw_settings_tail(
        x0, y0, acct_status,
        L("Tab = next field  Enter = next / submit",
          "Tab = 次の欄  Enter = 次へ / 実行"),
        L("Tab = next field", "Tab = 次の欄"));
}

static void draw_settings_accounts(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    int rows = text_rows();
    draw_text_small(x0, y0, L("Settings - Accounts", "設定 - アカウント"),
                    acc, bg);
    if (acct_mode == ACCT_MODE_ADD || acct_mode == ACCT_MODE_PASSWD) {
        draw_acct_form(x0, y0);
        return;
    }
    if (acct_mode == ACCT_MODE_DEL) {
        char line[96];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p, L("Delete user '", "ユーザー「"));
        buf_cat(line, sizeof(line), &p, acct_target);
        buf_cat(line, sizeof(line), &p,
                L("'?", "」を削除しますか？"));
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), line, fg,
                        bg);
        draw_settings_tail(x0, y0, acct_status,
                           L("Enter = delete  Esc = cancel",
                             "Enter = 削除  Esc = 中止"),
                           L("Tab = Security page",
                             "Tab = セキュリティのページ"));
        return;
    }
    if (acct_mode == ACCT_MODE_TOTP_SHOW) {
        char line[96];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p,
                L("TOTP secret for ", "TOTPの秘密: "));
        buf_cat(line, sizeof(line), &p, acct_target);
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), line, fg,
                        bg);
        draw_text_small(x0, y0 + (uint64_t)(3 * CELL_H),
                        acct_totp_secret, acc, bg);
        draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H),
                        L("Enter it in your authenticator app.",
                          "認証アプリにこの秘密を入力してください"),
                        dim, bg);
        draw_settings_tail(x0, y0, acct_status,
                           L("Any key = back", "キー入力で戻ります"),
                           L("Tab = Security page",
                             "Tab = セキュリティのページ"));
        return;
    }
    /* 一覧 */
    acct_refresh();
    {
        char line[128];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p, L("Signed in as: ", "ログイン中: "));
        buf_cat(line, sizeof(line), &p, shell_current_user_name());
        buf_cat(line, sizeof(line), &p, " (");
        buf_cat(line, sizeof(line), &p,
                shell_session_is_guest()
                    ? L("guest", "ゲスト")
                    : shell_current_user_is_admin()
                          ? L("admin", "管理者")
                          : L("user", "一般ユーザー"));
        buf_cat(line, sizeof(line), &p, ")");
        draw_text_small(x0, y0 + (uint64_t)CELL_H, line, dim, bg);
    }
    int total = acct_user_count + 2;
    if (acct_selected > total - 1) {
        acct_selected = total - 1;
    }
    if (acct_selected < 0) {
        acct_selected = 0;
    }
    int first_row = 3;
    int visible = rows - first_row - 4;
    if (visible < 1) {
        visible = 1;
    }
    int start = 0;
    if (acct_selected >= visible) {
        start = acct_selected - visible + 1;
    }
    for (int i = start; i < total && i < start + visible; i++) {
        char line[96];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p, i == acct_selected ? "> " : "  ");
        if (i == 0) {
            buf_cat(line, sizeof(line), &p, "admin");
            buf_cat(line, sizeof(line), &p,
                    L(" (primary account)", " (主アカウント)"));
            buf_cat(line, sizeof(line), &p, L("  TOTP: ", "  TOTP: "));
            buf_cat(line, sizeof(line), &p,
                    acct_totp_on("admin") ? L("on", "オン")
                                          : L("off", "オフ"));
        } else if (i <= acct_user_count) {
            const char *nm = acct_names[i - 1];
            char role[16];
            buf_cat(line, sizeof(line), &p, nm);
            buf_cat(line, sizeof(line), &p, " (");
            if (users_role(acct_text, acct_text_len, nm, role,
                           sizeof(role)) == 0) {
                buf_cat(line, sizeof(line), &p,
                        strcmp(role, "admin") == 0
                            ? L("admin", "管理者")
                            : L("user", "一般ユーザー"));
            }
            buf_cat(line, sizeof(line), &p, ")");
            buf_cat(line, sizeof(line), &p, L("  TOTP: ", "  TOTP: "));
            buf_cat(line, sizeof(line), &p,
                    acct_totp_on(nm) ? L("on", "オン")
                                     : L("off", "オフ"));
        } else {
            buf_cat(line, sizeof(line), &p,
                    L("[ Add user ]", "[ ユーザーを追加 ]"));
        }
        draw_sel_row(x0,
                     y0 + (uint64_t)((first_row + (i - start)) *
                                     CELL_H),
                     line, i == acct_selected);
    }
    const char *hints;
    if (!shell_current_user_is_admin()) {
        hints = shell_session_is_guest()
                    ? L("Guests cannot manage users.",
                        "ゲストはユーザーを管理できません")
                    : L("Admin only: view only on this page.",
                        "管理者のみ: このページは見るだけです");
    } else {
        hints = L("Enter = add  p = PIN  d = delete  t = TOTP",
                  "Enter = 追加  p = PIN変更  d = 削除  t = TOTP");
    }
    draw_settings_tail(x0, y0, acct_status, hints,
                       L("Tab = Security page",
                         "Tab = セキュリティのページ"));
}

/* ---- 描画: セキュリティページ ---- */
static void draw_settings_security(void) {
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    int rows = text_rows();
    draw_text_small(x0, y0,
                    L("Settings - Security", "設定 - セキュリティ"),
                    acc, col_window_bg());
    /* 項目行 (0..4) と保存行 (5)。 */
    for (int i = 0; i <= 5; i++) {
        char line[112];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p, sec_selected == i ? "> " : "  ");
        if (i == 0) {
            buf_cat(line, sizeof(line), &p,
                    L("External media (USB): ", "外部メディア (USB): "));
            buf_cat(line, sizeof(line), &p,
                    sec_usb ? L("blocked", "禁止") : L("allowed", "許可"));
            buf_cat(line, sizeof(line), &p,
                    L(" (takes effect at next boot)", " (次回起動から有効)"));
        } else if (i == 1) {
            buf_cat(line, sizeof(line), &p,
                    L("PIN minimum length: ", "PINの最小長: "));
            buf_cat_uint(line, sizeof(line), &p, (unsigned)sec_pin_min);
            buf_cat(line, sizeof(line), &p,
                    L("  (Left/Right)", "  (左右キー)"));
        } else if (i == 2) {
            buf_cat(line, sizeof(line), &p,
                    L("PIN requires a digit: ", "PINに数字を必須: "));
            buf_cat(line, sizeof(line), &p,
                    sec_pin_digit ? L("on", "オン") : L("off", "オフ"));
        } else if (i == 3) {
            buf_cat(line, sizeof(line), &p,
                    L("Lockout attempts: ", "ロックアウトまでの回数: "));
            buf_cat_uint(line, sizeof(line), &p,
                         (unsigned)sec_lock_attempts);
            buf_cat(line, sizeof(line), &p,
                    L("  (Left/Right)", "  (左右キー)"));
        } else if (i == 4) {
            buf_cat(line, sizeof(line), &p,
                    L("Lockout seconds: ", "ロックアウト秒数: "));
            buf_cat_uint(line, sizeof(line), &p,
                         (unsigned)sec_lock_seconds);
            buf_cat(line, sizeof(line), &p,
                    L("  (Left/Right)", "  (左右キー)"));
        } else {
            buf_cat(line, sizeof(line), &p,
                    L("[ Save security settings ]",
                      "[ セキュリティ設定を保存 ]"));
            if (sec_dirty) {
                buf_cat(line, sizeof(line), &p, " *");
            }
        }
        draw_sel_row(x0, y0 + (uint64_t)((2 + i) * CELL_H), line,
                     sec_selected == i);
    }
    /* 許可リスト。 */
    int list_row = 9;
    draw_text_small(x0, y0 + (uint64_t)(list_row * CELL_H),
                    L("App allowlist (Space = allow/deny):",
                      "アプリ許可リスト (スペース = 許可/拒否):"),
                    dim, col_window_bg());
    int visible = rows - (list_row + 1) - 4;
    if (visible < 1) {
        visible = 1;
    }
    int app_sel = sec_selected - 6;
    int start = 0;
    if (app_sel >= visible) {
        start = app_sel - visible + 1;
    }
    for (int i = start; i < SEC_APP_COUNT && i < start + visible;
         i++) {
        char line[96];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p,
                sec_selected == 6 + i ? "> " : "  ");
        buf_cat(line, sizeof(line), &p, sec_app_name(i));
        buf_cat(line, sizeof(line), &p, ": ");
        buf_cat(line, sizeof(line), &p,
                desk_allowed[i] ? L("allowed", "許可")
                                : L("denied", "拒否"));
        if (i == 0 || i == 1 || i == 2) {
            buf_cat(line, sizeof(line), &p,
                    L(" (cannot be denied)", " (拒否できません)"));
        }
        draw_sel_row(x0,
                     y0 + (uint64_t)((list_row + 1 + (i - start)) *
                                     CELL_H),
                     line, sec_selected == 6 + i);
    }
    const char *hints = shell_current_user_is_admin()
                            ? L("Up/Down = select  Left/Right = change",
                                "上下 = 選択  左右 = 変更")
                            : L("Admin only: view only on this page.",
                                "管理者のみ: このページは見るだけです");
    draw_settings_tail(x0, y0, sec_status, hints,
                       L("Tab = Updates page", "Tab = 更新のページ"));
}

/* ---- 描画: 更新ページ ---- */
static void draw_settings_updates(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Settings - Updates", "設定 - 更新"),
                    acc, bg);
    {
        char line[160];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p,
                L("Current version: ", "現在のバージョン: "));
        buf_cat(line, sizeof(line), &p, PRISM_VERSION);
        draw_text_small(x0, y0 + (uint64_t)CELL_H, line, fg, bg);
        p = 0;
        buf_cat(line, sizeof(line), &p, L("Server: ", "サーバー: "));
        if (desk_settings.update_host[0] == '\0') {
            buf_cat(line, sizeof(line), &p,
                    L("(not configured)", "(未設定)"));
        } else {
            buf_cat(line, sizeof(line), &p,
                    desk_settings.update_host);
            buf_cat(line, sizeof(line), &p, " ");
            buf_cat(line, sizeof(line), &p,
                    desk_settings.update_path);
        }
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), line, fg,
                        bg);
        p = 0;
        buf_cat(line, sizeof(line), &p,
                L("Update state: ", "更新の状態: "));
        size_t slen = 0;
        const char *st = vfs_read_ptr("update.state", &slen);
        if (st != NULL && slen >= 8 &&
            memcmp(st, "applying", 8) == 0) {
            buf_cat(line, sizeof(line), &p,
                    L("interrupted (previous update did not finish)",
                      "中断 (前回の更新が終わっていません)"));
        } else {
            buf_cat(line, sizeof(line), &p, L("idle", "通常"));
        }
        draw_text_small(x0, y0 + (uint64_t)(3 * CELL_H), line, dim,
                        bg);
    }
    /* 操作行 (0..4)。 */
    for (int i = 0; i <= 4; i++) {
        char line[112];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p, upd_selected == i ? "> " : "  ");
        if (i == 0) {
            buf_cat(line, sizeof(line), &p,
                    L("[ Check for updates ]", "[ 更新を確認 ]"));
        } else if (i == 1) {
            buf_cat(line, sizeof(line), &p,
                    L("Auto security updates: ",
                      "セキュリティ更新の自動適用: "));
            buf_cat(line, sizeof(line), &p,
                    desk_settings.auto_security ? L("on", "オン")
                                                : L("off", "オフ"));
        } else if (i == 2) {
            buf_cat(line, sizeof(line), &p,
                    L("Channel: ", "チャンネル: "));
            buf_cat(line, sizeof(line), &p,
                    desk_settings.update_channel
                        ? L("beta", "ベータ")
                        : L("stable", "安定版"));
        } else if (i == 3) {
            buf_cat(line, sizeof(line), &p,
                    L("[ Apply update ]", "[ 更新を適用 ]"));
            if (upd_check_rc == SHELL_UPDATE_AVAILABLE) {
                buf_cat(line, sizeof(line), &p, " (");
                buf_cat(line, sizeof(line), &p, upd_info.version);
                buf_cat(line, sizeof(line), &p, ")");
            }
        } else {
            buf_cat(line, sizeof(line), &p,
                    L("[ Roll back ]", "[ 巻き戻し ]"));
        }
        draw_sel_row(x0, y0 + (uint64_t)((5 + i) * CELL_H), line,
                     upd_selected == i);
    }
    /* 確認結果の詳細。 */
    int row = 11;
    if (upd_check_rc == SHELL_UPDATE_AVAILABLE) {
        char line[128];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p,
                L("Update available: ", "更新があります: "));
        buf_cat(line, sizeof(line), &p, upd_info.version);
        buf_cat(line, sizeof(line), &p, " (");
        buf_cat(line, sizeof(line), &p, upd_info.type);
        buf_cat(line, sizeof(line), &p, ", ");
        buf_cat(line, sizeof(line), &p, upd_info.channel);
        buf_cat(line, sizeof(line), &p, ")");
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), line, fg,
                        bg);
        row++;
        if (upd_info.has_kernel) {
            draw_text_small(
                x0, y0 + (uint64_t)(row * CELL_H),
                L("This update includes a kernel update (reboot after applying).",
                  "カーネルの更新を含みます (適用後に再起動)"),
                dim, bg);
            row++;
        }
    }
    /* 復元ポイント。 */
    draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                    L("Restore points:", "復元ポイント:"), dim, bg);
    row++;
    if (upd_snap_count == 0) {
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                        L("(none)", "(なし)"), fg, bg);
        row++;
    } else {
        for (int i = 0; i < upd_snap_count; i++) {
            draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                            upd_snaps[i], fg, bg);
            row++;
        }
    }
    const char *hints = L("Up/Down = select  Enter = run",
                          "上下 = 選択  Enter = 実行");
    draw_settings_tail(x0, y0, upd_status, hints,
                       L("Tab = Appearance page", "Tab = 見た目のページ"));
}

/* ---- 時計ページ (日付/時刻/タイムゾーン/NTP) ----
 * 保存先はシェルの clock_flow と同一: 時刻は RTC、
 * タイムゾーンは settings.txt の tz_offset。 */
static void inst_copy(char *dst, size_t cap, const char *src);

static int clk_row; /* 0=日時, 1=タイムゾーン, 2=NTP */
static char clk_buf[24];
static int clk_len;
static char clk_status[96];

static void clk_fmt2(char *out, int v) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = '\0';
}

static void clk_refresh(void) {
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        clk_buf[0] = '\0';
        clk_len = 0;
        return;
    }
    int year = 2000 + t.year;
    char *p = clk_buf;
    *p++ = (char)('0' + (year / 1000) % 10);
    *p++ = (char)('0' + (year / 100) % 10);
    *p++ = (char)('0' + (year / 10) % 10);
    *p++ = (char)('0' + year % 10);
    *p++ = '-';
    clk_fmt2(p, t.month);
    p += 2;
    *p++ = '-';
    clk_fmt2(p, t.day);
    p += 2;
    *p++ = ' ';
    clk_fmt2(p, t.hour);
    p += 2;
    *p++ = ':';
    clk_fmt2(p, t.minute);
    p += 2;
    *p++ = ':';
    clk_fmt2(p, t.second);
    p += 2;
    *p = '\0';
    clk_len = (int)(p - clk_buf);
}

static int clk_parse2(const char *p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') {
        return -1;
    }
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static void clk_apply(void) {
    const char *p = clk_buf;
    if (clk_len != 19 || p[4] != '-' || p[7] != '-' ||
        p[10] != ' ' || p[13] != ':' || p[16] != ':') {
        inst_copy(clk_status, sizeof(clk_status),
                  L("Use YYYY-MM-DD HH:MM:SS format.",
                    "YYYY-MM-DD HH:MM:SS の形式で入力してください"));
        desktop_redraw();
        return;
    }
    int year = 0;
    for (int i = 0; i < 4; i++) {
        if (p[i] < '0' || p[i] > '9') {
            year = -1;
            break;
        }
        year = year * 10 + (p[i] - '0');
    }
    int mon = clk_parse2(p + 5);
    int day = clk_parse2(p + 8);
    int hh = clk_parse2(p + 11);
    int mm = clk_parse2(p + 14);
    int ss = clk_parse2(p + 17);
    if (year < 2000 || year > 2099 || mon < 1 || mon > 12 ||
        day < 1 || day > rtc_days_in_month(year, mon) ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 ||
        ss > 59) {
        inst_copy(clk_status, sizeof(clk_status),
                  L("Date/time out of range.", "日時が範囲外です"));
        desktop_redraw();
        return;
    }
    prism_rtc_time t;
    t.year = (uint8_t)(year - 2000);
    t.month = (uint8_t)mon;
    t.day = (uint8_t)day;
    t.hour = (uint8_t)hh;
    t.minute = (uint8_t)mm;
    t.second = (uint8_t)ss;
    t.weekday = (uint8_t)rtc_weekday(year, mon, day);
    if (rtc_write(&t) != 0) {
        inst_copy(clk_status, sizeof(clk_status),
                  L("RTC write failed.", "RTC への書き込みに失敗しました"));
        desktop_redraw();
        return;
    }
    serial_write_line("settings: clock set");
    inst_copy(clk_status, sizeof(clk_status),
              L("Clock set.", "時刻を設定しました"));
    desktop_notify(L("Clock set", "時刻を設定しました"));
    desktop_redraw();
}

static void clk_ntp_sync(void) {
    uint8_t ip[4];
    if (net_resolve("pool.ntp.org", ip) != 0) {
        inst_copy(clk_status, sizeof(clk_status),
                  L("Cannot resolve the NTP server.",
                    "NTP サーバーを解決できません"));
        desktop_redraw();
        return;
    }
    uint8_t req[48];
    for (int i = 0; i < 48; i++) {
        req[i] = 0;
    }
    req[0] = 0x23;
    uint8_t resp[48];
    int n = net_udp_exchange(ip, 123, 49123, req, sizeof(req),
                             resp, sizeof(resp), 5);
    if (n < 48) {
        inst_copy(clk_status, sizeof(clk_status),
                  L("No reply from the NTP server.",
                    "NTP サーバーから応答がありません"));
        desktop_redraw();
        return;
    }
    uint32_t secs = ((uint32_t)resp[40] << 24) |
                    ((uint32_t)resp[41] << 16) |
                    ((uint32_t)resp[42] << 8) |
                    (uint32_t)resp[43];
    prism_rtc_time t;
    if (rtc_from_ntp(secs, desk_settings.tz_offset, &t) != 0 ||
        rtc_write(&t) != 0) {
        inst_copy(clk_status, sizeof(clk_status),
                  L("NTP sync failed.", "NTP 同期に失敗しました"));
        desktop_redraw();
        return;
    }
    serial_write_line("settings: ntp synced");
    inst_copy(clk_status, sizeof(clk_status),
              L("Clock synced with NTP.", "NTP で時刻を同期しました"));
    clk_refresh();
    desktop_redraw();
}

static int clock_key(int c) {
    if (c == DESK_KEY_UP) {
        if (clk_row > 0) {
            clk_row--;
        }
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_DOWN) {
        if (clk_row < 2) {
            clk_row++;
        }
        desktop_redraw();
        return 1;
    }
    if (clk_row == 1 &&
        (c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT)) {
        int tz = desk_settings.tz_offset +
                 (c == DESK_KEY_RIGHT ? 30 : -30);
        if (tz < -720) {
            tz = -720;
        }
        if (tz > 840) {
            tz = 840;
        }
        desk_settings.tz_offset = tz;
        if (save_shared_settings() == 0) {
            serial_write_line("settings: timezone set");
            inst_copy(clk_status, sizeof(clk_status),
                      L("Timezone saved.", "タイムゾーンを保存しました"));
        } else {
            inst_copy(clk_status, sizeof(clk_status),
                      L("Could not save.", "保存できませんでした"));
        }
        desktop_redraw();
        return 1;
    }
    if (clk_row == 0) {
        if (c == '\n' || c == '\r') {
            /* 時計設定は管理者 PIN 確認を必須にする
             * (シェルの date set と同じゲート。深層チェック
             * L6 の統一)。 */
            pin_dialog_begin(PINOP_CLK_SET);
            desktop_redraw();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (clk_len > 0) {
                clk_buf[--clk_len] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        if (c >= '0' && c <= '9') {
            /* 区切り文字は自動で入る位置に挿入する。 */
            if (clk_len == 4 || clk_len == 7) {
                if (clk_len + 1 < (int)sizeof(clk_buf)) {
                    clk_buf[clk_len++] = clk_len == 4 ? '-' : '-';
                    clk_buf[clk_len] = '\0';
                }
            } else if (clk_len == 10) {
                if (clk_len + 1 < (int)sizeof(clk_buf)) {
                    clk_buf[clk_len++] = ' ';
                    clk_buf[clk_len] = '\0';
                }
            } else if (clk_len == 13 || clk_len == 16) {
                if (clk_len + 1 < (int)sizeof(clk_buf)) {
                    clk_buf[clk_len++] = ':';
                    clk_buf[clk_len] = '\0';
                }
            }
            if (clk_len + 1 < (int)sizeof(clk_buf)) {
                clk_buf[clk_len++] = (char)c;
                clk_buf[clk_len] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    if (clk_row == 2 && (c == '\n' || c == '\r')) {
        pin_dialog_begin(PINOP_CLK_NTP);
        desktop_redraw();
        return 1;
    }
    return 1;
}

static void draw_settings_clock(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Settings - Clock", "設定 - 時計"),
                    acc, bg);
    /* 現在時刻 (RTC の生値)。 */
    {
        prism_rtc_time t;
        char line[64];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p,
                L("Now (RTC): ", "現在 (RTC): "));
        if (rtc_read(&t) == 0) {
            char tmp[24];
            size_t q = 0;
            int year = 2000 + t.year;
            tmp[q++] = (char)('0' + (year / 1000) % 10);
            tmp[q++] = (char)('0' + (year / 100) % 10);
            tmp[q++] = (char)('0' + (year / 10) % 10);
            tmp[q++] = (char)('0' + year % 10);
            tmp[q++] = '-';
            clk_fmt2(tmp + q, t.month);
            q += 2;
            tmp[q++] = '-';
            clk_fmt2(tmp + q, t.day);
            q += 2;
            tmp[q++] = ' ';
            clk_fmt2(tmp + q, t.hour);
            q += 2;
            tmp[q++] = ':';
            clk_fmt2(tmp + q, t.minute);
            q += 2;
            tmp[q++] = ':';
            clk_fmt2(tmp + q, t.second);
            q += 2;
            tmp[q] = '\0';
            buf_cat(line, sizeof(line), &p, tmp);
        }
        draw_text_small(x0, y0 + (uint64_t)(1 * CELL_H), line, fg,
                        bg);
    }
    /* 日時入力行。 */
    draw_text_small(x0, y0 + (uint64_t)(3 * CELL_H),
                    L("Date/time:", "日時:"),
                    clk_row == 0 ? acc : dim, bg);
    {
        char line[32];
        size_t p = 0;
        while (clk_buf[p] != '\0' && p + 1 < sizeof(line)) {
            line[p] = clk_buf[p];
            p++;
        }
        if (clk_row == 0 && p + 1 < sizeof(line)) {
            line[p++] = '_';
        }
        line[p] = '\0';
        draw_text_small(x0 + 220, y0 + (uint64_t)(3 * CELL_H),
                        line, fg, bg);
    }
    draw_text_small(x0, y0 + (uint64_t)(4 * CELL_H),
                    L("Enter = set the clock", "Enter = 時刻を設定"),
                    dim, bg);
    /* タイムゾーン行。 */
    {
        char line[64];
        size_t p = 0;
        buf_cat(line, sizeof(line), &p, "UTC");
        int tz = desk_settings.tz_offset;
        line[p++] = tz < 0 ? '-' : '+';
        if (tz < 0) {
            tz = -tz;
        }
        clk_fmt2(line + p, tz / 60);
        p += 2;
        line[p++] = ':';
        clk_fmt2(line + p, tz % 60);
        p += 2;
        line[p] = '\0';
        draw_text_small(x0, y0 + (uint64_t)(6 * CELL_H),
                        L("Timezone:", "タイムゾーン:"),
                        clk_row == 1 ? acc : dim, bg);
        draw_text_small(x0 + 220, y0 + (uint64_t)(6 * CELL_H),
                        line, fg, bg);
        draw_text_small(x0, y0 + (uint64_t)(7 * CELL_H),
                        L("Left/Right = change by 30 min (saved)",
                          "左右 = 30分ずつ変更 (自動保存)"),
                        dim, bg);
    }
    if (clk_row == 2) {
        draw_text_small(x0, y0 + (uint64_t)(9 * CELL_H),
                        L("[ Sync with NTP ]", "[ NTP で同期 ]"),
                        rgb(0xFFFFFF), acc);
    } else {
        draw_text_small(x0, y0 + (uint64_t)(9 * CELL_H),
                        L("[ Sync with NTP ]", "[ NTP で同期 ]"),
                        fg, bg);
    }
    const char *hints = L("Up/Down = select  Enter = run",
                          "上下 = 選択  Enter = 実行");
    draw_settings_tail(x0, y0, clk_status, hints,
                       L("Tab = Appearance page", "Tab = 見た目のページ"));
}

/* ---- 設定: ネットワークページ ----
 * 固定 IP / DNS の手動設定。netstack には設定が無かった
 * ので、設定値 (settings.txt) + netstack 側の登録・適用
 * (net_set_static_config) を含めて新設した。編集は
 * 管理者のみ + PIN 確認 (他のシステム系ページと同じ)。 */
static int net_row; /* 0=mode 1=ip 2=mask 3=gw 4=dns 5=apply
                      * 6=天気フィード 7=ニュースフィード */
static char net_fields[4][16];
static char net_feed_fields[2][128];
static char net_page_status[96];
static int net_mode_static;
/* PIN 確認待ちのフィード行 (0 = 天気、1 = ニュース)。 */
static int net_feed_pending;

static void net_refresh(void) {
    settings_refresh_shared();
    net_mode_static = desk_settings.net_static;
    const char *vals[4] = {desk_settings.net_ip,
                           desk_settings.net_mask,
                           desk_settings.net_gw,
                           desk_settings.net_dns};
    for (int f = 0; f < 4; f++) {
        size_t j = 0;
        while (vals[f][j] != '\0' && j + 1 < sizeof(net_fields[f])) {
            net_fields[f][j] = vals[f][j];
            j++;
        }
        net_fields[f][j] = '\0';
    }
    {
        const char *fvals[2] = {desk_settings.feed_weather,
                                desk_settings.feed_news};
        for (int f = 0; f < 2; f++) {
            size_t j = 0;
            while (fvals[f][j] != '\0' &&
                   j + 1 < sizeof(net_feed_fields[f])) {
                net_feed_fields[f][j] = fvals[f][j];
                j++;
            }
            net_feed_fields[f][j] = '\0';
        }
    }
    net_page_status[0] = '\0';
}

static void net_set_status(const char *en, const char *ja) {
    const char *s = L(en, ja);
    size_t p = 0;
    while (*s != '\0' && p + 1 < sizeof(net_page_status)) {
        net_page_status[p++] = *s++;
    }
    net_page_status[p] = '\0';
}

/* PIN 確認後に呼ばれる実際の適用。 */
static int net_do_apply(void) {
    if (!net_mode_static) {
        desk_settings.net_static = 0;
        settings_autosave();
        net_clear_static_config();
        serial_write_line("settings: network dhcp");
        net_set_status("Saved (DHCP)",
                       "保存しました (DHCP)");
        return 0;
    }
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    uint8_t dns[4];
    if (net_parse_ipv4(net_fields[0], ip) != 0 ||
        net_parse_ipv4(net_fields[1], mask) != 0 ||
        net_parse_ipv4(net_fields[2], gw) != 0 ||
        net_parse_ipv4(net_fields[3], dns) != 0) {
        net_set_status("Invalid address",
                       "アドレスの形式が正しくありません");
        return -1;
    }
    desk_settings.net_static = 1;
    const uint8_t *oct[4] = {ip, mask, gw, dns};
    char *dsts[4] = {desk_settings.net_ip, desk_settings.net_mask,
                     desk_settings.net_gw, desk_settings.net_dns};
    for (int f = 0; f < 4; f++) {
        /* "a.b.c.d" 形式に戻して保存する。 */
        char *d = dsts[f];
        size_t p = 0;
        for (int part = 0; part < 4; part++) {
            int v = oct[f][part];
            if (v >= 100) {
                d[p++] = (char)('0' + v / 100);
            }
            if (v >= 10) {
                d[p++] = (char)('0' + (v / 10) % 10);
            }
            d[p++] = (char)('0' + v % 10);
            if (part < 3) {
                d[p++] = '.';
            }
        }
        d[p] = '\0';
    }
    settings_autosave();
    net_set_static_config(ip, mask, gw, dns);
    serial_write_line("settings: network static applied");
    net_set_status("Saved (static IP)",
                   "保存しました (固定IP)");
    return 0;
}

/* PIN 確認後に呼ばれるフィード URL の保存。以前は Enter で
 * 即保存されており、同じページの固定 IP 適用 (PIN 必須) と
 * ゲートが揃っていなかった (監査 L2)。 */
static void net_do_feed_save(void) {
    char *dst = net_feed_pending == 0 ? desk_settings.feed_weather
                                      : desk_settings.feed_news;
    const char *buf = net_feed_fields[net_feed_pending];
    size_t j = 0;
    while (buf[j] != '\0' && j + 1 < 128) {
        dst[j] = buf[j];
        j++;
    }
    dst[j] = '\0';
    settings_autosave();
    serial_write_line("settings: feed url saved");
    net_set_status("Feed URL saved",
                   "フィードURLを保存しました");
    desktop_redraw();
}

static int net_key(int c) {
    if (!shell_current_user_is_admin()) {
        if (c == DESK_KEY_UP || c == DESK_KEY_DOWN) {
            net_row = (net_row + (c == DESK_KEY_DOWN ? 1 : 7)) % 8;
            desktop_redraw();
        }
        return 1;
    }
    if (c == DESK_KEY_UP) {
        net_row = (net_row + 7) % 8;
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_DOWN) {
        net_row = (net_row + 1) % 8;
        desktop_redraw();
        return 1;
    }
    if (net_row >= 6) {
        /* フィード URL の編集 (Enter で保存)。 */
        char *buf = net_feed_fields[net_row - 6];
        size_t len = 0;
        while (buf[len] != '\0') {
            len++;
        }
        if (c >= 32 && c <= 126) {
            if (len + 1 < sizeof(net_feed_fields[0])) {
                buf[len] = (char)c;
                buf[len + 1] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                buf[len - 1] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            /* 保存は PIN 確認後 (net_do_feed_save)。 */
            net_feed_pending = net_row - 6;
            pin_dialog_begin(PINOP_NET_FEED);
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    if (net_row == 0 && (c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT ||
                         c == ' ' || c == '\n' || c == '\r')) {
        net_mode_static = !net_mode_static;
        desktop_redraw();
        return 1;
    }
    if (net_row >= 1 && net_row <= 4) {
        char *buf = net_fields[net_row - 1];
        size_t len = 0;
        while (buf[len] != '\0') {
            len++;
        }
        if ((c >= '0' && c <= '9') || c == '.') {
            if (len + 1 < sizeof(net_fields[0])) {
                buf[len] = (char)c;
                buf[len + 1] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                buf[len - 1] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            net_row++;
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    if (net_row == 5 && (c == '\n' || c == '\r' || c == ' ')) {
        if (shell_session_is_guest()) {
            net_set_status("Admin only", "管理者のみ変更できます");
            desktop_redraw();
            return 1;
        }
        pin_dialog_begin(PINOP_NET_APPLY);
        desktop_redraw();
        return 1;
    }
    return 1;
}

static void draw_settings_network(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Settings - Network",
                              "設定 - ネットワーク"),
                    acc, bg);
    char cur[96];
    net_status(cur, sizeof(cur));
    draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), cur,
                    col_dim(), bg);
    const char *labels[6] = {
        L("Mode:", "方式:"),
        L("IP address:", "IPアドレス:"),
        L("Subnet mask:", "サブネットマスク:"),
        L("Gateway:", "ゲートウェイ:"),
        L("DNS server:", "DNSサーバ:"),
        L("Apply", "適用する"),
    };
    for (int r = 0; r < 6; r++) {
        char line[96];
        size_t p = 0;
        const char *lab = labels[r];
        while (*lab != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *lab++;
        }
        while (p < 22 && p + 1 < sizeof(line)) {
            line[p++] = ' ';
        }
        if (r == 0) {
            const char *v = net_mode_static
                ? L("Static IP", "固定IP")
                : L("DHCP (automatic)", "DHCP (自動)");
            while (*v != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *v++;
            }
        } else if (r >= 1 && r <= 4) {
            const char *v = net_fields[r - 1];
            while (*v != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *v++;
            }
            if (net_row == r) {
                if (p + 1 < sizeof(line)) {
                    line[p++] = '_';
                }
            }
        }
        line[p] = '\0';
        uint64_t py = y0 + (uint64_t)((4 + r) * CELL_H);
        if (r == net_row) {
            fb_fill_rect(x0, py, win_w() - 16, CELL_H, acc);
            fb_draw_text(x0 + 4, py, line, rgb(0xFFFFFF), acc, 1);
        } else {
            fb_draw_text(x0 + 4, py, line, fg, bg, 1);
        }
    }
    {
        /* ウィジェットのフィード URL (項目21)。 */
        const char *flabels[2] = {
            L("Weather feed:", "天気フィード:"),
            L("News feed:", "ニュースフィード:"),
        };
        for (int r = 0; r < 2; r++) {
            char line[192];
            size_t p = 0;
            const char *lab = flabels[r];
            while (*lab != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *lab++;
            }
            while (p < 22 && p + 1 < sizeof(line)) {
                line[p++] = ' ';
            }
            const char *v = net_feed_fields[r];
            while (*v != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *v++;
            }
            if (net_row == 6 + r) {
                if (p + 1 < sizeof(line)) {
                    line[p++] = '_';
                }
            }
            line[p] = '\0';
            uint64_t py = y0 + (uint64_t)((11 + r) * CELL_H);
            if (net_row == 6 + r) {
                fb_fill_rect(x0, py, win_w() - 16, CELL_H, acc);
                fb_draw_text(x0 + 4, py, line, rgb(0xFFFFFF), acc,
                             1);
            } else {
                fb_draw_text(x0 + 4, py, line, fg, bg, 1);
            }
        }
    }
    if (!shell_current_user_is_admin()) {
        draw_text_small(x0, y0 + (uint64_t)(14 * CELL_H),
                        L("Admin only", "管理者のみ変更できます"),
                        acc, bg);
    }
    const char *hints = L("Up/Down = select  Left/Right = mode  Enter = apply",
                          "上下 = 選択  左右 = 方式  Enter = 適用");
    draw_settings_tail(x0, y0, net_page_status, hints,
                       L("Tab = next page", "Tab = 次のページ"));
}

/* ---- 設定ページ: 管理 (バッチ4 項目22) ----
 * FDE / スナップショット / 監査ログ / ドライバ /
 * ファイアウォールを 1 ページにまとめ、セクションを
 * 左右キーで切り替える。裏側はシェルの各フローと
 * 同じ内部処理を使い、変更系はすべて管理者 +
 * PIN 確認ダイアログを通す。 */
static int adm_section; /* 0=FDE 1=Snap 2=Audit 3=Drv 4=FW */
static int adm_selected;
static char adm_status[160];
static int adm_form; /* 0=なし 1=FDE有効化 2=FDE無効化 */
static int adm_field;
static char adm_pass1[64];
static int adm_pass1_len;
static char adm_pass2[64];
static int adm_pass2_len;
static char adm_recovery[32];
static diskfs_snapshot_info adm_snaps[DISKFS_SNAP_SLOTS];
static int adm_snap_count;
static int adm_audit_scroll;
static char adm_drv_rows[16][40];
static int adm_drv_count;
static char adm_fw_ip[16];
static int adm_fw_len;
static int adm_fw_target; /* 削除対象ルールの番号 */

static void adm_set_status(const char *en, const char *ja) {
    const char *s = L(en, ja);
    size_t p = 0;
    while (s[p] != '\0' && p + 1 < sizeof(adm_status)) {
        adm_status[p] = s[p];
        p++;
    }
    adm_status[p] = '\0';
}

static void adm_refresh(void) {
    adm_snap_count = 0;
    if (persist_snapshot_list(adm_snaps, DISKFS_SNAP_SLOTS,
                              &adm_snap_count) != 0) {
        adm_snap_count = 0;
    }
    /* ドライバ行 = 読み込み済み ∪ 無効化リスト。 */
    adm_drv_count = 0;
    for (int i = 0; i < drv_loaded_count() && adm_drv_count < 16;
         i++) {
        const char *n = drv_loaded_name(i);
        if (n == NULL) {
            continue;
        }
        size_t j = 0;
        while (n[j] != '\0' && j + 1 < sizeof(adm_drv_rows[0])) {
            adm_drv_rows[adm_drv_count][j] = n[j];
            j++;
        }
        adm_drv_rows[adm_drv_count][j] = '\0';
        adm_drv_count++;
    }
    size_t len = 0;
    const char *dis = vfs_read_ptr("drivers.disabled", &len);
    if (dis != NULL) {
        size_t i = 0;
        while (i < len && adm_drv_count < 16) {
            size_t j = i;
            while (j < len && dis[j] != '\n') {
                j++;
            }
            if (j > i) {
                size_t nl = j - i;
                if (nl > 39) {
                    nl = 39;
                }
                char name[40];
                for (size_t k = 0; k < nl; k++) {
                    name[k] = dis[i + k];
                }
                name[nl] = '\0';
                int dup = 0;
                for (int r = 0; r < adm_drv_count; r++) {
                    if (strcmp(adm_drv_rows[r], name) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    inst_copy(adm_drv_rows[adm_drv_count],
                              sizeof(adm_drv_rows[0]), name);
                    adm_drv_count++;
                }
            }
            i = j + 1;
        }
    }
    if (adm_selected < 0) {
        adm_selected = 0;
    }
}

static int adm_row_count(void) {
    switch (adm_section) {
        case 0:
            return 1; /* FDE のアクション行 */
        case 1:
            return adm_snap_count + 1; /* + 作成行 */
        case 2:
            return 0; /* 監査ログはスクロールのみ */
        case 3:
            return adm_drv_count;
        default:
            return net_fw_count() + 2; /* + 追加行 + 全消去行 */
    }
}

static void adm_do_fde_enable(void) {
    if (!persist_installed()) {
        adm_set_status("Not installed on disk",
                       "ディスクにインストールされていません");
        return;
    }
    if (persist_fde_present()) {
        adm_set_status("Already enabled", "すでに有効です");
        return;
    }
    if (!persist_key_ready()) {
        adm_set_status("Log in first (disk key inactive)",
                       "先にログインしてください (鍵が無効)");
        return;
    }
    char recovery[25];
    int rc = persist_fde_enable(adm_pass1, recovery,
                                sizeof(recovery));
    for (int i = 0; i < 64; i++) {
        adm_pass1[i] = 0;
        adm_pass2[i] = 0;
    }
    adm_pass1_len = 0;
    adm_pass2_len = 0;
    adm_form = 0;
    if (rc != 0) {
        adm_set_status("Could not enable encryption",
                       "暗号化を有効にできませんでした");
        return;
    }
    inst_copy(adm_recovery, sizeof(adm_recovery), recovery);
    for (int i = 0; i < 25; i++) {
        recovery[i] = 0;
    }
    serial_write_line("settings: fde enabled");
    audit_log_event("fde-enable",
                    "full-disk encryption enabled (settings)");
    adm_set_status("Encryption enabled. Recovery key shown "
                   "below - write it down.",
                   "暗号化を有効にしました。下の回復キーを"
                   "書き留めてください");
}

static void adm_do_fde_disable(void) {
    int ok = persist_fde_verify(adm_pass1);
    for (int i = 0; i < 64; i++) {
        adm_pass1[i] = 0;
    }
    adm_pass1_len = 0;
    adm_form = 0;
    if (!ok) {
        adm_set_status("Wrong passphrase", "パスフレーズが違います");
        return;
    }
    if (persist_fde_disable() != 0) {
        adm_set_status("Could not remove encryption",
                       "暗号化を解除できませんでした");
        return;
    }
    adm_recovery[0] = '\0';
    serial_write_line("settings: fde disabled");
    audit_log_event("fde-disable",
                    "full-disk encryption removed (settings)");
    adm_set_status("Encryption removed", "暗号化を解除しました");
}

static void adm_do_snap_restore(void) {
    if (adm_selected < 0 || adm_selected >= adm_snap_count) {
        return;
    }
    const char *name = adm_snaps[adm_selected].name;
    int rc = persist_snapshot_restore(name);
    if (rc == 0) {
        persist_load();
        persist_decrypt_loaded();
        serial_write("settings: restored snapshot ");
        serial_write_line(name);
        adm_set_status("Restored", "復元しました");
    } else if (rc == -1) {
        adm_set_status("No such restore point",
                       "復元ポイントがありません");
    } else {
        adm_set_status("Restore failed (broken snapshot)",
                       "復元に失敗しました (壊れています)");
    }
    adm_refresh();
}

static void adm_do_snap_delete(void) {
    if (adm_selected < 0 || adm_selected >= adm_snap_count) {
        return;
    }
    const char *name = adm_snaps[adm_selected].name;
    if (persist_snapshot_delete(name) == 0) {
        serial_write("settings: snapshot deleted ");
        serial_write_line(name);
        adm_set_status("Deleted", "削除しました");
    } else {
        adm_set_status("No such restore point",
                       "復元ポイントがありません");
    }
    adm_refresh();
    if (adm_selected >= adm_row_count()) {
        adm_selected = adm_row_count() > 0 ? adm_row_count() - 1
                                           : 0;
    }
}

static void adm_do_drv_toggle(void) {
    if (adm_selected < 0 || adm_selected >= adm_drv_count) {
        return;
    }
    const char *name = adm_drv_rows[adm_selected];
    int dis = shell_driver_name_disabled(name);
    shell_driver_set_disabled(name, !dis);
    persist_maybe_sync();
    serial_write("settings: driver ");
    serial_write(name);
    serial_write_line(dis ? " enabled" : " disabled");
    adm_set_status(dis ? "Driver enabled (installs allowed)"
                       : "Driver disabled (installs refused)",
                   dis ? "ドライバを有効にしました (導入可)"
                       : "ドライバを無効にしました (導入拒否)");
    adm_refresh();
}

static void adm_do_fw_add(void) {
    adm_fw_ip[adm_fw_len] = '\0';
    uint8_t ip[4];
    if (net_parse_ipv4(adm_fw_ip, ip) != 0) {
        adm_set_status("Invalid IP address",
                       "IPアドレスが不正です");
        return;
    }
    if (net_fw_add_drop(ip) != 0) {
        adm_set_status("Rule list is full",
                       "ルールが上限に達しています");
        return;
    }
    shell_fw_save();
    serial_write("settings: firewall drop added ");
    serial_write_line(adm_fw_ip);
    adm_fw_len = 0;
    adm_fw_ip[0] = '\0';
    adm_set_status("Rule added", "ルールを追加しました");
}

static void adm_do_fw_remove(void) {
    int n = net_fw_count();
    if (adm_fw_target < 0 || adm_fw_target >= n) {
        return;
    }
    uint8_t keep[16][4]; /* NET_FW_MAX は netstack 内部で 16 */
    int kn = 0;
    for (int i = 0; i < n; i++) {
        uint8_t ip[4];
        if (i != adm_fw_target && net_fw_get(i, ip) == 0) {
            for (int k = 0; k < 4; k++) {
                keep[kn][k] = ip[k];
            }
            kn++;
        }
    }
    net_fw_clear();
    for (int i = 0; i < kn; i++) {
        net_fw_add_drop(keep[i]);
    }
    shell_fw_save();
    serial_write_line("settings: firewall rule removed");
    adm_set_status("Rule removed", "ルールを削除しました");
    if (adm_selected >= adm_row_count()) {
        adm_selected = adm_row_count() > 0 ? adm_row_count() - 1
                                           : 0;
    }
}

static void adm_do_fw_clear(void) {
    net_fw_clear();
    shell_fw_save();
    serial_write_line("settings: firewall cleared");
    adm_set_status("All rules cleared", "全ルールを消去しました");
    adm_selected = 0;
}

/* FDE フォームのキー処理。 */
static int adm_form_key(int c) {
    char *buf = adm_field == 0 ? adm_pass1 : adm_pass2;
    int *blen = adm_field == 0 ? &adm_pass1_len : &adm_pass2_len;
    int max_fields = adm_form == 1 ? 2 : 1;
    if (c == '\t') {
        adm_field = (adm_field + 1) % max_fields;
        desktop_redraw();
        return 1;
    }
    if (c == '\n' || c == '\r') {
        if (adm_field + 1 < max_fields) {
            adm_field++;
            desktop_redraw();
            return 1;
        }
        if (adm_form == 1) {
            /* 有効化: 長さと一致を検証してから PIN 確認へ。 */
            adm_pass1[adm_pass1_len] = '\0';
            adm_pass2[adm_pass2_len] = '\0';
            if (adm_pass1_len < 8) {
                adm_set_status("Passphrase too short (8+)",
                               "パスフレーズが短すぎます (8文字以上)");
                desktop_redraw();
                return 1;
            }
            if (strcmp(adm_pass1, adm_pass2) != 0) {
                adm_set_status("Passphrases do not match",
                               "パスフレーズが一致しません");
                desktop_redraw();
                return 1;
            }
            pin_dialog_begin(PINOP_ADM_FDE_ENABLE);
        } else {
            adm_pass1[adm_pass1_len] = '\0';
            pin_dialog_begin(PINOP_ADM_FDE_DISABLE);
        }
        desktop_redraw();
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        if (*blen > 0) {
            (*blen)--;
            buf[*blen] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    if (c >= 32 && c <= 126 && *blen + 1 < 64) {
        buf[(*blen)++] = (char)c;
        buf[*blen] = '\0';
        desktop_redraw();
        return 1;
    }
    return 1;
}

static int adm_key(int c) {
    if (adm_form) {
        return adm_form_key(c);
    }
    if (!shell_current_user_is_admin()) {
        /* 読み取り専用: セクション移動とスクロールだけ。 */
        if (c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT) {
            adm_section = (adm_section +
                           (c == DESK_KEY_RIGHT ? 1 : 4)) %
                          5;
            adm_selected = 0;
            adm_refresh();
            desktop_redraw();
        } else if (c == DESK_KEY_UP || c == DESK_KEY_DOWN) {
            if (adm_section == 2) {
                adm_audit_scroll += (c == DESK_KEY_DOWN ? 1 : -1);
                if (adm_audit_scroll < 0) {
                    adm_audit_scroll = 0;
                }
            } else {
                int n = adm_row_count();
                if (n > 0) {
                    adm_selected =
                        (adm_selected +
                         (c == DESK_KEY_DOWN ? 1 : n - 1)) %
                        n;
                }
            }
            desktop_redraw();
        }
        return 1;
    }
    if (c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT) {
        adm_section = (adm_section +
                       (c == DESK_KEY_RIGHT ? 1 : 4)) %
                      5;
        adm_selected = 0;
        adm_fw_len = 0;
        adm_fw_ip[0] = '\0';
        adm_refresh();
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_UP || c == DESK_KEY_DOWN) {
        if (adm_section == 2) {
            adm_audit_scroll += (c == DESK_KEY_DOWN ? 1 : -1);
            if (adm_audit_scroll < 0) {
                adm_audit_scroll = 0;
            }
        } else {
            int n = adm_row_count();
            if (n > 0) {
                adm_selected = (adm_selected +
                                (c == DESK_KEY_DOWN ? 1 : n - 1)) %
                               n;
            }
        }
        desktop_redraw();
        return 1;
    }
    if (adm_section == 0 && (c == '\n' || c == '\r')) {
        if (!persist_fde_present()) {
            adm_form = 1;
            adm_field = 0;
            adm_pass1_len = 0;
            adm_pass2_len = 0;
            adm_pass1[0] = '\0';
            adm_pass2[0] = '\0';
            adm_set_status("Enter a new disk passphrase",
                           "新しいディスクパスフレーズを入力");
        } else if (persist_fde_unlocked()) {
            adm_form = 2;
            adm_field = 0;
            adm_pass1_len = 0;
            adm_pass1[0] = '\0';
            adm_set_status("Enter the disk passphrase",
                           "ディスクパスフレーズを入力");
        } else {
            adm_set_status("Disk is locked",
                           "ディスクがロックされています");
        }
        desktop_redraw();
        return 1;
    }
    if (adm_section == 1) {
        if (c == '\n' || c == '\r') {
            if (adm_selected >= adm_snap_count) {
                char name[40];
                shell_snapshot_auto_name(name, sizeof(name));
                if (persist_snapshot_create(name) == 0) {
                    serial_write("settings: snapshot created ");
                    serial_write_line(name);
                    adm_set_status("Restore point created",
                                   "復元ポイントを作成しました");
                    adm_refresh();
                } else {
                    adm_set_status("Failed (no installed disk)",
                                   "失敗 (ディスク未インストール)");
                }
            } else {
                pin_dialog_begin(PINOP_ADM_SNAP_RESTORE);
            }
            desktop_redraw();
            return 1;
        }
        if ((c == 'd' || c == 'D') &&
            adm_selected < adm_snap_count) {
            pin_dialog_begin(PINOP_ADM_SNAP_DELETE);
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    if (adm_section == 3 && (c == '\n' || c == '\r') &&
        adm_drv_count > 0) {
        pin_dialog_begin(PINOP_ADM_DRV_TOGGLE);
        desktop_redraw();
        return 1;
    }
    if (adm_section == 4) {
        int n = net_fw_count();
        if (adm_selected == n) {
            /* 追加行: IP を入力して Enter。 */
            if ((c >= '0' && c <= '9') || c == '.') {
                if (adm_fw_len + 1 < (int)sizeof(adm_fw_ip)) {
                    adm_fw_ip[adm_fw_len++] = (char)c;
                    adm_fw_ip[adm_fw_len] = '\0';
                }
                desktop_redraw();
                return 1;
            }
            if (c == '\b' || c == 0x7F) {
                if (adm_fw_len > 0) {
                    adm_fw_ip[--adm_fw_len] = '\0';
                }
                desktop_redraw();
                return 1;
            }
            if (c == '\n' || c == '\r') {
                if (adm_fw_len > 0) {
                    pin_dialog_begin(PINOP_ADM_FW_ADD);
                }
                desktop_redraw();
                return 1;
            }
            return 1;
        }
        if (c == '\n' || c == '\r') {
            if (adm_selected < n) {
                adm_fw_target = adm_selected;
                pin_dialog_begin(PINOP_ADM_FW_REMOVE);
            } else {
                pin_dialog_begin(PINOP_ADM_FW_CLEAR);
            }
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    return 1;
}

static void adm_line(uint64_t x0, uint64_t y0, int row,
                     const char *text, int selected, uint32_t fg,
                     uint32_t bg, uint32_t acc) {
    uint64_t py = y0 + (uint64_t)(row * CELL_H);
    if (selected) {
        fb_fill_rect(x0, py, win_w() - 16, CELL_H, acc);
        fb_draw_text(x0 + 4, py, text, rgb(0xFFFFFF), acc, 1);
    } else {
        fb_draw_text(x0 + 4, py, text, fg, bg, 1);
    }
}

static void draw_settings_admin(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Settings - Administration",
                              "設定 - 管理"),
                    acc, bg);
    const char *tabs[5] = {L("FDE", "暗号化"),
                           L("Snapshots", "復元"),
                           L("Audit log", "監査ログ"),
                           L("Drivers", "ドライバ"),
                           L("Firewall", "ファイアウォール")};
    char tline[96];
    size_t tp = 0;
    for (int i = 0; i < 5; i++) {
        const char *s = i == adm_section ? "[" : "";
        while (*s != '\0' && tp + 1 < sizeof(tline)) {
            tline[tp++] = *s++;
        }
        s = tabs[i];
        while (*s != '\0' && tp + 1 < sizeof(tline)) {
            tline[tp++] = *s++;
        }
        s = i == adm_section ? "] " : "  ";
        while (*s != '\0' && tp + 1 < sizeof(tline)) {
            tline[tp++] = *s++;
        }
    }
    tline[tp] = '\0';
    draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), tline, fg, bg);
    int row = 4;
    if (adm_form) {
        const char *labels[2] = {
            L("New passphrase:", "新しいパスフレーズ:"),
            L("Confirm:", "確認:")};
        int fields = adm_form == 1 ? 2 : 1;
        if (adm_form == 2) {
            labels[0] = L("Disk passphrase:", "ディスクパスフレーズ:");
        }
        for (int f = 0; f < fields; f++) {
            char line[112];
            size_t p = 0;
            const char *lab = labels[f];
            while (*lab != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *lab++;
            }
            while (p < 24 && p + 1 < sizeof(line)) {
                line[p++] = ' ';
            }
            int n = f == 0 ? adm_pass1_len : adm_pass2_len;
            for (int i = 0; i < n && p + 1 < sizeof(line); i++) {
                line[p++] = '*';
            }
            if (adm_field == f) {
                line[p++] = '_';
            }
            line[p] = '\0';
            adm_line(x0, y0, row, line, adm_field == f, fg, bg, acc);
            row += 2;
        }
        draw_text_small(
            x0, y0 + (uint64_t)(row * CELL_H),
            L("Tab = next field  Enter = continue  Esc = cancel",
              "Tab = 次の欄  Enter = 進む  Esc = 中止"),
            col_dim(), bg);
        row += 2;
    } else if (adm_section == 0) {
        char line[128];
        const char *st;
        if (!persist_disk_present()) {
            st = L("No disk.", "ディスクがありません");
        } else if (!persist_fde_present()) {
            st = L("Full-disk encryption: off",
                   "ディスク全体の暗号化: オフ");
        } else if (persist_fde_unlocked()) {
            st = L("Full-disk encryption: on (unlocked)",
                   "ディスク全体の暗号化: オン (解除済み)");
        } else {
            st = L("Full-disk encryption: on (locked)",
                   "ディスク全体の暗号化: オン (ロック中)");
        }
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), st, fg,
                        bg);
        row++;
        int mig = persist_fde_present()
                      ? persist_fde_migration_pending()
                      : 0;
        if (mig == 1) {
            draw_text_small(
                x0, y0 + (uint64_t)(row * CELL_H),
                L("Encryption setup incomplete; resumes at unlock.",
                  "暗号化の途中です。解除時に再開します"),
                col_dim(), bg);
            row++;
        } else if (mig == 2) {
            draw_text_small(
                x0, y0 + (uint64_t)(row * CELL_H),
                L("Encryption removal incomplete; resumes at "
                  "unlock.",
                  "暗号化解除の途中です。解除時に再開します"),
                col_dim(), bg);
            row++;
        }
        row++;
        const char *act = !persist_fde_present()
            ? L("Enable encryption...", "暗号化を有効にする...")
            : (persist_fde_unlocked()
                   ? L("Disable encryption...",
                       "暗号化を解除する...")
                   : L("(locked)", "(ロック中)"));
        adm_line(x0, y0, row, act, adm_selected == 0, fg, bg, acc);
        row += 2;
        if (adm_recovery[0] != '\0') {
            char rl[64];
            size_t p = 0;
            const char *pre = L("Recovery key: ", "回復キー: ");
            while (*pre != '\0' && p + 1 < sizeof(rl)) {
                rl[p++] = *pre++;
            }
            const char *rk = adm_recovery;
            while (*rk != '\0' && p + 1 < sizeof(rl)) {
                rl[p++] = *rk++;
            }
            rl[p] = '\0';
            draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), rl,
                            acc, bg);
            row++;
        }
    } else if (adm_section == 1) {
        for (int i = 0; i < adm_snap_count; i++) {
            char line[96];
            size_t p = 0;
            const char *nm = adm_snaps[i].name;
            while (*nm != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *nm++;
            }
            line[p++] = ' ';
            line[p++] = ' ';
            const char *ts = adm_snaps[i].timestamp;
            while (*ts != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *ts++;
            }
            line[p] = '\0';
            adm_line(x0, y0, row, line, adm_selected == i, fg, bg,
                     acc);
            row++;
        }
        if (adm_snap_count == 0) {
            draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                            L("(no restore points)",
                              "(復元ポイントなし)"),
                            col_dim(), bg);
            row++;
        }
        adm_line(x0, y0, row,
                 L("Create restore point", "復元ポイントを作成"),
                 adm_selected == adm_snap_count, fg, bg, acc);
        row += 2;
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                        L("Enter = restore / create  D = delete",
                          "Enter = 復元 / 作成  D = 削除"),
                        col_dim(), bg);
        row++;
    } else if (adm_section == 2) {
        if (persist_name_pending_decrypt("audit.log")) {
            draw_text_small(
                x0, y0 + (uint64_t)(row * CELL_H),
                L("Audit log is sealed until login.",
                  "監査ログはログインまで封印されています"),
                fg, bg);
            row += 2;
        } else {
            size_t len = 0;
            const char *log = vfs_read_ptr("audit.log", &len);
            int count = 0;
            int bad = 0;
            int rc = (log != NULL)
                         ? audit_verify(log, len, &count, &bad)
                         : -2;
            char vline[96];
            size_t p = 0;
            const char *pre = L("Chain: ", "チェーン: ");
            while (*pre != '\0' && p + 1 < sizeof(vline)) {
                vline[p++] = *pre++;
            }
            const char *vs = rc == 0 ? L("OK", "正常")
                             : (rc == -1 ? L("BROKEN (tampered?)",
                                             "破損 (改ざん?)")
                                         : L("format broken",
                                             "形式が壊れています"));
            while (*vs != '\0' && p + 1 < sizeof(vline)) {
                vline[p++] = *vs++;
            }
            vline[p] = '\0';
            draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), vline,
                            acc, bg);
            row += 2;
            int total = log != NULL ? audit_count(log, len) : 0;
            int visible = text_rows() - row - 4;
            if (visible < 1) {
                visible = 1;
            }
            if (adm_audit_scroll > total) {
                adm_audit_scroll = total;
            }
            int start = total - visible - adm_audit_scroll;
            if (start < 0) {
                start = 0;
            }
            size_t pos = 0;
            int idx = 0;
            int shown = 0;
            while (pos < len && shown < visible) {
                size_t eol = pos;
                while (eol < len && log[eol] != '\n') {
                    eol++;
                }
                if (idx >= start) {
                    char line[104];
                    size_t lp = 0;
                    for (size_t i = pos; i < eol &&
                                        lp + 1 < sizeof(line);
                         i++) {
                        line[lp++] = log[i];
                    }
                    line[lp] = '\0';
                    draw_text_small(
                        x0, y0 + (uint64_t)(row * CELL_H), line,
                        fg, bg);
                    row++;
                    shown++;
                }
                idx++;
                pos = eol + 1;
            }
            if (total == 0) {
                draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                                L("(no audit entries yet)",
                                  "(監査記録はまだありません)"),
                                col_dim(), bg);
                row++;
            }
        }
    } else if (adm_section == 3) {
        for (int i = 0; i < adm_drv_count; i++) {
            char line[96];
            size_t p = 0;
            const char *nm = adm_drv_rows[i];
            while (*nm != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *nm++;
            }
            line[p++] = ' ';
            const char *st = shell_driver_name_disabled(
                                 adm_drv_rows[i])
                ? L("[disabled]", "[無効]")
                : L("[enabled]", "[有効]");
            while (*st != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *st++;
            }
            line[p] = '\0';
            adm_line(x0, y0, row, line, adm_selected == i, fg, bg,
                     acc);
            row++;
        }
        if (adm_drv_count == 0) {
            draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                            L("(no add-on drivers)",
                              "(追加ドライバなし)"),
                            col_dim(), bg);
            row++;
        }
        row++;
        draw_text_small(
            x0, y0 + (uint64_t)(row * CELL_H),
            L("Enter = enable/disable (installs are refused "
              "while disabled)",
              "Enter = 有効/無効 (無効の間は導入を拒否)"),
            col_dim(), bg);
        row++;
    } else {
        int n = net_fw_count();
        for (int i = 0; i < n; i++) {
            uint8_t ip[4];
            if (net_fw_get(i, ip) != 0) {
                continue;
            }
            char line[48];
            size_t p = 0;
            const char *pre = "drop ";
            while (*pre != '\0') {
                line[p++] = *pre++;
            }
            for (int part = 0; part < 4; part++) {
                if (part > 0) {
                    line[p++] = '.';
                }
                int v = ip[part];
                if (v >= 100) {
                    line[p++] = (char)('0' + v / 100);
                }
                if (v >= 10) {
                    line[p++] = (char)('0' + (v / 10) % 10);
                }
                line[p++] = (char)('0' + v % 10);
            }
            line[p] = '\0';
            adm_line(x0, y0, row, line, adm_selected == i, fg, bg,
                     acc);
            row++;
        }
        if (n == 0) {
            draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                            L("(no rules - all traffic allowed)",
                              "(ルールなし - 全て許可)"),
                            col_dim(), bg);
            row++;
        }
        {
            char line[64];
            size_t p = 0;
            const char *pre = L("Add drop rule: ", "遮断を追加: ");
            while (*pre != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *pre++;
            }
            for (int i = 0; i < adm_fw_len && p + 1 < sizeof(line);
                 i++) {
                line[p++] = adm_fw_ip[i];
            }
            if (adm_selected == n) {
                line[p++] = '_';
            }
            line[p] = '\0';
            adm_line(x0, y0, row, line, adm_selected == n, fg, bg,
                     acc);
            row++;
        }
        adm_line(x0, y0, row, L("Clear all rules", "全ルールを消去"),
                 adm_selected == n + 1, fg, bg, acc);
        row += 2;
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H),
                        L("Enter on a rule = remove it",
                          "ルールで Enter = そのルールを削除"),
                        col_dim(), bg);
        row++;
    }
    if (!shell_current_user_is_admin()) {
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 3) * CELL_H),
                        L("Admin only", "管理者のみ変更できます"),
                        acc, bg);
    }
    const char *hints = L("Left/Right = section  Up/Down = select  Enter = run",
                          "左右 = 分類  上下 = 選択  Enter = 実行");
    draw_settings_tail(x0, y0, adm_status, hints,
                       L("Tab = next page", "Tab = 次のページ"));
}

/* ---- キー処理: アカウントページ ---- */

/* 選択中の行の対象ユーザー名 ("admin" または追加ユーザー名)。 */
static const char *acct_selected_target(void) {
    if (acct_selected <= 0) {
        return "admin";
    }
    if (acct_selected <= acct_user_count) {
        return acct_names[acct_selected - 1];
    }
    return "admin";
}

static void acct_set_target(void) {
    const char *t = acct_selected_target();
    size_t p = 0;
    while (t[p] != '\0' && p + 1 < sizeof(acct_target)) {
        acct_target[p] = t[p];
        p++;
    }
    acct_target[p] = '\0';
}

static int acct_form_key(int c) {
    int nfields = acct_mode == ACCT_MODE_ADD ? 5 : 3;
    if (c == '\t' || c == DESK_KEY_DOWN) {
        acct_field = (acct_field + 1) % nfields;
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_UP) {
        acct_field = (acct_field + nfields - 1) % nfields;
        desktop_redraw();
        return 1;
    }
    if (acct_mode == ACCT_MODE_ADD && acct_field == 3) {
        /* 役割は左右/スペースで切り替える。 */
        if (c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT || c == ' ') {
            acct_role_admin = !acct_role_admin;
            desktop_redraw();
        }
        return 1;
    }
    if (c == '\n' || c == '\r') {
        int button = acct_mode == ACCT_MODE_ADD ? 4 : 2;
        if (acct_field != button) {
            acct_field++;
            desktop_redraw();
            return 1;
        }
        if (acct_mode == ACCT_MODE_ADD) {
            if (!users_name_ok(acct_name)) {
                set_page_status(
                    L("Bad user name (1-16 chars, letters/digits/_-; admin and guest are reserved).",
                      "名前が規則に合いません (1-16文字、英数字と_-; adminとguestは予約済み)"));
            } else if (acct_users_load() == 0 &&
                       users_exists(acct_text, acct_text_len,
                                    acct_name)) {
                set_page_status(L("That user already exists.",
                                  "そのユーザーは既にあります"));
            } else if (!account_pin_ok(acct_pin1) ||
                       strcmp(acct_pin1, acct_pin2) != 0) {
                set_page_status(
                    L("PINs do not match or are not valid (4-16 chars).",
                      "PINが一致しないか、規則に合いません (4-16文字)"));
            } else {
                pin_dialog_begin(PINOP_ACCT_ADD);
            }
        } else {
            int ok;
            if (strcmp(acct_target, "admin") == 0) {
                ok = account_pin_ok_policy(
                    acct_pin1, desk_settings.pin_min_len,
                    desk_settings.pin_require_digit);
                if (!ok) {
                    set_page_status(
                        L("PIN does not meet the policy (length / digit).",
                          "PINがポリシーに合いません (長さ / 数字)"));
                }
            } else {
                ok = account_pin_ok(acct_pin1);
                if (!ok) {
                    set_page_status(
                        L("PIN is not valid (4-16 chars).",
                          "PINが規則に合いません (4-16文字)"));
                }
            }
            if (ok && strcmp(acct_pin1, acct_pin2) != 0) {
                set_page_status(L("PINs do not match.",
                                  "PINが一致しません"));
                ok = 0;
            }
            if (ok) {
                pin_dialog_begin(PINOP_ACCT_PASSWD);
            }
        }
        desktop_redraw();
        return 1;
    }
    /* テキスト入力。 */
    char *buf = NULL;
    int *len = NULL;
    int cap = 16;
    if (acct_mode == ACCT_MODE_ADD) {
        if (acct_field == 0) {
            buf = acct_name;
            len = &acct_name_len;
            cap = PRISM_USER_NAME_MAX;
        } else if (acct_field == 1) {
            buf = acct_pin1;
            len = &acct_pin1_len;
        } else if (acct_field == 2) {
            buf = acct_pin2;
            len = &acct_pin2_len;
        }
    } else {
        if (acct_field == 0) {
            buf = acct_pin1;
            len = &acct_pin1_len;
        } else if (acct_field == 1) {
            buf = acct_pin2;
            len = &acct_pin2_len;
        }
    }
    if (buf != NULL) {
        if (c == '\b' || c == 0x7F) {
            if (*len > 0) {
                (*len)--;
                buf[*len] = '\0';
            }
            desktop_redraw();
        } else if (c >= 32 && c < 127 && *len < cap) {
            buf[(*len)++] = (char)c;
            buf[*len] = '\0';
            desktop_redraw();
        }
    }
    return 1;
}

static int acct_key(int c) {
    if (acct_mode == ACCT_MODE_TOTP_SHOW) {
        acct_mode = ACCT_MODE_LIST;
        desktop_redraw();
        return 1;
    }
    if (acct_mode == ACCT_MODE_DEL) {
        if (c == '\n' || c == '\r') {
            pin_dialog_begin(PINOP_ACCT_DEL);
        }
        desktop_redraw();
        return 1;
    }
    if (acct_mode == ACCT_MODE_ADD || acct_mode == ACCT_MODE_PASSWD) {
        return acct_form_key(c);
    }
    /* 一覧モード。 */
    acct_refresh();
    int total = acct_user_count + 2;
    if (c == DESK_KEY_UP) {
        if (acct_selected > 0) {
            acct_selected--;
        }
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_DOWN) {
        if (acct_selected + 1 < total) {
            acct_selected++;
        }
        desktop_redraw();
        return 1;
    }
    if (!shell_current_user_is_admin()) {
        if (c == '\n' || c == '\r' || c == 'p' || c == 'd' ||
            c == 't') {
            set_page_status(shell_session_is_guest()
                                ? L("Guests cannot manage users.",
                                    "ゲストはユーザーを管理できません")
                                : L("Admin only.", "管理者のみです"));
            serial_write_line("settings: account action refused");
            desktop_redraw();
        }
        return 1;
    }
    if (c == '\n' || c == '\r') {
        if (acct_selected == acct_user_count + 1) {
            acct_clear_form();
            acct_status[0] = '\0';
            acct_mode = ACCT_MODE_ADD;
            desktop_redraw();
        }
        return 1;
    }
    if (c == 'p') {
        acct_set_target();
        acct_pin1[0] = '\0';
        acct_pin1_len = 0;
        acct_pin2[0] = '\0';
        acct_pin2_len = 0;
        acct_field = 0;
        acct_status[0] = '\0';
        acct_mode = ACCT_MODE_PASSWD;
        desktop_redraw();
        return 1;
    }
    if (c == 'd') {
        acct_set_target();
        if (strcmp(acct_target, "admin") == 0) {
            set_page_status(L("The primary account cannot be deleted.",
                              "主アカウントは削除できません"));
        } else if (strcmp(acct_target,
                          shell_current_user_name()) == 0) {
            set_page_status(L("You cannot delete yourself.",
                              "自分自身は削除できません"));
        } else {
            char role[16];
            int admin_role =
                users_role(acct_text, acct_text_len, acct_target,
                           role, sizeof(role)) == 0 &&
                strcmp(role, "admin") == 0;
            if (admin_role && acct_admin_count() <= 1) {
                set_page_status(L("Cannot delete the last admin.",
                                  "最後の管理者は削除できません"));
            } else {
                acct_status[0] = '\0';
                acct_mode = ACCT_MODE_DEL;
            }
        }
        desktop_redraw();
        return 1;
    }
    if (c == 't') {
        acct_set_target();
        acct_refresh();
        if (acct_totp_on(acct_target)) {
            pin_dialog_begin(PINOP_ACCT_TOTP_OFF);
        } else {
            pin_dialog_begin(PINOP_ACCT_TOTP_SET);
        }
        desktop_redraw();
        return 1;
    }
    return 1;
}

/* ---- キー処理: セキュリティページ ---- */
static int sec_key(int c) {
    int total = 6 + SEC_APP_COUNT;
    if (c == DESK_KEY_UP) {
        if (sec_selected > 0) {
            sec_selected--;
        }
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_DOWN) {
        if (sec_selected + 1 < total) {
            sec_selected++;
        }
        desktop_redraw();
        return 1;
    }
    if (!shell_current_user_is_admin()) {
        if (c == '\n' || c == '\r' || c == ' ' ||
            c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT) {
            set_page_status(L("Admin only.", "管理者のみです"));
            serial_write_line("settings: security change refused");
            desktop_redraw();
        }
        return 1;
    }
    if (sec_selected == 0) {
        if (c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT || c == ' ' ||
            c == '\n' || c == '\r') {
            sec_usb = !sec_usb;
            sec_dirty = 1;
        }
    } else if (sec_selected == 1) {
        if (c == DESK_KEY_LEFT && sec_pin_min > 4) {
            sec_pin_min--;
            sec_dirty = 1;
        }
        if (c == DESK_KEY_RIGHT && sec_pin_min < 16) {
            sec_pin_min++;
            sec_dirty = 1;
        }
    } else if (sec_selected == 2) {
        if (c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT || c == ' ' ||
            c == '\n' || c == '\r') {
            sec_pin_digit = !sec_pin_digit;
            sec_dirty = 1;
        }
    } else if (sec_selected == 3) {
        if (c == DESK_KEY_LEFT && sec_lock_attempts > 1) {
            sec_lock_attempts--;
            sec_dirty = 1;
        }
        if (c == DESK_KEY_RIGHT && sec_lock_attempts < 10) {
            sec_lock_attempts++;
            sec_dirty = 1;
        }
    } else if (sec_selected == 4) {
        if (c == DESK_KEY_LEFT && sec_lock_seconds >= 30) {
            sec_lock_seconds -= 30;
            sec_dirty = 1;
        }
        if (c == DESK_KEY_RIGHT && sec_lock_seconds <= 3570) {
            sec_lock_seconds += 30;
            sec_dirty = 1;
        }
    } else if (sec_selected == 5) {
        if (c == '\n' || c == '\r') {
            if (sec_dirty) {
                pin_dialog_begin(PINOP_SEC_SAVE);
            } else {
                set_page_status(L("No changes to save.",
                                  "保存する変更がありません"));
            }
        }
    } else {
        int app = sec_selected - 6;
        if (c == ' ' || c == '\n' || c == '\r') {
            if (desk_allowed[app] &&
                (app == 0 || app == 1 || app == 2)) {
                set_page_status(
                    L("Terminal, Files and Settings cannot be denied.",
                      "ターミナル、ファイル、設定は拒否できません"));
            } else {
                sec_pin_app = app;
                sec_pin_allow = !desk_allowed[app];
                pin_dialog_begin(PINOP_SEC_ALLOW);
            }
        }
    }
    desktop_redraw();
    return 1;
}

/* ---- キー処理: 更新ページ ---- */
static int upd_key(int c) {
    if (c == DESK_KEY_UP) {
        if (upd_selected > 0) {
            upd_selected--;
        }
        upd_confirm = 0;
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_DOWN) {
        if (upd_selected < 4) {
            upd_selected++;
        }
        upd_confirm = 0;
        desktop_redraw();
        return 1;
    }
    if (c != '\n' && c != '\r') {
        return 1;
    }
    if (upd_selected == 0) {
        upd_do_check();
    } else if (upd_selected == 1) {
        if (!shell_current_user_is_admin()) {
            set_page_status(L("Admin only.", "管理者のみです"));
        } else {
            pin_dialog_begin(PINOP_UPD_AUTO);
        }
    } else if (upd_selected == 2) {
        if (!shell_current_user_is_admin()) {
            set_page_status(L("Admin only.", "管理者のみです"));
        } else {
            pin_dialog_begin(PINOP_UPD_CHANNEL);
        }
    } else if (upd_selected == 3) {
        if (!shell_current_user_is_admin()) {
            set_page_status(L("Admin only.", "管理者のみです"));
        } else if (upd_check_rc != SHELL_UPDATE_AVAILABLE) {
            set_page_status(L("Check for updates first.",
                              "先に更新を確認してください"));
        } else if (strcmp(upd_info.type, "feature") == 0 &&
                   upd_confirm != 1) {
            upd_confirm = 1;
            set_page_status(
                L("This is a feature update. Press Enter again to apply.",
                  "機能更新です。もう一度 Enter を押すと適用します"));
        } else {
            pin_dialog_begin(PINOP_UPD_APPLY);
        }
    } else if (upd_selected == 4) {
        if (!shell_current_user_is_admin()) {
            set_page_status(L("Admin only.", "管理者のみです"));
        } else if (upd_confirm != 2) {
            upd_confirm = 2;
            set_page_status(
                L("Press Enter again to roll back.",
                  "もう一度 Enter を押すと巻き戻します"));
        } else {
            pin_dialog_begin(PINOP_UPD_ROLLBACK);
        }
    }
    desktop_redraw();
    return 1;
}

/* 設定アプリを開いたときの初期化 (switch_mode から)。 */
static void settings_gui_enter(void) {
    settings_refresh_shared();
    acct_mode = ACCT_MODE_LIST;
    pin_dialog_open = 0;
    pin_dialog_len = 0;
    pin_dialog_buf[0] = '\0';
    acct_status[0] = '\0';
    sec_status[0] = '\0';
    upd_status[0] = '\0';
    upd_check_rc = 99;
    upd_confirm = 0;
    upd_refresh_snaps();
}

/* Esc でフォーム/ダイアログを取り消す (全体の Esc 処理から)。 */
static void settings_cancel_modal(void) {
    if (pin_dialog_open) {
        pin_dialog_open = 0;
        pin_dialog_len = 0;
        pin_dialog_buf[0] = '\0';
        pin_dialog_error = 0;
        return;
    }
    if (acct_mode != ACCT_MODE_LIST) {
        acct_mode = ACCT_MODE_LIST;
        acct_clear_form();
    }
}

/* ---- Browser: a small real renderer ----
 * Fetches a page (HTTP, or HTTPS with the TLS client),
 * follows redirects, parses HTML into display lines with
 * colours (a basic CSS subset: <style> tag rules for
 * body/h1/h2/h3/p/a colours, inline style=, <font color>),
 * clickable/Tab-focusable links, and inline BMP images
 * (24-bit BMP only; PNG/JPEG have no decoder and show
 * their alt text). JavaScript is not executed.
 *
 * HTTPS trust policy: certificate pinning with
 * trust-on-first-use. The first visit to a host stores
 * the certificate's SHA-256 fingerprint in
 * browser_pins.txt; later visits must present the same
 * certificate or the handshake is refused. */

#define BR_MAX_LINES 220
#define BR_LINE_TEXT 120
#define BR_MAX_LINKS 32
#define BR_MAX_IMAGES 4
#define BR_RAW_CAP 262144

typedef struct {
    char text[BR_LINE_TEXT];
    uint32_t color; /* 0 = page default */
    int link;       /* index into br_links, or -1 */
    int heading;    /* 0 = body, 1 = h3, 2 = h2, 3 = h1 */
    int img;        /* image index, or -1 */
    int img_first;  /* first display row of the image */
} br_line;

static br_line br_lines[BR_MAX_LINES];
static int br_line_count;
static char br_links[BR_MAX_LINKS][168];
static int br_link_count;
static int br_focus; /* focused link index, or -1 */
static uint32_t br_page_bg; /* 0 = window default */
static uint32_t br_page_fg; /* 0 = default text colour */
static char br_title[96];

static char *br_raw; /* fetched bytes (HTML or image) */
/* ページ内検索・保存・ズーム (バッチ4 項目20)。 */
static size_t br_raw_len;
static int br_wrap_cols; /* ズームに応じた折返し幅 (0=自動) */
static int browser_zoom = 1; /* 1..3 */
static int br_find_mode;
static char br_find_buf[64];
static int br_find_len;
static int br_find_last; /* 最後に見つけた行 (-1 = なし) */
static char *br_img_raw[BR_MAX_IMAGES];
static wallpaper_img br_imgs[BR_MAX_IMAGES];
static int br_img_disp_w[BR_MAX_IMAGES];
static int br_img_disp_h[BR_MAX_IMAGES];
static int br_img_count;

/* CSS subset state: colours per element kind. */
static uint32_t br_css_color[6]; /* body,h1,h2,h3,p,a */
static uint32_t br_css_bg;       /* body background */

typedef struct {
    int https;
    char host[96];
    int port;
    char path[168];
} br_url_parts;

static int br_parse_color(const char *s, uint32_t *out) {
    static const char *names[] = {
        "black", "white", "red", "green", "blue", "yellow",
        "cyan", "magenta", "gray", "grey", "orange", "purple",
        "brown", "pink", "lime", "navy", "teal", "maroon",
        "silver", NULL
    };
    static const uint32_t values[] = {
        0x000000, 0xFFFFFF, 0xFF0000, 0x008000, 0x0000FF,
        0xFFFF00, 0x00FFFF, 0xFF00FF, 0x808080, 0x808080,
        0xFFA500, 0x800080, 0xA52A2A, 0xFFC0CB, 0x00FF00,
        0x000080, 0x008080, 0x800000, 0xC0C0C0
    };
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (s[0] == '#') {
        uint32_t v = 0;
        int n = 0;
        for (int i = 1; s[i] != '\0' && n < 6; i++) {
            char ch = s[i];
            int d = -1;
            if (ch >= '0' && ch <= '9') {
                d = ch - '0';
            } else if (ch >= 'a' && ch <= 'f') {
                d = ch - 'a' + 10;
            } else if (ch >= 'A' && ch <= 'F') {
                d = ch - 'A' + 10;
            }
            if (d < 0) {
                break;
            }
            v = v * 16 + (uint32_t)d;
            n++;
        }
        if (n == 3) {
            /* #rgb -> #rrggbb */
            uint32_t r = (v >> 8) & 0xF;
            uint32_t g = (v >> 4) & 0xF;
            uint32_t b = v & 0xF;
            *out = (r << 20) | (r << 16) | (g << 12) | (g << 8) |
                   (b << 4) | b;
            return 0;
        }
        if (n == 6) {
            *out = v;
            return 0;
        }
        return -1;
    }
    for (int i = 0; names[i] != NULL; i++) {
        size_t nl = strlen(names[i]);
        if (strncmp(s, names[i], nl) == 0 &&
            (s[nl] == '\0' || s[nl] == ';' || s[nl] == ' ' ||
             s[nl] == '}')) {
            *out = values[i];
            return 0;
        }
    }
    return -1;
}

static int br_parse_url(const char *url, br_url_parts *out) {
    const char *p = url;
    out->https = 0;
    out->port = 80;
    if (strncmp(p, "https://", 8) == 0) {
        out->https = 1;
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    size_t h = 0;
    while (p[h] != '\0' && p[h] != '/' && p[h] != ':' &&
           h + 1 < sizeof(out->host)) {
        out->host[h] = p[h];
        h++;
    }
    out->host[h] = '\0';
    if (out->host[0] == '\0') {
        return -1;
    }
    const char *q = p + h;
    if (*q == ':') {
        int port = 0;
        q++;
        while (*q >= '0' && *q <= '9') {
            /* 範囲を超えたらそれ以上は累積しない (監査 L8:
             * 桁数無制限の累積が int をあふれさせ、あふれた
             * 後の値で範囲チェックをすり抜けていた。悪意の
             * あるサーバーの Location: 等から到達可能)。 */
            if (port <= 65535) {
                port = port * 10 + (*q - '0');
            }
            q++;
        }
        if (port > 0 && port < 65536) {
            out->port = port;
        }
    }
    if (*q == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        size_t n = 0;
        while (q[n] != '\0' && n + 1 < sizeof(out->path)) {
            out->path[n] = q[n];
            n++;
        }
        out->path[n] = '\0';
    }
    return 0;
}

static void br_compose_url(const br_url_parts *u, char *out,
                           size_t cap) {
    size_t p = 0;
    const char *scheme = u->https ? "https://" : "http://";
    while (*scheme != '\0' && p + 1 < cap) {
        out[p++] = *scheme++;
    }
    const char *h = u->host;
    while (*h != '\0' && p + 1 < cap) {
        out[p++] = *h++;
    }
    int defport = u->https ? 443 : 80;
    if (u->port != defport && p + 1 < cap) {
        char num[8];
        int ni = 0;
        int v = u->port;
        char tmp[8];
        int ti = 0;
        while (v > 0) {
            tmp[ti++] = (char)('0' + v % 10);
            v /= 10;
        }
        out[p++] = ':';
        while (ti > 0 && p + 1 < cap) {
            num[ni++] = tmp[--ti];
            out[p++] = num[ni - 1];
        }
    }
    const char *pa = u->path;
    while (*pa != '\0' && p + 1 < cap) {
        out[p++] = *pa++;
    }
    out[p] = '\0';
}

/* Resolve href against the current page URL. */
static void br_resolve(const br_url_parts *base, const char *href,
                       char *out, size_t cap) {
    if (strncmp(href, "http://", 7) == 0 ||
        strncmp(href, "https://", 8) == 0) {
        size_t n = 0;
        while (href[n] != '\0' && n + 1 < cap) {
            out[n] = href[n];
            n++;
        }
        out[n] = '\0';
        return;
    }
    br_url_parts u = *base;
    if (href[0] == '/') {
        size_t n = 0;
        while (href[n] != '\0' && n + 1 < sizeof(u.path)) {
            u.path[n] = href[n];
            n++;
        }
        u.path[n] = '\0';
    } else {
        /* Relative: replace the last path segment. */
        char dir[168];
        size_t dn = 0;
        const char *last = u.path;
        for (const char *s = u.path; *s != '\0'; s++) {
            if (*s == '/') {
                last = s;
            }
        }
        while (u.path + dn <= last && dn + 1 < sizeof(dir)) {
            dir[dn] = u.path[dn];
            dn++;
        }
        dir[dn] = '\0';
        size_t n = 0;
        while (dir[n] != '\0' && n + 1 < sizeof(u.path)) {
            u.path[n] = dir[n];
            n++;
        }
        size_t hi = 0;
        while (href[hi] != '\0' && n + 1 < sizeof(u.path)) {
            u.path[n++] = href[hi++];
        }
        u.path[n] = '\0';
    }
    br_compose_url(&u, out, cap);
}

/* ---- Certificate pins (trust on first use) ---- */

/* proxy.conf ("host port") を読む。設定があれば 0。 */
static int br_proxy(char *host, size_t hcap, int *port_out) {
    size_t len = 0;
    const char *text = vfs_read_ptr("proxy.conf", &len);
    if (text == NULL || len == 0) {
        return -1;
    }
    char buf[128];
    if (len + 1 > sizeof(buf)) {
        return -1;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    size_t i = 0;
    while (buf[i] != '\0' && buf[i] != ' ' && i + 1 < hcap) {
        host[i] = buf[i];
        i++;
    }
    host[i] = '\0';
    while (buf[i] == ' ') {
        i++;
    }
    int port = 0;
    while (buf[i] >= '0' && buf[i] <= '9') {
        port = port * 10 + (buf[i] - '0');
        i++;
    }
    if (host[0] == '\0' || port <= 0 || port > 65535) {
        return -1;
    }
    *port_out = port;
    return 0;
}

static int br_pin_lookup(const char *host, char *fp_out) {
    size_t len = 0;
    const char *data = vfs_read_ptr("browser_pins.txt", &len);
    if (data == NULL) {
        return -1;
    }
    size_t hl = strlen(host);
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && data[i] != '\n') {
            i++;
        }
        /* line = data[ls..i) */
        if (i - ls > hl + 1 && memcmp(data + ls, host, hl) == 0 &&
            data[ls + hl] == ' ') {
            size_t n = 0;
            size_t j = ls + hl + 1;
            while (j < i && n < 64) {
                fp_out[n++] = data[j++];
            }
            fp_out[n] = '\0';
            if (n == 64) {
                return 0;
            }
            return -1;
        }
        i++; /* skip newline */
    }
    return -1;
}

static void br_pin_save(const char *host, const char *fp) {
    char buf[4096];
    size_t p = 0;
    size_t len = 0;
    const char *old = vfs_read_ptr("browser_pins.txt", &len);
    if (old != NULL) {
        while (p < len && p + 1 < sizeof(buf)) {
            buf[p] = old[p];
            p++;
        }
    }
    const char *h = host;
    while (*h != '\0' && p + 1 < sizeof(buf)) {
        buf[p++] = *h++;
    }
    if (p + 1 < sizeof(buf)) {
        buf[p++] = ' ';
    }
    while (*fp != '\0' && p + 1 < sizeof(buf)) {
        buf[p++] = *fp++;
    }
    if (p + 1 < sizeof(buf)) {
        buf[p++] = '\n';
    }
    buf[p] = '\0';
    if (vfs_write("browser_pins.txt", buf, p) == 0) {
        (void)persist_maybe_sync();
    }
    serial_write("browser: pin saved for ");
    serial_write_line(host);
}

/* ---- Fetching ---- */

static char br_fetch_err[64];

/* Fetch url into buf (following redirects). On success
 * returns 0 and sets *out_len / *out_status; the final URL
 * (after redirects) goes to final_url. */
/* ---- Cookie jar (VFS の cookies.txt に保存) ----
 * 形式は net.c の純粋関数が扱う "host<TAB>name<TAB>value"。
 * 有効期限やパス属性は保持しない (ドメイン照合のみ)。 */
#define BR_COOKIE_JAR_CAP 8192
static char br_cookie_jar[BR_COOKIE_JAR_CAP];

static void br_cookie_jar_load(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr("cookies.txt", &len);
    if (text == NULL) {
        br_cookie_jar[0] = '\0';
        return;
    }
    if (len > sizeof(br_cookie_jar) - 1) {
        len = sizeof(br_cookie_jar) - 1;
    }
    for (size_t i = 0; i < len; i++) {
        br_cookie_jar[i] = text[i];
    }
    br_cookie_jar[len] = '\0';
}

/* 応答の Set-Cookie (複数行) を jar に取り込む。 */
static void br_cookie_store(const char *host,
                            const char *set_cookie_block) {
    br_cookie_jar_load();
    int changed = 0;
    const char *p = set_cookie_block;
    while (*p != '\0') {
        const char *eol = p;
        while (*eol != '\0' && *eol != '\n') {
            eol++;
        }
        char one[256];
        size_t n = (size_t)(eol - p);
        if (n > sizeof(one) - 1) {
            n = sizeof(one) - 1;
        }
        for (size_t i = 0; i < n; i++) {
            one[i] = p[i];
        }
        one[n] = '\0';
        char name[64], value[128];
        if (net_cookie_parse_pair(one, name, sizeof(name), value,
                                  sizeof(value)) == 0 &&
            net_cookie_jar_update(br_cookie_jar,
                                  sizeof(br_cookie_jar), host,
                                  name, value) == 0) {
            changed = 1;
            serial_write("browser: cookie ");
            serial_write(name);
            serial_write(" set for ");
            serial_write_line(host);
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    if (changed) {
        size_t len = 0;
        while (br_cookie_jar[len] != '\0') {
            len++;
        }
        vfs_write("cookies.txt", br_cookie_jar, len);
    }
}

static int br_fetch_buf(const char *url, char *buf,
                        size_t buf_cap, size_t *out_len,
                        int *out_status, char *final_url,
                        size_t final_cap) {
    char current[256];
    size_t n = 0;
    while (url[n] != '\0' && n + 1 < sizeof(current)) {
        current[n] = url[n];
        n++;
    }
    current[n] = '\0';
    for (int hop = 0; hop < 4; hop++) {
        br_url_parts u;
        if (br_parse_url(current, &u) != 0) {
            strncpy(br_fetch_err, L("bad address", "アドレスが不正"),
                    sizeof(br_fetch_err) - 1);
            return -1;
        }
        net_fetch_info info;
        size_t len = 0;
        int rc;
        /* このホスト宛の Cookie を jar から組み立てて
         * フェッチに渡す (終わったら必ず消す)。 */
        br_cookie_jar_load();
        char cookie_hdr[512];
        net_cookie_build_header(br_cookie_jar, u.host, cookie_hdr,
                                sizeof(cookie_hdr));
        net_set_fetch_cookie(cookie_hdr);
        if (u.https) {
            char pin[65];
            if (br_pin_lookup(u.host, pin) == 0) {
                rc = net_fetch_https(u.host, (uint16_t)u.port,
                                     u.path, pin, NULL, buf,
                                     buf_cap, &len, &info);
            } else {
                char fp[65];
                rc = net_fetch_https(u.host, (uint16_t)u.port,
                                     u.path, NULL, fp, buf,
                                     buf_cap, &len, &info);
                if (rc == 0) {
                    br_pin_save(u.host, fp);
                }
            }
        } else {
            /* 平文 HTTP は proxy.conf があればプロキシ
             * 経由で取得する。 */
            char phost[64];
            int pport = 0;
            if (br_proxy(phost, sizeof(phost), &pport) == 0) {
                rc = net_fetch_http_via(phost, (uint16_t)pport,
                                        u.host, u.path, buf,
                                        buf_cap, &len, &info);
            } else {
                rc = net_fetch_http_px(u.host, (uint16_t)u.port,
                                       u.path, buf, buf_cap,
                                       &len, &info);
            }
        }
        net_set_fetch_cookie(NULL);
        if (rc != 0) {
            strncpy(br_fetch_err, L("fetch failed (network or TLS)", "取得に失敗 (ネットワークまたはTLS)"),
                    sizeof(br_fetch_err) - 1);
            return -1;
        }
        *out_status = info.status;
        if (info.set_cookie[0] != '\0') {
            br_cookie_store(u.host, info.set_cookie);
        }
        if ((info.status == 301 || info.status == 302 ||
             info.status == 303 || info.status == 307 ||
             info.status == 308) &&
            info.location[0] != '\0' && hop < 3) {
            br_url_parts base;
            (void)br_parse_url(current, &base);
            br_resolve(&base, info.location, current,
                       sizeof(current));
            serial_write("browser: redirect -> ");
            serial_write_line(current);
            continue;
        }
        if (info.status != 200) {
            char msg[64];
            size_t mp = 0;
            const char *pre = L("HTTP status ", "HTTPステータス ");
            while (*pre != '\0' && mp + 1 < sizeof(msg)) {
                msg[mp++] = *pre++;
            }
            int v = info.status;
            char tmp[8];
            int ti = 0;
            if (v == 0) {
                tmp[ti++] = '0';
            }
            while (v > 0) {
                tmp[ti++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (ti > 0 && mp + 1 < sizeof(msg)) {
                msg[mp++] = tmp[--ti];
            }
            msg[mp] = '\0';
            strncpy(br_fetch_err, msg, sizeof(br_fetch_err) - 1);
            return -1;
        }
        *out_len = len;
        /* Record the final URL (after redirects), in
         * canonical scheme://host form. */
        br_url_parts fu;
        if (br_parse_url(current, &fu) == 0) {
            br_compose_url(&fu, final_url, final_cap);
        } else {
            size_t ci = 0;
            while (current[ci] != '\0' && ci + 1 < final_cap) {
                final_url[ci] = current[ci];
                ci++;
            }
            final_url[ci] = '\0';
        }
        return 0;
    }
    strncpy(br_fetch_err, L("too many redirects", "リダイレクトが多すぎます"),
            sizeof(br_fetch_err) - 1);
    return -1;
}

/* Fetch the address-bar URL into br_raw and make the
 * final URL the new address-bar content. */
static int br_fetch(const char *url, size_t *out_len,
                    int *out_status) {
    if (br_raw == NULL) {
        br_raw = (char *)kmalloc(BR_RAW_CAP);
        if (br_raw == NULL) {
            return -1;
        }
    }
    char final_url[256];
    int rc = br_fetch_buf(url, br_raw, BR_RAW_CAP, out_len,
                          out_status, final_url,
                          sizeof(final_url));
    if (rc == 0) {
        size_t ci = 0;
        while (final_url[ci] != '\0' &&
               ci + 1 < sizeof(browser_url)) {
            browser_url[ci] = final_url[ci];
            ci++;
        }
        browser_url[ci] = '\0';
        browser_url_len = (int)ci;
    }
    return rc;
}

/* ---- HTML parsing into display lines ---- */

static char p_text[BR_LINE_TEXT];
static int p_len;
static uint32_t p_color;
static int p_link;
static int p_heading;
static int p_blank_pending;
static int p_space_pending;

static void br_reset_line(void) {
    p_space_pending = 0;
    p_len = 0;
    p_text[0] = '\0';
    p_color = 0;
    p_link = -1;
    p_heading = 0;
}

static void br_flush_line(void) {
    if (p_len == 0) {
        return;
    }
    if (br_line_count < BR_MAX_LINES) {
        br_line *ln = &br_lines[br_line_count++];
        size_t i = 0;
        while (p_text[i] != '\0') {
            ln->text[i] = p_text[i];
            i++;
        }
        ln->text[i] = '\0';
        ln->color = p_color;
        ln->link = p_link;
        ln->heading = p_heading;
        ln->img = -1;
        ln->img_first = 0;
    }
    int link = p_link;
    int heading = p_heading;
    uint32_t color = p_color;
    br_reset_line();
    p_link = link;
    p_heading = heading;
    p_color = color;
}

static void br_blank_line(void) {
    br_flush_line();
    if (br_line_count < BR_MAX_LINES && br_line_count > 0 &&
        br_lines[br_line_count - 1].text[0] != '\0') {
        br_line *ln = &br_lines[br_line_count++];
        ln->text[0] = '\0';
        ln->color = 0;
        ln->link = -1;
        ln->heading = 0;
        ln->img = -1;
        ln->img_first = 0;
    }
}

static void br_putc(char ch) {
    int cols = br_wrap_cols > 0 ? br_wrap_cols : text_cols();
    if (cols > BR_LINE_TEXT - 2) {
        cols = BR_LINE_TEXT - 2;
    }
    if (cols < 8) {
        cols = 8;
    }
    if (p_len >= cols - 1) {
        /* Wrap at the last space if there is one. */
        int split = -1;
        for (int i = p_len - 1; i > 0; i--) {
            if (p_text[i] == ' ') {
                split = i;
                break;
            }
        }
        if (split > 0) {
            char tail[BR_LINE_TEXT];
            int tn = 0;
            for (int i = split + 1; i < p_len; i++) {
                tail[tn++] = p_text[i];
            }
            tail[tn] = '\0';
            p_text[split] = '\0';
            p_len = split;
            br_flush_line();
            for (int i = 0; i < tn; i++) {
                p_text[p_len++] = tail[i];
            }
            p_text[p_len] = '\0';
        } else {
            br_flush_line();
        }
    }
    if (p_len + 1 < BR_LINE_TEXT) {
        p_text[p_len++] = ch;
        p_text[p_len] = '\0';
    }
}

static void br_puts(const char *s) {
    while (*s != '\0') {
        br_putc(*s++);
    }
}

/* Find an attribute value in a tag's attribute text. */
static int br_attr(const char *attrs, const char *name, char *out,
                   size_t cap) {
    size_t nl = strlen(name);
    const char *p = attrs;
    while (*p != '\0') {
        if (strncmp(p, name, nl) == 0 &&
            (p[nl] == '=' || p[nl] == ' ')) {
            p += nl;
            while (*p == ' ' || *p == '=') {
                p++;
            }
            char quote = 0;
            if (*p == '"' || *p == '\'') {
                quote = *p++;
            }
            size_t n = 0;
            while (*p != '\0' && n + 1 < cap &&
                   (quote ? *p != quote
                          : (*p != ' ' && *p != '\t' && *p != '>'))) {
                out[n++] = *p++;
            }
            out[n] = '\0';
            return 0;
        }
        p++;
    }
    return -1;
}

static int br_tag_is(const char *tag, const char *name) {
    size_t nl = strlen(name);
    return strncmp(tag, name, nl) == 0 &&
           (tag[nl] == '\0' || tag[nl] == ' ' || tag[nl] == '/' ||
            tag[nl] == '\t');
}

/* Parse a style attribute / CSS declaration list for
 * "color: X" and "background(-color): X". */
static void br_style_colors(const char *decl, uint32_t *color,
                            uint32_t *bg) {
    const char *p = decl;
    while (*p != '\0') {
        if (strncmp(p, "color:", 6) == 0) {
            uint32_t v;
            if (br_parse_color(p + 6, &v) == 0 && color != NULL) {
                *color = v;
            }
        } else if (strncmp(p, "background-color:", 17) == 0) {
            uint32_t v;
            if (br_parse_color(p + 17, &v) == 0 && bg != NULL) {
                *bg = v;
            }
        } else if (strncmp(p, "background:", 11) == 0) {
            uint32_t v;
            if (br_parse_color(p + 11, &v) == 0 && bg != NULL) {
                *bg = v;
            }
        }
        p++;
    }
}

/* Parse the <style> subset: rules "sel { decls }" with
 * sel in body/h1/h2/h3/p/a. */
static void br_parse_css(const char *css, size_t len) {
    static const char *sels[6] = { "body", "h1", "h2",
                                   "h3",   "p",  "a" };
    size_t i = 0;
    while (i < len) {
        /* selector */
        while (i < len && (css[i] == ' ' || css[i] == '\n' ||
                           css[i] == '\r' || css[i] == '\t')) {
            i++;
        }
        size_t ss = i;
        while (i < len && css[i] != '{' && css[i] != '}') {
            i++;
        }
        if (i >= len || css[i] != '{') {
            break;
        }
        size_t sl = i - ss;
        while (sl > 0 && (css[ss + sl - 1] == ' ' ||
                          css[ss + sl - 1] == '\t')) {
            sl--;
        }
        i++;
        size_t ds = i;
        while (i < len && css[i] != '}') {
            i++;
        }
        char decl[160];
        size_t dn = 0;
        for (size_t k = ds; k < i && dn + 1 < sizeof(decl); k++) {
            decl[dn++] = css[k];
        }
        decl[dn] = '\0';
        if (i < len) {
            i++;
        }
        for (int s = 0; s < 6; s++) {
            if (strlen(sels[s]) == sl &&
                strncmp(css + ss, sels[s], sl) == 0) {
                uint32_t bg = 0;
                br_style_colors(decl, &br_css_color[s],
                                s == 0 ? &bg : NULL);
                if (s == 0 && bg != 0) {
                    br_css_bg = bg;
                }
            }
        }
    }
}

static int br_add_link(const char *url) {
    if (br_link_count >= BR_MAX_LINKS) {
        return br_link_count - 1;
    }
    int id = br_link_count++;
    size_t n = 0;
    while (url[n] != '\0' && n + 1 < sizeof(br_links[id])) {
        br_links[id][n] = url[n];
        n++;
    }
    br_links[id][n] = '\0';
    return id;
}

static void br_free_images(void) {
    for (int i = 0; i < BR_MAX_IMAGES; i++) {
        if (br_img_raw[i] != NULL) {
            kfree(br_img_raw[i]);
            br_img_raw[i] = NULL;
        }
        br_imgs[i].pixels = NULL;
    }
    br_img_count = 0;
}

/* Fetch and register one image. Returns the image index
 * or -1 (caller shows alt text instead). */
/* PNG をデコードして画像スロット idx に登録する。
 * ピクセルは表示上限 (480x240) に縮小してヒープに持ち、
 * br_img_raw[idx] が所有する (br_free_images で解放)。 */
static int br_register_png(const uint8_t *data, size_t len,
                           int idx) {
    uint8_t *px = (uint8_t *)kmalloc(480 * 240 * 3);
    if (px == NULL) {
        return -1;
    }
    int dw = 0, dh = 0;
    if (png_decode(data, len, px, 480 * 240 * 3, 480, 240,
                   &dw, &dh) != 0) {
        kfree(px);
        return -1;
    }
    br_imgs[idx].pixels = px;
    br_imgs[idx].width = dw;
    br_imgs[idx].height = dh;
    br_imgs[idx].stride = dw * 3;
    br_imgs[idx].top_down = 1;
    br_img_raw[idx] = (char *)px;
    br_img_disp_w[idx] = dw;
    br_img_disp_h[idx] = dh;
    return 0;
}

static int br_load_image(const char *url) {
    if (br_img_count >= BR_MAX_IMAGES) {
        return -1;
    }
    char *buf = (char *)kmalloc(BR_RAW_CAP);
    if (buf == NULL) {
        return -1;
    }
    size_t len = 0;
    int status = 0;
    char final_url[256];
    if (br_fetch_buf(url, buf, BR_RAW_CAP, &len, &status,
                     final_url, sizeof(final_url)) != 0) {
        kfree(buf);
        return -1;
    }
    int idx = br_img_count;
    if (wallpaper_parse((const uint8_t *)buf, len,
                        &br_imgs[idx]) == 0) {
        br_img_count++;
        br_img_raw[idx] = buf;
        int w = br_imgs[idx].width;
        int h = br_imgs[idx].height;
        int dw = w > 480 ? 480 : w;
        int dh = (int)((int64_t)h * dw / (w > 0 ? w : 1));
        if (dh > 240) {
            dh = 240;
            dw = (int)((int64_t)w * dh / (h > 0 ? h : 1));
        }
        if (dw < 1) {
            dw = 1;
        }
        if (dh < 1) {
            dh = 1;
        }
        br_img_disp_w[idx] = dw;
        br_img_disp_h[idx] = dh;
    } else if (png_is_png((const uint8_t *)buf, len)) {
        if (br_register_png((const uint8_t *)buf, len, idx) != 0) {
            kfree(buf);
            return -1;
        }
        br_img_count++;
        kfree(buf);
        serial_write("browser: image (png) ");
        serial_write(final_url);
        serial_write_line("");
        return idx;
    } else {
        kfree(buf);
        return -1;
    }
    serial_write("browser: image ");
    serial_write(final_url);
    serial_write_line("");
    return idx;
}

static void br_insert_image(int idx) {
    br_flush_line();
    int rows = (br_img_disp_h[idx] + CELL_H - 1) / CELL_H;
    for (int r = 0; r < rows && br_line_count < BR_MAX_LINES; r++) {
        br_line *ln = &br_lines[br_line_count++];
        ln->text[0] = '\0';
        ln->color = 0;
        ln->link = -1;
        ln->heading = 0;
        ln->img = idx;
        ln->img_first = (r == 0);
    }
}

/* The element colour for a block element start. */
static uint32_t br_elem_color(int elem, const char *attrs) {
    uint32_t c = br_css_color[elem];
    char style[96];
    if (br_attr(attrs, "style", style, sizeof(style)) == 0) {
        uint32_t ic = 0;
        br_style_colors(style, &ic, NULL);
        if (ic != 0) {
            c = ic;
        }
    }
    return c;
}

/* <script> の蓄積と実行。document.write の出力は
 * その位置の本文テキストとして流し込む。 */
static char br_script_buf[8192];
static size_t br_script_len;

static void br_js_write(void *ctx, const char *text) {
    (void)ctx;
    br_puts(text);
}

static void br_run_script(void) {
    br_script_buf[br_script_len] = '\0';
    if (br_script_len == 0) {
        return;
    }
    js_host host;
    host.ctx = NULL;
    host.write = br_js_write;
    char err[96];
    if (js_run(br_script_buf, &host, err, sizeof(err)) == 0) {
        serial_write_line("browser: script ran");
    } else {
        serial_write("browser: script error: ");
        serial_write_line(err);
    }
}

static void br_parse_html(const char *html, size_t len,
                          const br_url_parts *base) {
    br_reset_line();
    int in_head = 0;
    int in_title = 0;
    int in_style = 0;
    int in_script = 0;
    int elem = 0; /* current block element kind for colour */
    uint32_t font_color = 0;
    int link_open = -1;
    char css_buf[2048];
    size_t css_len = 0;
    size_t i = 0;
    while (i < len) {
        if (html[i] == '<') {
            /* Tag: read to '>' (quotes respected). */
            size_t ts = i + 1;
            size_t te = ts;
            char quote = 0;
            while (te < len) {
                if (quote) {
                    if (html[te] == quote) {
                        quote = 0;
                    }
                } else if (html[te] == '"' || html[te] == '\'') {
                    quote = html[te];
                } else if (html[te] == '>') {
                    break;
                }
                te++;
            }
            char tag[512];
            size_t tn = 0;
            for (size_t k = ts; k < te && tn + 1 < sizeof(tag);
                 k++) {
                char ch = html[k];
                if (ch >= 'A' && ch <= 'Z') {
                    ch = (char)(ch - 'A' + 'a');
                }
                tag[tn++] = ch;
            }
            tag[tn] = '\0';
            i = te + 1;
            if (in_style) {
                if (br_tag_is(tag, "/style")) {
                    in_style = 0;
                    br_parse_css(css_buf, css_len);
                }
                continue;
            }
            if (in_script) {
                if (br_tag_is(tag, "/script")) {
                    in_script = 0;
                    br_run_script();
                }
                continue;
            }
            if (br_tag_is(tag, "style")) {
                in_style = 1;
                css_len = 0;
                continue;
            }
            if (br_tag_is(tag, "script")) {
                in_script = 1;
                br_script_len = 0;
                continue;
            }
            if (br_tag_is(tag, "head")) {
                in_head = 1;
                continue;
            }
            if (br_tag_is(tag, "/head")) {
                in_head = 0;
                continue;
            }
            if (br_tag_is(tag, "title")) {
                in_title = 1;
                br_title[0] = '\0';
                continue;
            }
            if (br_tag_is(tag, "/title")) {
                in_title = 0;
                continue;
            }
            if (in_head) {
                continue;
            }
            if (br_tag_is(tag, "body")) {
                uint32_t bg = br_css_bg;
                uint32_t fg = br_css_color[0];
                char style[128];
                if (br_attr(tag, "style", style,
                            sizeof(style)) == 0) {
                    br_style_colors(style, &fg, &bg);
                }
                char attr[64];
                if (br_attr(tag, "bgcolor", attr,
                            sizeof(attr)) == 0) {
                    (void)br_parse_color(attr, &bg);
                }
                if (br_attr(tag, "text", attr, sizeof(attr)) ==
                    0) {
                    (void)br_parse_color(attr, &fg);
                }
                br_page_bg = bg;
                br_page_fg = fg;
                continue;
            }
            if (br_tag_is(tag, "br") || br_tag_is(tag, "br/")) {
                br_flush_line();
                continue;
            }
            if (br_tag_is(tag, "p") || br_tag_is(tag, "div") ||
                br_tag_is(tag, "/p") || br_tag_is(tag, "/div")) {
                br_blank_line();
                elem = 4;
                p_color = br_elem_color(4, tag);
                p_heading = 0;
                continue;
            }
            if (br_tag_is(tag, "h1") || br_tag_is(tag, "h2") ||
                br_tag_is(tag, "h3")) {
                br_blank_line();
                elem = tag[1] == '1' ? 1 : (tag[1] == '2' ? 2 : 3);
                p_color = br_elem_color(elem, tag);
                if (p_color == 0) {
                    p_color = col_accent();
                }
                p_heading = 4 - elem;
                continue;
            }
            if (br_tag_is(tag, "/h1") || br_tag_is(tag, "/h2") ||
                br_tag_is(tag, "/h3")) {
                br_flush_line();
                p_heading = 0;
                elem = 0;
                continue;
            }
            if (br_tag_is(tag, "li")) {
                br_flush_line();
                br_puts("- ");
                continue;
            }
            if (br_tag_is(tag, "a")) {
                char href[200];
                if (br_attr(tag, "href", href, sizeof(href)) ==
                    0) {
                    char resolved[256];
                    br_resolve(base, href, resolved,
                               sizeof(resolved));
                    br_flush_line();
                    link_open = br_add_link(resolved);
                    p_link = link_open;
                    p_color = br_elem_color(5, tag);
                    if (p_color == 0) {
                        p_color = col_accent();
                    }
                }
                continue;
            }
            if (br_tag_is(tag, "/a")) {
                br_flush_line();
                link_open = -1;
                p_link = -1;
                p_color = br_css_color[elem];
                continue;
            }
            if (br_tag_is(tag, "font")) {
                char attr[64];
                if (br_attr(tag, "color", attr, sizeof(attr)) ==
                    0) {
                    uint32_t v;
                    if (br_parse_color(attr, &v) == 0) {
                        br_flush_line();
                        font_color = v;
                        p_color = v;
                    }
                }
                continue;
            }
            if (br_tag_is(tag, "/font")) {
                br_flush_line();
                font_color = 0;
                p_color = br_css_color[elem];
                continue;
            }
            if (br_tag_is(tag, "img")) {
                char src[200];
                char alt[80];
                alt[0] = '\0';
                (void)br_attr(tag, "alt", alt, sizeof(alt));
                if (br_attr(tag, "src", src, sizeof(src)) == 0) {
                    char resolved[256];
                    br_resolve(base, src, resolved,
                               sizeof(resolved));
                    int idx = br_load_image(resolved);
                    if (idx >= 0) {
                        br_insert_image(idx);
                    } else {
                        br_flush_line();
                        br_puts(L("[image: ", "[画像: "));
                        br_puts(alt[0] != '\0' ? alt : src);
                        br_puts("]");
                        br_flush_line();
                    }
                }
                continue;
            }
            /* Unknown tags are ignored (their text is kept). */
            continue;
        }
        /* Text run until the next '<'. In a script, only
         * "</script" ends the run: a '<' comparison in
         * the code is text, not a tag. */
        size_t rs = i;
        if (in_script) {
            while (i < len) {
                if (html[i] == '<' && i + 8 <= len &&
                    html[i + 1] == '/' &&
                    (html[i + 2] == 's' || html[i + 2] == 'S') &&
                    (html[i + 3] == 'c' || html[i + 3] == 'C') &&
                    (html[i + 4] == 'r' || html[i + 4] == 'R') &&
                    (html[i + 5] == 'i' || html[i + 5] == 'I') &&
                    (html[i + 6] == 'p' || html[i + 6] == 'P') &&
                    (html[i + 7] == 't' || html[i + 7] == 'T')) {
                    break;
                }
                i++;
            }
        } else {
            while (i < len && html[i] != '<') {
                i++;
            }
        }
        if (in_style) {
            for (size_t k = rs; k < i && css_len + 1 < sizeof(css_buf);
                 k++) {
                css_buf[css_len++] = html[k];
            }
            continue;
        }
        if (in_script) {
            for (size_t k = rs; k < i &&
                               br_script_len + 1 <
                                   sizeof(br_script_buf);
                 k++) {
                br_script_buf[br_script_len++] = html[k];
            }
            continue;
        }
        if (in_head && !in_title) {
            continue;
        }
        if (in_title) {
            size_t tn = strlen(br_title);
            for (size_t k = rs; k < i && tn + 1 < sizeof(br_title);
                 k++) {
                if (html[k] != '\n' && html[k] != '\r') {
                    br_title[tn++] = html[k];
                }
            }
            br_title[tn] = '\0';
            continue;
        }
        /* Visible text: collapse whitespace, decode the
         * common entities. Whitespace pending across a tag
         * boundary is kept in p_space_pending. */
        for (size_t k = rs; k < i; k++) {
            char ch = html[k];
            if (ch == ' ' || ch == '\t' || ch == '\n' ||
                ch == '\r') {
                p_space_pending = 1;
                continue;
            }
            if (ch == '&') {
                char ent[12];
                size_t en = 0;
                size_t m = k + 1;
                while (m < i && en + 1 < sizeof(ent) &&
                       html[m] != ';' && html[m] != ' ' &&
                       html[m] != '<') {
                    ent[en++] = html[m++];
                }
                if (m < i && html[m] == ';' && en > 0) {
                    ent[en] = '\0';
                    char rep = 0;
                    if (strcmp(ent, "amp") == 0) {
                        rep = '&';
                    } else if (strcmp(ent, "lt") == 0) {
                        rep = '<';
                    } else if (strcmp(ent, "gt") == 0) {
                        rep = '>';
                    } else if (strcmp(ent, "quot") == 0) {
                        rep = '"';
                    } else if (strcmp(ent, "nbsp") == 0) {
                        rep = ' ';
                    }
                    if (rep != 0) {
                        if (p_space_pending && p_len > 0) {
                            br_putc(' ');
                        }
                        p_space_pending = 0;
                        br_putc(rep);
                        k = m;
                        continue;
                    }
                }
                /* Unknown entity: keep it literally. */
            }
            if (p_space_pending && p_len > 0) {
                br_putc(' ');
            }
            p_space_pending = 0;
            if (font_color != 0 && p_color == br_css_color[elem]) {
                p_color = font_color;
            }
            br_putc(ch);
        }
    }
    br_flush_line();
    (void)link_open;
    (void)p_blank_pending;
}

/* Load the page in the address bar into the renderer. */
static void br_render_current(void);

static void browser_load(void) {
    br_free_images();
    br_line_count = 0;
    br_link_count = 0;
    br_focus = -1;
    browser_scroll = 0;
    br_page_bg = 0;
    br_page_fg = 0;
    br_title[0] = '\0';
    for (int s = 0; s < 6; s++) {
        br_css_color[s] = 0;
    }
    br_css_bg = 0;
    size_t len = 0;
    int status = 0;
    if (br_fetch(browser_url, &len, &status) != 0) {
        browser_state = -1;
        browser_status = status;
        serial_write("browser: ");
        serial_write_line(br_fetch_err);
        return;
    }
    browser_status = status;
    br_raw_len = len;
    br_render_current();
    browser_state = 1;
    serial_write("browser: fetched ");
    serial_write_line(browser_url);
}

/* 取得済みの br_raw を現在のズームで描き直す
 * (行の折返し幅が変わるため再パースする)。 */
static void br_render_current(void) {
    size_t len = br_raw_len;
    br_free_images();
    br_line_count = 0;
    br_link_count = 0;
    br_focus = -1;
    browser_scroll = 0;
    br_page_bg = 0;
    br_page_fg = 0;
    br_title[0] = '\0';
    for (int s = 0; s < 6; s++) {
        br_css_color[s] = 0;
    }
    br_css_bg = 0;
    br_wrap_cols = text_cols() / browser_zoom;
    br_url_parts base;
    (void)br_parse_url(browser_url, &base);
    if (png_is_png((const uint8_t *)br_raw, len)) {
        /* The address points straight at a PNG image. */
        if (br_register_png((const uint8_t *)br_raw, len, 0) == 0) {
            br_img_count = 1;
            br_insert_image(0);
        }
    } else if (len >= 2 && br_raw[0] == 'B' && br_raw[1] == 'M') {
        /* The address points straight at a BMP image. */
        if (wallpaper_parse((const uint8_t *)br_raw, len,
                            &br_imgs[0]) == 0) {
            br_img_count = 1;
            br_img_raw[0] = NULL; /* pixels point into br_raw */
            int w = br_imgs[0].width;
            int h = br_imgs[0].height;
            int dw = w > 480 ? 480 : w;
            int dh = (int)((int64_t)h * dw / (w > 0 ? w : 1));
            if (dh > 240) {
                dh = 240;
            }
            if (dw < 1) {
                dw = 1;
            }
            if (dh < 1) {
                dh = 1;
            }
            br_img_disp_w[0] = dw;
            br_img_disp_h[0] = dh;
            br_insert_image(0);
        }
    } else {
        br_parse_html(br_raw, len, &base);
    }
}

/* ページ内検索 (項目20): 行テキストを大文字小文字を
 * 区別せずに部分一致で探し、表示位置を移す。 */
static int br_find_run(void) {
    if (br_find_len == 0 || br_line_count == 0) {
        return -1;
    }
    for (int n = 0; n < br_line_count; n++) {
        int li = (br_find_last + 1 + n) % br_line_count;
        const char *t = br_lines[li].text;
        for (int s = 0; t[s] != '\0'; s++) {
            int ok = 1;
            for (int k = 0; k < br_find_len; k++) {
                char a = t[s + k];
                char b = br_find_buf[k];
                if (a == '\0') {
                    ok = 0;
                    break;
                }
                if (a >= 'A' && a <= 'Z') {
                    a = (char)(a - 'A' + 'a');
                }
                if (b >= 'A' && b <= 'Z') {
                    b = (char)(b - 'A' + 'a');
                }
                if (a != b) {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                br_find_last = li;
                browser_scroll = li;
                serial_write("browser: found at line ");
                serial_write_num(li);
                serial_write_line("");
                return li;
            }
        }
    }
    serial_write_line("browser: find: not found");
    desktop_notify(L("Not found", "見つかりませんでした"));
    return -1;
}

static char br_save_buf[16384];

/* 表示中のページをテキストで保存する (項目20)。
 * ファイル上限に合わせて途中で切れる場合はその旨を
 * シリアルに残す。 */
static void br_save_page(void) {
    size_t p = 0;
    const char *t = br_title[0] != '\0' ? br_title : browser_url;
    while (*t != '\0' && p + 2 < sizeof(br_save_buf)) {
        br_save_buf[p++] = *t++;
    }
    br_save_buf[p++] = '\n';
    const char *u = browser_url;
    while (*u != '\0' && p + 2 < sizeof(br_save_buf)) {
        br_save_buf[p++] = *u++;
    }
    br_save_buf[p++] = '\n';
    br_save_buf[p++] = '\n';
    int truncated = 0;
    for (int li = 0; li < br_line_count; li++) {
        if (br_lines[li].img >= 0) {
            continue;
        }
        const char *s = br_lines[li].text;
        while (*s != '\0') {
            if (p + 2 >= sizeof(br_save_buf)) {
                truncated = 1;
                break;
            }
            br_save_buf[p++] = *s++;
        }
        if (truncated || p + 2 >= sizeof(br_save_buf)) {
            truncated = 1;
            break;
        }
        br_save_buf[p++] = '\n';
    }
    char name[16];
    inst_copy(name, sizeof(name), "page.txt");
    if (vfs_read_ptr(name, NULL) != NULL) {
        for (int i = 1; i <= 9; i++) {
            name[4] = (char)('0' + i);
            name[5] = '\0';
            {
                const char *ext = ".txt";
                size_t q = 5;
                while (*ext != '\0' && q + 1 < sizeof(name)) {
                    name[q++] = *ext++;
                }
                name[q] = '\0';
            }
            if (vfs_read_ptr(name, NULL) == NULL) {
                break;
            }
            if (i == 9) {
                desktop_notify(L("Too many saved pages",
                                 "保存したページが多すぎます"));
                return;
            }
        }
    }
    if (vfs_write(name, br_save_buf, p) != 0) {
        desktop_notify(L("Save failed", "保存できませんでした"));
        return;
    }
    persist_maybe_sync();
    serial_write("browser: page saved ");
    serial_write(name);
    serial_write_line(truncated ? " (truncated)" : "");
    desktop_notify(L("Page saved", "ページを保存しました"));
}

static void br_navigate(const char *url) {
    size_t n = 0;
    while (url[n] != '\0' && n + 1 < sizeof(browser_url)) {
        browser_url[n] = url[n];
        n++;
    }
    browser_url[n] = '\0';
    browser_url_len = (int)n;
    browser_load();
    desktop_redraw();
}

/* ---- Browser drawing ---- */

static void draw_browser(void) {
    uint32_t fg = br_page_fg != 0 ? br_page_fg : col_text();
    uint32_t bg = br_page_bg != 0 ? br_page_bg : col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    char status[300];
    size_t p = 0;
    const char *h = L("Browser - ", "ブラウザ - ");
    while (*h != '\0' && p + 1 < sizeof(status)) {
        status[p++] = *h++;
    }
    const char *disp = br_title[0] != '\0' ? br_title : browser_url;
    while (*disp != '\0' && p + 1 < sizeof(status)) {
        status[p++] = *disp++;
    }
    status[p] = '\0';
    draw_text_small(x0, y0, status, acc, col_window_bg());
    if (browser_state == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("Type an address, Enter to fetch.", "アドレスを入力し、Enterで取得"),
                        col_text(), col_window_bg());
        draw_text_small(x0, y0 + (uint64_t)(3 * CELL_H),
                        L("Links: Tab selects, Enter opens, click works too.", "リンク: Tabで選択、Enterで開く、クリックでも可"),
                        col_dim(), col_window_bg());
        /* Show the address being edited. */
        char addr[300];
        size_t ap = 0;
        const char *pre = "> ";
        while (*pre != '\0' && ap + 1 < sizeof(addr)) {
            addr[ap++] = *pre++;
        }
        for (int i2 = 0; i2 < browser_url_len &&
                        ap + 1 < sizeof(addr);
             i2++) {
            addr[ap++] = browser_url[i2];
        }
        addr[ap] = '\0';
        draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H), addr,
                        col_text(), col_window_bg());
        return;
    }
    if (browser_state < 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        br_fetch_err, col_text(), col_window_bg());
        draw_text_small(x0, y0 + (uint64_t)(3 * CELL_H),
                        L("Edit the address and press Enter to retry.", "アドレスを直してEnterで再試行"),
                        col_dim(), col_window_bg());
        char addr[300];
        size_t ap = 0;
        const char *pre = "> ";
        while (*pre != '\0' && ap + 1 < sizeof(addr)) {
            addr[ap++] = *pre++;
        }
        for (int i2 = 0; i2 < browser_url_len &&
                        ap + 1 < sizeof(addr);
             i2++) {
            addr[ap++] = browser_url[i2];
        }
        addr[ap] = '\0';
        draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H), addr,
                        col_text(), col_window_bg());
        return;
    }
    if (br_page_bg != 0) {
        fb_fill_rect(x0 - 8, y0 + CELL_H, fb_width() - 2 * (x0 - 8),
                     (uint64_t)(text_rows() - 2) * CELL_H, bg);
    }
    int rows = (text_rows() - 3) / browser_zoom;
    if (rows < 1) {
        rows = 1;
    }
    if (browser_scroll > br_line_count - 1) {
        browser_scroll = br_line_count - 1;
    }
    if (browser_scroll < 0) {
        browser_scroll = 0;
    }
    for (int r = 0; r < rows; r++) {
        int li = browser_scroll + r;
        if (li >= br_line_count) {
            break;
        }
        br_line *ln = &br_lines[li];
        uint64_t py = y0 + (uint64_t)(2 * CELL_H) +
                      (uint64_t)r * (uint64_t)CELL_H *
                          (uint64_t)browser_zoom;
        if (ln->img >= 0) {
            if (ln->img_first && br_imgs[ln->img].pixels != NULL) {
                fb_draw_image_rgb24(
                    x0, py, (uint64_t)br_img_disp_w[ln->img],
                    (uint64_t)br_img_disp_h[ln->img],
                    br_imgs[ln->img].pixels, br_imgs[ln->img].width,
                    br_imgs[ln->img].height, br_imgs[ln->img].stride,
                    br_imgs[ln->img].top_down);
            }
            continue;
        }
        if (ln->text[0] == '\0') {
            continue;
        }
        uint32_t color = ln->color != 0 ? ln->color : fg;
        if (ln->link >= 0 && ln->link == br_focus) {
            fb_draw_text(x0, py, ln->text, bg, acc, browser_zoom);
        } else {
            fb_draw_text(x0, py, ln->text, color, bg,
                         browser_zoom);
            if (ln->link >= 0) {
                size_t tw = strlen(ln->text);
                fb_fill_rect(x0,
                             py + (uint64_t)CELL_H *
                                        (uint64_t)browser_zoom -
                                 3,
                             tw * 8 * (uint64_t)browser_zoom, 1,
                             color);
            }
        }
    }
    if (br_find_mode) {
        char fbuf[96];
        size_t fp3 = 0;
        const char *fp_pre = L("Find: ", "検索: ");
        while (*fp_pre != '\0' && fp3 + 1 < sizeof(fbuf)) {
            fbuf[fp3++] = *fp_pre++;
        }
        for (int i3 = 0; i3 < br_find_len && fp3 + 1 < sizeof(fbuf);
             i3++) {
            fbuf[fp3++] = br_find_buf[i3];
        }
        fbuf[fp3] = '\0';
        draw_text_small(x0,
                        y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                        fbuf, acc, col_window_bg());
    }
    char foot[300];
    size_t fp2 = 0;
    if (br_focus >= 0 && br_focus < br_link_count) {
        const char *pre = L("Link: ", "リンク: ");
        while (*pre != '\0' && fp2 + 1 < sizeof(foot)) {
            foot[fp2++] = *pre++;
        }
        const char *u = br_links[br_focus];
        while (*u != '\0' && fp2 + 1 < sizeof(foot)) {
            foot[fp2++] = *u++;
        }
    } else {
        const char *d = L("Up/Down scroll, Tab = link, Ctrl+F = find, Ctrl+S = save, Ctrl+/- = zoom", "上下でスクロール、Tab = リンク、Ctrl+F = 検索、Ctrl+S = 保存、Ctrl+/- = 拡大");
        while (*d != '\0' && fp2 + 1 < sizeof(foot)) {
            foot[fp2++] = *d++;
        }
    }
    foot[fp2] = '\0';
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    foot, col_dim(), col_window_bg());
}

/* ---- Notes: editable text file ---- */

static void notes_load(void) {
    if (notes_loaded) {
        return;
    }
    notes_loaded = 1;
    notes_len = 0;
    notes_cursor = 0;
    notes_dirty = 0;
    notes_saved = 0;
    if (notes_buf == NULL) {
        /* The load below and the insert guards both use
         * the file size limit (16384) as the text cap, so
         * the buffer must hold that plus the NUL. (This
         * was kmalloc(4097): opening a note.txt over 4KB
         * overflowed the heap.) */
        notes_buf = (char *)kmalloc(PRISM_VFS_MAX_APP_FILE_SIZE + 1);
        if (notes_buf == NULL) {
            return;
        }
    }
    notes_buf[0] = '\0';
    size_t len = 0;
    const char *content = vfs_read_ptr("note.txt", &len);
    if (content != NULL && len > 0) {
        size_t copy = len < PRISM_VFS_MAX_APP_FILE_SIZE ? len : PRISM_VFS_MAX_APP_FILE_SIZE;
        for (size_t i = 0; i < copy; i++) {
            notes_buf[i] = content[i];
        }
        notes_len = copy;
        notes_buf[notes_len] = '\0';
        notes_cursor = notes_len;
    }
}

static int notes_save(void) {
    if (notes_buf == NULL) {
        return -1;
    }
    if (vfs_write("note.txt", notes_buf, notes_len) != 0) {
        return -1;
    }
    notes_dirty = 0;
    notes_saved = 1;
    (void)persist_maybe_sync();
    desktop_notify(L("Notes saved", "メモを保存しました"));
    return 0;
}

/* ---- Notes の取り消し/やり直しと検索・置換 ---- */
#define NOTES_UNDO_DEPTH 8
static char *notes_undo_bufs[NOTES_UNDO_DEPTH];
static size_t notes_undo_lens[NOTES_UNDO_DEPTH];
static size_t notes_undo_curs[NOTES_UNDO_DEPTH];
static int notes_undo_count;
static char *notes_redo_buf;
static size_t notes_redo_len;
static size_t notes_redo_cur;
static int notes_redo_valid;
static int notes_undo_group; /* 0 無し, 1 挿入, 2 削除 */
static int notes_undo_run;   /* 前回の保存からの文字数 */

/* 変更前に現在を保存する。同種の変更が続く間は
 * 24 文字ごとに 1 回だけ保存して履歴を粗く保つ。 */
static void notes_undo_push(int group) {
    if (notes_buf == NULL) {
        return;
    }
    if (group == notes_undo_group && notes_undo_count > 0 &&
        notes_undo_run < 24) {
        notes_undo_run++;
        return;
    }
    if (notes_undo_count > 0) {
        int top = notes_undo_count - 1;
        if (notes_undo_lens[top] == notes_len &&
            memcmp(notes_undo_bufs[top], notes_buf,
                   notes_len) == 0) {
            notes_undo_group = group;
            notes_undo_run = 0;
            return;
        }
    }
    if (notes_undo_count == NOTES_UNDO_DEPTH) {
        kfree(notes_undo_bufs[0]);
        for (int i = 1; i < NOTES_UNDO_DEPTH; i++) {
            notes_undo_bufs[i - 1] = notes_undo_bufs[i];
            notes_undo_lens[i - 1] = notes_undo_lens[i];
            notes_undo_curs[i - 1] = notes_undo_curs[i];
        }
        notes_undo_count--;
    }
    char *copy = (char *)kmalloc(PRISM_VFS_MAX_APP_FILE_SIZE + 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, notes_buf, notes_len + 1);
    notes_undo_bufs[notes_undo_count] = copy;
    notes_undo_lens[notes_undo_count] = notes_len;
    notes_undo_curs[notes_undo_count] = notes_cursor;
    notes_undo_count++;
    notes_undo_group = group;
    notes_undo_run = 0;
    notes_redo_valid = 0;
}

static void notes_undo_apply(void) {
    if (notes_undo_count == 0 || notes_buf == NULL) {
        desktop_notify(L("Nothing to undo", "取り消せる操作がありません"));
        return;
    }
    if (notes_redo_buf == NULL) {
        notes_redo_buf =
            (char *)kmalloc(PRISM_VFS_MAX_APP_FILE_SIZE + 1);
    }
    if (notes_redo_buf != NULL) {
        memcpy(notes_redo_buf, notes_buf, notes_len + 1);
        notes_redo_len = notes_len;
        notes_redo_cur = notes_cursor;
        notes_redo_valid = 1;
    }
    notes_undo_count--;
    memcpy(notes_buf, notes_undo_bufs[notes_undo_count],
           notes_undo_lens[notes_undo_count] + 1);
    notes_len = notes_undo_lens[notes_undo_count];
    notes_cursor = notes_undo_curs[notes_undo_count];
    kfree(notes_undo_bufs[notes_undo_count]);
    notes_undo_group = 0;
    notes_undo_run = 0;
    notes_dirty = 1;
    notes_saved = 0;
    serial_write_line("notes: undo");
    desktop_refresh_console();
}

static void notes_redo_apply(void) {
    if (!notes_redo_valid || notes_buf == NULL ||
        notes_redo_buf == NULL) {
        desktop_notify(L("Nothing to redo", "やり直せる操作がありません"));
        return;
    }
    /* 現在を undo 側へ戻してから redo を適用する。 */
    if (notes_undo_count == NOTES_UNDO_DEPTH) {
        kfree(notes_undo_bufs[0]);
        for (int i = 1; i < NOTES_UNDO_DEPTH; i++) {
            notes_undo_bufs[i - 1] = notes_undo_bufs[i];
            notes_undo_lens[i - 1] = notes_undo_lens[i];
            notes_undo_curs[i - 1] = notes_undo_curs[i];
        }
        notes_undo_count--;
    }
    char *copy = (char *)kmalloc(PRISM_VFS_MAX_APP_FILE_SIZE + 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, notes_buf, notes_len + 1);
    notes_undo_bufs[notes_undo_count] = copy;
    notes_undo_lens[notes_undo_count] = notes_len;
    notes_undo_curs[notes_undo_count] = notes_cursor;
    notes_undo_count++;
    memcpy(notes_buf, notes_redo_buf, notes_redo_len + 1);
    notes_len = notes_redo_len;
    notes_cursor = notes_redo_cur;
    notes_redo_valid = 0;
    notes_undo_group = 0;
    notes_undo_run = 0;
    notes_dirty = 1;
    notes_saved = 0;
    serial_write_line("notes: redo");
    desktop_refresh_console();
}

/* 検索・置換の入力状態。stage 1 = 検索語、2 = 置換語。 */
static int notes_find_stage;
static int notes_find_replace; /* 置換モードなら 1 */
static char notes_find_buf[64];
static size_t notes_find_len;
static char notes_find_term[64];
static char notes_repl_buf[64];
static size_t notes_repl_len;

static int notes_find_next(void) {
    size_t tl = 0;
    while (notes_find_term[tl] != '\0') {
        tl++;
    }
    if (tl == 0 || notes_buf == NULL) {
        return -1;
    }
    for (size_t start = notes_cursor;;) {
        size_t i = start;
        int found = 0;
        while (i + tl <= notes_len) {
            if (memcmp(notes_buf + i, notes_find_term, tl) == 0) {
                found = 1;
                break;
            }
            i++;
        }
        if (found) {
            notes_cursor = i + tl;
            return (int)i;
        }
        if (start == 0) {
            return -1;
        }
        start = 0; /* 先頭から探し直す */
    }
}

static int notes_replace_all(void) {
    size_t tl = 0, rl = 0;
    while (notes_find_term[tl] != '\0') {
        tl++;
    }
    while (notes_repl_buf[rl] != '\0') {
        rl++;
    }
    if (tl == 0 || notes_buf == NULL) {
        return 0;
    }
    notes_undo_push(1);
    static char tmp[PRISM_VFS_MAX_APP_FILE_SIZE + 1];
    size_t out = 0;
    size_t i = 0;
    int count = 0;
    while (i < notes_len) {
        if (i + tl <= notes_len &&
            memcmp(notes_buf + i, notes_find_term, tl) == 0) {
            if (out + rl > PRISM_VFS_MAX_APP_FILE_SIZE) {
                break;
            }
            memcpy(tmp + out, notes_repl_buf, rl);
            out += rl;
            i += tl;
            count++;
        } else {
            if (out + 1 > PRISM_VFS_MAX_APP_FILE_SIZE) {
                break;
            }
            tmp[out++] = notes_buf[i++];
        }
    }
    tmp[out] = '\0';
    memcpy(notes_buf, tmp, out + 1);
    notes_len = out;
    if (count > 0) {
        notes_dirty = 1;
        notes_saved = 0;
        if (notes_cursor > notes_len) {
            notes_cursor = notes_len;
        }
    }
    return count;
}

static void draw_notes(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    notes_load();
    draw_text_small(x0, y0, L("Notes - note.txt", "メモ - note.txt"), acc, bg);
    if (notes_buf == NULL) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("(no memory for the editor)", "(エディタ用のメモリ不足)"), fg, bg);
        return;
    }
    int cols = text_cols();
    int rows = text_rows() - 3;
    /* Find the row/col of the cursor by walking the text. */
    int cur_row = 0;
    int cur_col = 0;
    for (size_t i = 0; i < notes_cursor; i++) {
        if (notes_buf[i] == '\n') {
            cur_row++;
            cur_col = 0;
        } else if (cur_col + 1 >= cols) {
            cur_row++;
            cur_col = 0;
        } else {
            cur_col++;
        }
    }
    int top_row = 0;
    if (cur_row >= rows) {
        top_row = cur_row - rows + 1;
    }
    size_t pos = 0;
    int row = 0;
    while (pos < notes_len && row < top_row + rows) {
        if (row >= top_row) {
            char line[128];
            int n = 0;
            size_t p = pos;
            while (p < notes_len && notes_buf[p] != '\n' &&
                   n < cols && n + 1 < (int)sizeof(line)) {
                line[n++] = notes_buf[p++];
            }
            line[n] = '\0';
            uint64_t py = y0 + (uint64_t)((row - top_row + 2) * CELL_H);
            fb_draw_text(x0, py, line, fg, bg, 1);
            /* Cursor block on its row. */
            if (row == cur_row) {
                uint64_t cx = x0 + (uint64_t)(cur_col * CELL_W);
                fb_draw_char(cx, py, ' ', bg, fg, TEXT_SCALE);
            }
        }
        while (pos < notes_len && notes_buf[pos] != '\n') {
            pos++;
        }
        if (pos < notes_len) {
            pos++; /* newline */
        }
        row++;
    }
    if (notes_len == 0 || cur_row == row) {
        uint64_t py = y0 + (uint64_t)((cur_row - top_row + 2) * CELL_H);
        uint64_t cx = x0 + (uint64_t)(cur_col * CELL_W);
        fb_draw_char(cx, py, ' ', bg, fg, TEXT_SCALE);
    }
    char foot[256];
    size_t p = 0;
    const char *f = notes_dirty ? L("F6 = save (unsaved changes)", "F6 = 保存 (未保存の変更あり)")
                                : (notes_saved ? L("Saved to note.txt", "note.txtに保存しました")
                                               : L("F6 = save", "F6 = 保存"));
    while (*f != '\0' && p + 1 < sizeof(foot)) {
        foot[p++] = *f++;
    }
    if (ime_on()) {
        const char *tag = "  [IME F9";
        while (*tag != '\0' && p + 1 < sizeof(foot)) {
            foot[p++] = *tag++;
        }
        if (ime_active()) {
            const char *d = ime_display();
            foot[p++] = ':';
            foot[p++] = ' ';
            while (*d != '\0' && p + 1 < sizeof(foot)) {
                foot[p++] = *d++;
            }
            if (ime_candidate_count() > 0) {
                char num[16];
                int ni = 0;
                int ci = ime_candidate_index() + 1;
                int cc = ime_candidate_count();
                num[ni++] = ' ';
                num[ni++] = '(';
                num[ni++] = (char)('0' + ci);
                num[ni++] = '/';
                num[ni++] = (char)('0' + cc);
                num[ni++] = ')';
                num[ni] = '\0';
                const char *np = num;
                while (*np != '\0' && p + 1 < sizeof(foot)) {
                    foot[p++] = *np++;
                }
            }
        }
        if (p + 1 < sizeof(foot)) {
            foot[p++] = ']';
        }
    }
    foot[p] = '\0';
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H), foot,
                    notes_dirty ? acc : col_dim(), bg);
    if (notes_find_stage > 0) {
        char line[128];
        size_t q = 0;
        const char *pre = notes_find_stage == 1
                              ? (notes_find_replace ? L("Replace: ", "置換: ")
                                                    : L("Find: ", "検索: "))
                              : L("With: ", "置換後: ");
        while (*pre != '\0' && q + 1 < sizeof(line)) {
            line[q++] = *pre++;
        }
        const char *buf = notes_find_stage == 1 ? notes_find_buf
                                                : notes_repl_buf;
        size_t bl = notes_find_stage == 1 ? notes_find_len
                                          : notes_repl_len;
        for (size_t i = 0; i < bl && q + 1 < sizeof(line); i++) {
            line[q++] = buf[i];
        }
        if (q + 1 < sizeof(line)) {
            line[q++] = '_';
        }
        line[q] = '\0';
        fb_fill_rect(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                     win_w() - 16, CELL_H, bg);
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                        line, acc, bg);
    }
}

/* Notes の IME キー処理。確定文字列があればカーソル
 * 位置へ挿入する。IME が消費したら 1、通常処理へ
 * 回すべきキーなら 0 を返す。 */
static void notes_insert_text(const char *text) {
    notes_undo_push(1);
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (notes_len + 1 < PRISM_VFS_MAX_APP_FILE_SIZE) {
            for (size_t j = notes_len; j > notes_cursor; j--) {
                notes_buf[j] = notes_buf[j - 1];
            }
            notes_buf[notes_cursor++] = text[i];
            notes_len++;
            notes_buf[notes_len] = '\0';
            notes_dirty = 1;
            notes_saved = 0;
        }
    }
}

/* 共通 IME キー処理: 確定した文字列を挿入関数へ渡す。
 * IME が消費したら 1、通常処理へ回すキーなら 0。
 * Notes / エディタ / Docs / メール作成 / 検索欄で共用。 */
static int ime_key_common(int c, void (*insert)(const char *)) {
    if (!ime_on()) {
        return 0;
    }
    char out[192];
    if (c >= 'a' && c <= 'z') {
        if (ime_feed(c, out, sizeof(out))) {
            insert(out);
            serial_write("ime: commit\n");
        }
        desktop_refresh_console();
        return 1;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\b' ||
        c == 0x7F) {
        if (!ime_active()) {
            return 0;
        }
        if (ime_feed(c, out, sizeof(out))) {
            insert(out);
            serial_write("ime: commit\n");
        }
        desktop_refresh_console();
        return 1;
    }
    return 0;
}

static int notes_ime_key(int c) {
    return ime_key_common(c, notes_insert_text);
}

/* ---- Docs: formatted document editor ---- */

static doc_state docs_doc;
static int docs_loaded;
static int docs_dirty;
static int docs_saved;
static int docs_fmt_pending; /* '\' typed, next key sets the format */
static char docs_status[96];

/* Docs の取り消し履歴 (文書全体のスナップショット)。 */
#define DOCS_UNDO_DEPTH 6
static doc_state docs_undo_stack[DOCS_UNDO_DEPTH];
static int docs_undo_count;
static doc_state docs_redo_state;
static int docs_redo_valid;

static void docs_history_push(void) {
    if (docs_undo_count > 0) {
        doc_state *top = &docs_undo_stack[docs_undo_count - 1];
        if (top->len == docs_doc.len &&
            memcmp(top->buf, docs_doc.buf, docs_doc.len) == 0) {
            return;
        }
    }
    if (docs_undo_count == DOCS_UNDO_DEPTH) {
        for (int i = 1; i < DOCS_UNDO_DEPTH; i++) {
            docs_undo_stack[i - 1] = docs_undo_stack[i];
        }
        docs_undo_count--;
    }
    docs_undo_stack[docs_undo_count++] = docs_doc;
    docs_redo_valid = 0;
}

static void docs_undo_apply(void) {
    if (docs_undo_count == 0) {
        desktop_notify(L("Nothing to undo",
                         "取り消せる操作がありません"));
        return;
    }
    docs_redo_state = docs_doc;
    docs_redo_valid = 1;
    docs_doc = docs_undo_stack[--docs_undo_count];
    docs_dirty = 1;
    docs_saved = 0;
    serial_write_line("docs: undo");
    desktop_refresh_console();
}

/* Docs の検索置換 (Ctrl+F)。入力は docs_status 行に
 * 映しながら受け付ける。 */
static int docs_fr_stage; /* 0 = off, 1 = find, 2 = replace */
static char docs_fr_find[48];
static char docs_fr_repl[48];
static int docs_fr_len;
static char docs_fr_buf[DOC_MAX + 1];

static void docs_fr_show(void) {
    const char *lab = docs_fr_stage == 1
        ? L("Find: ", "検索: ")
        : L("Replace with: ", "置換: ");
    const char *val = docs_fr_stage == 1 ? docs_fr_find
                                         : docs_fr_repl;
    size_t p = 0;
    while (*lab != '\0' && p + 1 < sizeof(docs_status)) {
        docs_status[p++] = *lab++;
    }
    while (*val != '\0' && p + 1 < sizeof(docs_status)) {
        docs_status[p++] = *val++;
    }
    docs_status[p] = '\0';
}

static void docs_fr_execute(void) {
    size_t fl = 0;
    while (docs_fr_find[fl] != '\0') {
        fl++;
    }
    if (fl == 0 || docs_doc.len == 0) {
        return;
    }
    docs_history_push();
    size_t op = 0;
    int replaced = 0;
    size_t k = 0;
    while (k < docs_doc.len) {
        size_t m = 0;
        while (m < fl && k + m < docs_doc.len &&
               docs_doc.buf[k + m] == docs_fr_find[m]) {
            m++;
        }
        if (m == fl) {
            replaced++;
            for (size_t r = 0; docs_fr_repl[r] != '\0' &&
                               op < DOC_MAX;
                 r++) {
                docs_fr_buf[op++] = docs_fr_repl[r];
            }
            k += fl;
        } else {
            if (op < DOC_MAX) {
                docs_fr_buf[op++] = docs_doc.buf[k];
            }
            k++;
        }
    }
    docs_fr_buf[op] = '\0';
    for (size_t i = 0; i <= op; i++) {
        docs_doc.buf[i] = docs_fr_buf[i];
    }
    docs_doc.len = op;
    if (docs_doc.cursor > op) {
        docs_doc.cursor = op;
    }
    if (replaced > 0) {
        docs_dirty = 1;
        docs_saved = 0;
    }
    serial_write("docs: replaced ");
    serial_write_num(replaced);
    serial_write_line("");
    desktop_notify(replaced > 0 ? L("Replaced",
                                    "置換しました")
                                : L("Not found",
                                    "見つかりませんでした"));
}

static int docs_fr_key(int c) {
    char *buf = docs_fr_stage == 1 ? docs_fr_find : docs_fr_repl;
    if (c == 0x1B) {
        docs_fr_stage = 0;
        docs_status[0] = '\0';
        desktop_refresh_console();
        return 1;
    }
    if (c == '\n' || c == '\r') {
        buf[docs_fr_len] = '\0';
        if (docs_fr_stage == 1) {
            docs_fr_stage = 2;
            docs_fr_len = 0;
            while (docs_fr_repl[docs_fr_len] != '\0') {
                docs_fr_len++;
            }
            docs_fr_show();
        } else {
            docs_fr_stage = 0;
            docs_status[0] = '\0';
            docs_fr_execute();
        }
        desktop_refresh_console();
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        if (docs_fr_len > 0) {
            buf[--docs_fr_len] = '\0';
        }
        docs_fr_show();
        desktop_refresh_console();
        return 1;
    }
    if (c >= 32 && c <= 126 && docs_fr_len < 47) {
        buf[docs_fr_len++] = (char)c;
        buf[docs_fr_len] = '\0';
        docs_fr_show();
        desktop_refresh_console();
        return 1;
    }
    return 1;
}

static void docs_redo_apply(void) {
    if (!docs_redo_valid) {
        desktop_notify(L("Nothing to redo",
                         "やり直せる操作がありません"));
        return;
    }
    if (docs_undo_count < DOCS_UNDO_DEPTH) {
        docs_undo_stack[docs_undo_count++] = docs_doc;
    }
    docs_doc = docs_redo_state;
    docs_redo_valid = 0;
    docs_dirty = 1;
    docs_saved = 0;
    serial_write_line("docs: redo");
    desktop_refresh_console();
}

/* Docs への IME 確定文字列の挿入。 */
static void docs_insert_text(const char *text) {
    docs_history_push();
    for (size_t i = 0; text[i] != '\0'; i++) {
        doc_insert_char(&docs_doc, text[i]);
    }
    docs_dirty = 1;
    docs_saved = 0;
}

/* Write a decimal number to serial (for log lines). */
static void serial_write_num(int v) {
    char num[21];
    int ni = 0;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[21];
        int ri = 0;
        int x = v;
        while (x > 0 && ri < 20) {
            rev[ri++] = (char)('0' + (x % 10));
            x /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    serial_write(num);
}

/* Append a decimal number to a buffer. */
static void buf_add_num(char *buf, size_t *p, size_t cap, int v) {
    char rev[12];
    int ri = 0;
    if (v == 0) {
        rev[ri++] = '0';
    } else {
        int x = v;
        while (x > 0 && ri < 11) {
            rev[ri++] = (char)('0' + (x % 10));
            x /= 10;
        }
    }
    while (ri > 0 && *p + 1 < cap) {
        buf[(*p)++] = rev[--ri];
    }
    buf[*p] = '\0';
}

static void docs_load(void) {
    if (docs_loaded) {
        return;
    }
    docs_loaded = 1;
    docs_dirty = 0;
    docs_saved = 0;
    docs_fmt_pending = 0;
    docs_status[0] = '\0';
    doc_clear(&docs_doc);
    size_t len = 0;
    const char *content = vfs_read_ptr("doc.txt", &len);
    if (content != NULL && len > 0) {
        doc_load(&docs_doc, content, len);
    }
}

static int docs_save(void) {
    if (vfs_write("doc.txt", docs_doc.buf, docs_doc.len) != 0) {
        serial_write_line("docs: save failed");
        return -1;
    }
    docs_dirty = 0;
    docs_saved = 1;
    (void)persist_maybe_sync();
    serial_write_line("docs: saved doc.txt");
    return 0;
}

static int docs_export(void) {
    static uint8_t out[PRISM_VFS_MAX_APP_FILE_SIZE];
    int n = docx_export(docs_doc.buf, docs_doc.len, out, sizeof(out));
    if (n < 0) {
        serial_write_line("docs: export failed (too big for the file limit)");
        return -1;
    }
    if (vfs_write("doc.docx", (const char *)out, (size_t)n) != 0) {
        serial_write_line("docs: export failed (write)");
        return -1;
    }
    (void)persist_maybe_sync();
    serial_write("docs: exported doc.docx (");
    serial_write_num(n);
    serial_write_line(" bytes)");
    return 0;
}

static int docs_import(void) {
    size_t len = 0;
    const char *content = vfs_read_ptr("doc.docx", &len);
    if (content == NULL || len == 0) {
        serial_write_line("docs: import failed (no doc.docx)");
        return -1;
    }
    static char text[PRISM_VFS_MAX_APP_FILE_SIZE];
    int n = docx_import((const uint8_t *)content, len, text,
                        sizeof(text));
    if (n < 0) {
        serial_write_line("docs: import failed");
        return -1;
    }
    doc_load(&docs_doc, text, (size_t)n);
    docs_dirty = 1;
    docs_saved = 0;
    serial_write_line("docs: imported doc.docx");
    return 0;
}

/* Draw one document line's bold segments. Returns the x after
 * the text. Bold segments are drawn twice (offset by the
 * scale) so they look bolder. */
static uint64_t draw_doc_segments(uint64_t x, uint64_t y,
                                  const char *text, size_t len,
                                  uint32_t fg, uint32_t bg, int scale) {
    size_t pos = 0;
    uint64_t cx = x;
    while (pos < len) {
        size_t ss = 0;
        size_t sl = 0;
        int bold = 0;
        if (!doc_next_segment(text, len, pos, &ss, &sl, &bold)) {
            break;
        }
        char seg[DOC_LINE_MAX + 1];
        size_t n = sl < sizeof(seg) - 1 ? sl : sizeof(seg) - 1;
        for (size_t i = 0; i < n; i++) {
            seg[i] = text[ss + i];
        }
        seg[n] = '\0';
        if (bold) {
            fb_draw_text(cx, y, seg, fg, bg, scale);
            fb_draw_text(cx + (uint64_t)scale, y, seg, fg, bg, scale);
            cx += (uint64_t)n * 6 * (uint64_t)scale + (uint64_t)scale;
        } else {
            fb_draw_text(cx, y, seg, fg, bg, scale);
            cx += (uint64_t)n * 6 * (uint64_t)scale;
        }
        pos = ss + sl + (bold ? 2 : 0);
    }
    return cx;
}

static void draw_docs(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    docs_load();
    draw_text_small(x0, y0, L("Docs - doc.txt", "ドキュメント - doc.txt"), acc, bg);
    int cols = text_cols();
    int rows = text_rows() - 3;
    if (rows < 1) {
        rows = 1;
    }
    size_t total_rows = doc_line_count(&docs_doc);
    size_t cur_row = doc_cursor_row(&docs_doc);
    size_t cur_col = doc_cursor_col(&docs_doc);
    /* Display height of each line (H1 takes 2 rows). Work out
     * the cursor's display row, then the first display row. */
    size_t cur_disp = 0;
    for (size_t r = 0; r < cur_row && r < total_rows; r++) {
        size_t s = 0;
        size_t e = 0;
        if (!doc_line_bounds(&docs_doc, r, &s, &e)) {
            break;
        }
        char tmp[8];
        size_t tl = e - s < sizeof(tmp) - 1 ? e - s : sizeof(tmp) - 1;
        for (size_t i = 0; i < tl; i++) {
            tmp[i] = docs_doc.buf[s + i];
        }
        tmp[tl] = '\0';
        cur_disp += doc_classify_line(tmp) == DOC_LINE_H1 ? 2 : 1;
    }
    size_t top_disp = 0;
    if (cur_disp >= (size_t)rows) {
        top_disp = cur_disp - (size_t)rows + 1;
    }
    size_t disp = 0;
    for (size_t r = 0; r < total_rows; r++) {
        size_t s = 0;
        size_t e = 0;
        if (!doc_line_bounds(&docs_doc, r, &s, &e)) {
            break;
        }
        char line[DOC_LINE_MAX + 1];
        size_t ll = e - s;
        if (ll > (size_t)cols) {
            ll = (size_t)cols;
        }
        if (ll > sizeof(line) - 1) {
            ll = sizeof(line) - 1;
        }
        for (size_t i = 0; i < ll; i++) {
            line[i] = docs_doc.buf[s + i];
        }
        line[ll] = '\0';
        doc_line_type type = doc_classify_line(line);
        size_t marker = doc_line_marker_len(line);
        int scale = type == DOC_LINE_H1 ? 2 : 1;
        size_t height = type == DOC_LINE_H1 ? 2 : 1;
        if (disp + height > top_disp && disp < top_disp + (size_t)rows) {
            uint64_t py =
                y0 + (uint64_t)((disp - top_disp + 2) * CELL_H);
            uint32_t c = fg;
            if (type == DOC_LINE_H1 || type == DOC_LINE_H3) {
                c = acc;
            }
            const char *text = line + marker;
            size_t tlen = ll - marker;
            if (type == DOC_LINE_H2) {
                /* H2: bold (drawn twice in the segment helper
                 * when the line has ** marks; here the whole
                 * line is drawn bold by drawing it twice). */
                draw_doc_segments(x0, py, text, tlen, c, bg, 1);
                draw_doc_segments(x0 + 1, py, text, tlen, c, bg, 1);
            } else {
                draw_doc_segments(x0, py, text, tlen, c, bg, scale);
            }
            if (r == cur_row) {
                size_t dcol = cur_col;
                if (marker > 0) {
                    dcol = cur_col >= marker ? cur_col - marker : 0;
                }
                if (dcol >= (size_t)cols) {
                    dcol = (size_t)cols - 1;
                }
                uint64_t cx =
                    x0 + (uint64_t)(dcol * 6 * (uint64_t)scale);
                fb_draw_char(cx, py, ' ', bg, fg, scale);
            }
        }
        disp += height;
    }
    if (total_rows == 0 || cur_row == total_rows) {
        uint64_t py = y0 + (uint64_t)((cur_disp - top_disp + 2) * CELL_H);
        fb_draw_char(x0, py, ' ', bg, fg, 1);
    }
    if (docs_status[0] != '\0') {
        draw_text_small(x0,
                        y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                        docs_status, acc, bg);
    }
    const char *foot;
    if (docs_fmt_pending) {
        foot = L("Format: 1=H1 2=H2 3=H3 -=bullet 0=plain", "書式: 1=見出し1 2=見出し2 3=見出し3 -=箇条書き 0=通常");
    } else if (docs_dirty) {
        foot = "F6 = save (unsaved)  \\ + key = format  Ctrl+E = .docx";
    } else if (docs_saved) {
        foot = "Saved to doc.txt  \\ + key = format  Ctrl+E = .docx";
    } else {
        foot = "F6 = save  \\ + key = format  Ctrl+E = .docx  Ctrl+R = import";
    }
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H), foot,
                    docs_dirty ? acc : col_dim(), bg);
}

/* ---- Slides: presentation editor + slideshow ---- */

static slides_state slides_deck;
static int slides_current;
static int slides_loaded;
static int slides_editing;
static int slides_dirty;
static int slides_saved;
static int slides_field; /* 0 = title, 1.. = bullet lines */

/* Slides の取り消し履歴 (デッキ全体のスナップショット)。 */
#define SLIDES_UNDO_DEPTH 4
static slides_state slides_undo_stack[SLIDES_UNDO_DEPTH];
static int slides_undo_count;
static slides_state slides_redo_state;
static int slides_redo_valid;

static void slides_history_push(void) {
    if (slides_undo_count > 0 &&
        memcmp(&slides_undo_stack[slides_undo_count - 1],
               &slides_deck, sizeof(slides_state)) == 0) {
        return;
    }
    if (slides_undo_count == SLIDES_UNDO_DEPTH) {
        for (int i = 1; i < SLIDES_UNDO_DEPTH; i++) {
            slides_undo_stack[i - 1] = slides_undo_stack[i];
        }
        slides_undo_count--;
    }
    slides_undo_stack[slides_undo_count++] = slides_deck;
    slides_redo_valid = 0;
}

static void slides_undo_apply(void) {
    if (slides_undo_count == 0) {
        desktop_notify(L("Nothing to undo",
                         "取り消せる操作がありません"));
        return;
    }
    slides_redo_state = slides_deck;
    slides_redo_valid = 1;
    slides_deck = slides_undo_stack[--slides_undo_count];
    if (slides_current >= slides_deck.count) {
        slides_current = slides_deck.count > 0
                             ? slides_deck.count - 1
                             : 0;
    }
    slides_field = 0;
    slides_editing = 0;
    slides_dirty = 1;
    slides_saved = 0;
    serial_write_line("slides: undo");
    desktop_redraw();
}

static void slides_redo_apply(void) {
    if (!slides_redo_valid) {
        desktop_notify(L("Nothing to redo",
                         "やり直せる操作がありません"));
        return;
    }
    if (slides_undo_count < SLIDES_UNDO_DEPTH) {
        slides_undo_stack[slides_undo_count++] = slides_deck;
    }
    slides_deck = slides_redo_state;
    slides_redo_valid = 0;
    if (slides_current >= slides_deck.count) {
        slides_current = slides_deck.count > 0
                             ? slides_deck.count - 1
                             : 0;
    }
    slides_field = 0;
    slides_editing = 0;
    slides_dirty = 1;
    slides_saved = 0;
    serial_write_line("slides: redo");
    desktop_redraw();
}
static char slides_edit[SLIDES_LINE_MAX + 1];
static int slides_edit_len;
static int slides_show;
static char slides_status[96];
static char slides_io_buf[PRISM_VFS_MAX_APP_FILE_SIZE];

static void slides_load(void) {
    if (slides_loaded) {
        return;
    }
    slides_loaded = 1;
    slides_current = 0;
    slides_field = 0;
    slides_editing = 0;
    slides_edit_len = 0;
    slides_show = 0;
    slides_dirty = 0;
    slides_saved = 0;
    slides_status[0] = '\0';
    slides_clear(&slides_deck);
    size_t len = 0;
    const char *content = vfs_read_ptr("slides.txt", &len);
    if (content != NULL && len > 0) {
        if (slides_parse(&slides_deck, content, len) <= 0) {
            slides_clear(&slides_deck);
        }
    }
}

/* Copy the current field's text into the edit buffer. */
static void slides_edit_start(void) {
    const slide *sl = &slides_deck.slides[slides_current];
    const char *src = "";
    if (slides_field == 0) {
        src = sl->title;
    } else if (slides_field - 1 < sl->line_count) {
        src = sl->lines[slides_field - 1];
    }
    size_t n = 0;
    while (src[n] != '\0' && n < sizeof(slides_edit) - 1) {
        slides_edit[n] = src[n];
        n++;
    }
    slides_edit[n] = '\0';
    slides_edit_len = (int)n;
    slides_editing = 1;
}

static void slides_commit_edit(void) {
    if (!slides_editing) {
        return;
    }
    slides_history_push();
    slides_editing = 0;
    slides_edit[slides_edit_len] = '\0';
    const slide *sl = &slides_deck.slides[slides_current];
    if (slides_field == 0) {
        slides_set_title(&slides_deck, slides_current, slides_edit);
    } else if (slides_field - 1 < sl->line_count) {
        slides_set_line(&slides_deck, slides_current, slides_field - 1,
                        slides_edit);
    } else if (slides_edit_len > 0) {
        slides_add_line(&slides_deck, slides_current, slides_edit);
    }
    slides_dirty = 1;
    slides_saved = 0;
}

static int slides_save(void) {
    slides_commit_edit();
    int n = slides_serialize(&slides_deck, slides_io_buf,
                             sizeof(slides_io_buf));
    if (n < 0) {
        serial_write_line("slides: too big to save (slides.txt)");
        return -1;
    }
    if (vfs_write("slides.txt", slides_io_buf, (size_t)n) != 0) {
        serial_write_line("slides: save failed");
        return -1;
    }
    slides_dirty = 0;
    slides_saved = 1;
    (void)persist_maybe_sync();
    serial_write_line("slides: saved slides.txt");
    return 0;
}

static int slides_export(void) {
    slides_commit_edit();
    int n = pptx_export(&slides_deck, (uint8_t *)slides_io_buf,
                        sizeof(slides_io_buf));
    if (n < 0) {
        serial_write_line(
            L("slides: export failed (too big for the file limit)", "スライド: 書き出し失敗 (ファイル上限超過)"));
        return -1;
    }
    if (vfs_write("slides.pptx", slides_io_buf, (size_t)n) != 0) {
        serial_write_line("slides: export failed (write)");
        return -1;
    }
    (void)persist_maybe_sync();
    serial_write("slides: exported slides.pptx (");
    serial_write_num(n);
    serial_write_line(" bytes)");
    return 0;
}

static int slides_import(void) {
    size_t len = 0;
    const char *content = vfs_read_ptr("slides.pptx", &len);
    if (content == NULL || len == 0) {
        serial_write_line("slides: import failed (no slides.pptx)");
        return -1;
    }
    /* Static: a deck is about 3.6 KB, too big for the stack. */
    static slides_state tmp;
    int n = pptx_import((const uint8_t *)content, len, &tmp);
    if (n <= 0) {
        serial_write_line("slides: import failed");
        return -1;
    }
    slides_deck = tmp;
    slides_current = 0;
    slides_field = 0;
    slides_editing = 0;
    slides_dirty = 1;
    slides_saved = 0;
    serial_write("slides: imported slides.pptx (");
    serial_write_num(n);
    serial_write_line(" slides)");
    return 0;
}

static void draw_slideshow(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    const slide *sl = &slides_deck.slides[slides_current];
    char title[41];
    size_t n = 0;
    while (sl->title[n] != '\0' && n < sizeof(title) - 1) {
        title[n] = sl->title[n];
        n++;
    }
    title[n] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(2 * CELL_H), title, acc, bg, 3);
    for (int i = 0; i < sl->line_count; i++) {
        char line[SLIDES_LINE_MAX + 3];
        size_t p = 0;
        line[p++] = '-';
        line[p++] = ' ';
        size_t k = 0;
        while (sl->lines[i][k] != '\0' && p + 1 < sizeof(line)) {
            line[p++] = sl->lines[i][k++];
        }
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)((6 + i * 2) * CELL_H), line, fg,
                     bg, 2);
    }
    char foot[64];
    size_t p = 0;
    const char *pre = L("Slide ", "スライド ");
    while (*pre != '\0' && p + 1 < sizeof(foot)) {
        foot[p++] = *pre++;
    }
    buf_add_num(foot, &p, sizeof(foot), slides_current + 1);
    const char *mid = " / ";
    while (*mid != '\0' && p + 1 < sizeof(foot)) {
        foot[p++] = *mid++;
    }
    buf_add_num(foot, &p, sizeof(foot), slides_deck.count);
    const char *tail = L(" - Left/Right = move, Esc = exit", " - 左右 = 移動、Esc = 終了");
    while (*tail != '\0' && p + 1 < sizeof(foot)) {
        foot[p++] = *tail++;
    }
    foot[p] = '\0';
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H), foot,
                    col_dim(), bg);
}

static void draw_slides(void) {
    if (slides_show) {
        draw_slideshow();
        return;
    }
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    slides_load();
    const slide *sl = &slides_deck.slides[slides_current];
    char head[48];
    size_t p = 0;
    const char *pre = L("Slides - slide ", "スライド - ");
    while (*pre != '\0' && p + 1 < sizeof(head)) {
        head[p++] = *pre++;
    }
    buf_add_num(head, &p, sizeof(head), slides_current + 1);
    const char *mid = " / ";
    while (*mid != '\0' && p + 1 < sizeof(head)) {
        head[p++] = *mid++;
    }
    buf_add_num(head, &p, sizeof(head), slides_deck.count);
    head[p] = '\0';
    draw_text_small(x0, y0, head, acc, bg);
    /* Title, scale 2. */
    const char *title =
        (slides_editing && slides_field == 0) ? slides_edit : sl->title;
    uint64_t tw =
        fb_draw_text(x0, y0 + (uint64_t)(2 * CELL_H), title, acc, bg, 2);
    if (slides_editing && slides_field == 0) {
        fb_draw_char(x0 + tw, y0 + (uint64_t)(2 * CELL_H), '_', fg, bg, 2);
    }
    /* Bullet lines. */
    for (int i = 0; i < sl->line_count; i++) {
        const char *text =
            (slides_editing && slides_field == i + 1) ? slides_edit
                                                      : sl->lines[i];
        char line[SLIDES_LINE_MAX + 3];
        size_t q = 0;
        line[q++] = '-';
        line[q++] = ' ';
        size_t k = 0;
        while (text[k] != '\0' && q + 1 < sizeof(line)) {
            line[q++] = text[k++];
        }
        line[q] = '\0';
        uint64_t py = y0 + (uint64_t)((5 + i) * CELL_H);
        if (slides_field == i + 1) {
            fb_fill_rect(x0, py, win_w() - 16, CELL_H, col_title_bg());
            fb_draw_text(x0, py, line, acc, col_title_bg(), 1);
            if (slides_editing) {
                uint64_t lw = fb_draw_text(x0, py, line, acc,
                                           col_title_bg(), 1);
                fb_draw_char(x0 + lw, py, '_', fg, col_title_bg(), 1);
            }
        } else {
            fb_draw_text(x0, py, line, fg, bg, 1);
        }
    }
    /* Empty slot for a new bullet line. */
    if (sl->line_count < SLIDES_LINES_MAX) {
        int field = sl->line_count + 1;
        uint64_t py = y0 + (uint64_t)((5 + sl->line_count) * CELL_H);
        if (slides_field == field) {
            const char *text = slides_editing ? slides_edit : "";
            char line[SLIDES_LINE_MAX + 3];
            size_t q = 0;
            line[q++] = '-';
            line[q++] = ' ';
            size_t k = 0;
            while (text[k] != '\0' && q + 1 < sizeof(line)) {
                line[q++] = text[k++];
            }
            line[q] = '\0';
            fb_fill_rect(x0, py, win_w() - 16, CELL_H, col_title_bg());
            uint64_t lw =
                fb_draw_text(x0, py, line, acc, col_title_bg(), 1);
            if (slides_editing) {
                fb_draw_char(x0 + lw, py, '_', fg, col_title_bg(), 1);
            }
        } else {
            draw_text_small(x0, py, "- ", col_dim(), bg);
        }
    }
    if (slides_status[0] != '\0') {
        draw_text_small(x0,
                        y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                        slides_status, acc, bg);
    }
    const char *foot =
        slides_dirty
            ? "Tab = next field  Enter = new bullet  Ctrl+N = new slide  Ctrl+P = show  F6 = save (unsaved)"
            : "Tab = next field  Enter = new bullet  Ctrl+N = new slide  Ctrl+P = show  F6 = save";
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H), foot,
                    slides_dirty ? acc : col_dim(), bg);
}

/* ---- Calendar: real date from the clock ---- */

static void draw_calendar(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        draw_text_small(x0, y0, L("Calendar - (no clock)", "カレンダー - (時計なし)"), acc, bg);
        return;
    }
    int year = 2000 + t.year;
    int month = t.month + cal_offset;
    while (month < 1) {
        month += 12;
        year -= 1;
    }
    while (month > 12) {
        month -= 12;
        year += 1;
    }
    int is_current = (cal_offset == 0);
    char title[32];
    size_t p = 0;
    int y = year;
    char num[5];
    int ni = 0;
    if (y == 0) {
        num[ni++] = '0';
    } else {
        char rev[5];
        int ri = 0;
        while (y > 0 && ri < 4) {
            rev[ri++] = (char)('0' + (y % 10));
            y /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    const char *pre = L("Calendar - ", "カレンダー - ");
    while (*pre != '\0' && p + 1 < sizeof(title)) {
        title[p++] = *pre++;
    }
    for (int i = 0; num[i] != '\0' && p + 1 < sizeof(title); i++) {
        title[p++] = num[i];
    }
    title[p++] = '-';
    title[p++] = (char)('0' + (month / 10));
    title[p++] = (char)('0' + (month % 10));
    title[p] = '\0';
    draw_text_small(x0, y0, title, acc, bg);
    char date_line[32];
    rtc_format_date(&t, date_line);
    draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), date_line, fg, bg);
    static const char *wd[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (int i = 0; i < 7; i++) {
        uint64_t cx = x0 + (uint64_t)(i * 56);
        draw_text_small(cx, y0 + (uint64_t)(4 * CELL_H), wd[i],
                        col_dim(), bg);
    }
    int first = rtc_weekday(year, month, 1);
    int days = rtc_days_in_month(year, month);
    for (int d = 1; d <= days; d++) {
        int idx = first + d - 1;
        int r = idx / 7;
        int c = idx % 7;
        uint64_t cx = x0 + (uint64_t)(c * 56);
        uint64_t cy = y0 + (uint64_t)((5 + r) * CELL_H);
        char cell[4];
        cell[0] = (char)('0' + (d / 10));
        cell[1] = (char)('0' + (d % 10));
        cell[2] = '\0';
        if (is_current && d == t.day) {
            fb_fill_rect(cx - 2, cy, 40, CELL_H, acc);
            fb_draw_text(cx, cy, cell, rgb(0xFFFFFF), acc, 1);
        } else {
            fb_draw_text(cx, cy, cell, fg, bg, 1);
        }
    }
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("Left/Right = month, Esc = back to Terminal", "左右 = 月移動、Esc = ターミナルへ戻る"),
                    col_dim(), bg);
}

/* ---- Photos: real BMP files from the file system ---- */

static int name_ends_with(const char *name, const char *suffix) {
    size_t nl = strlen(name);
    size_t sl = strlen(suffix);
    if (nl < sl) {
        return 0;
    }
    const char *tail = name + nl - sl;
    for (size_t i = 0; i < sl; i++) {
        char a = tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* VFS indices of the .bmp files, in file order. */
static int photos_list(int *out, int max) {
    int count = 0;
    int n = vfs_file_count();
    for (int i = 0; i < n && count < max; i++) {
        const char *name = vfs_file_name(i);
        if (name != NULL && name_ends_with(name, ".bmp")) {
            out[count++] = i;
        }
    }
    return count;
}

/* 表示中の画像を壁紙に設定する。既存の BMP デコーダ
 * (wallpaper_parse) で読める 24bit BMP のみ。成功したら
 * settings.txt へ保存し、再起動後も同じ壁紙になる。 */
static void photos_set_wallpaper(void) {
    int list[PRISM_VFS_MAX_FILES];
    int count = photos_list(list, PRISM_VFS_MAX_FILES);
    if (photos_viewing < 0 || photos_viewing >= count) {
        return;
    }
    const char *name = vfs_file_name(list[photos_viewing]);
    if (name == NULL) {
        return;
    }
    size_t len = 0;
    const char *content = vfs_read_ptr(name, &len);
    wallpaper_img img;
    if (content == NULL ||
        wallpaper_parse((const uint8_t *)content, len, &img) != 0) {
        /* 読めない形式は正直にエラーにする。 */
        size_t p = 0;
        const char *e = L("Not a 24-bit BMP file.",
                          "24ビットBMPファイルではありません");
        while (*e != '\0' && p + 1 < sizeof(photos_status)) {
            photos_status[p++] = *e++;
        }
        photos_status[p] = '\0';
        serial_write_line("photos: wallpaper failed (not a BMP)");
        return;
    }
    size_t j = 0;
    while (name[j] != '\0' &&
           j + 1 < sizeof(desk_settings.wallpaper_file)) {
        desk_settings.wallpaper_file[j] = name[j];
        j++;
    }
    desk_settings.wallpaper_file[j] = '\0';
    desk_settings.wallpaper_image = 2;
    save_desk_settings();
    wp_custom_load();
    {
        size_t p = 0;
        const char *s = L("Set as wallpaper: ",
                          "壁紙に設定しました: ");
        while (*s != '\0' && p + 1 < sizeof(photos_status)) {
            photos_status[p++] = *s++;
        }
        const char *nm = desk_settings.wallpaper_file;
        while (*nm != '\0' && p + 1 < sizeof(photos_status)) {
            photos_status[p++] = *nm++;
        }
        photos_status[p] = '\0';
    }
    serial_write("photos: wallpaper set to ");
    serial_write_line(desk_settings.wallpaper_file);
    desktop_notify(L("Wallpaper set", "壁紙を設定しました"));
}

static void draw_photos(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    int list[PRISM_VFS_MAX_FILES];
    int count = photos_list(list, PRISM_VFS_MAX_FILES);
    if (photos_viewing >= 0 && photos_viewing < count) {
        const char *name = vfs_file_name(list[photos_viewing]);
        size_t len = 0;
        const char *content = vfs_read_ptr(name, &len);
        wallpaper_img img;
        char header[64];
        size_t p = 0;
        const char *h = L("Viewing: ", "表示中: ");
        while (*h != '\0' && p + 1 < sizeof(header)) {
            header[p++] = *h++;
        }
        while (*name != '\0' && p + 1 < sizeof(header)) {
            header[p++] = *name++;
        }
        header[p] = '\0';
        draw_text_small(x0, y0, header, acc, bg);
        if (content == NULL ||
            wallpaper_parse((const uint8_t *)content, len, &img) != 0) {
            draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                            L("Not a 24-bit BMP file.", "24ビットBMPファイルではありません"), fg, bg);
        } else {
            /* 回転が指定されていれば回転後の画素列を描く。
             * ズームは中央の矩形を拡大して描く。 */
            uint64_t iw = win_w() - 16;
            uint64_t ih = (uint64_t)(text_rows() - 3) * CELL_H;
            const uint8_t *pix = img.pixels;
            int pw = img.width;
            int ph = img.height;
            int pstride = img.stride;
            int ptd = img.top_down;
            int rw = 0;
            int rh = 0;
            if (photos_rot != 0 &&
                photos_rotate_build(&img, photos_rot, &rw, &rh) ==
                    0) {
                pix = photos_rot_buf;
                pw = rw;
                ph = rh;
                pstride = rw * 3;
                ptd = 1;
            }
            int zw = pw >> photos_zoom;
            int zh = ph >> photos_zoom;
            if (zw < 1) {
                zw = 1;
            }
            if (zh < 1) {
                zh = 1;
            }
            fb_draw_image_rgb24_rect(x0, y0 + (uint64_t)(2 * CELL_H),
                                     iw, ih, pix, pw, ph, pstride,
                                     ptd, (pw - zw) / 2, (ph - zh) / 2,
                                     zw, zh);
        }
        if (photos_status[0] != '\0') {
            draw_text_small(x0,
                            y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                            photos_status, acc, bg);
        }
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                        L("Left/Right = prev/next  +/- = zoom  "
                          "R = rotate  S = slideshow  "
                          "W = wallpaper  Esc = list",
                          "左右 = 前/次  +/- = 拡大  R = 回転  "
                          "S = スライドショー  W = 壁紙  Esc = 一覧"),
                        col_dim(), bg);
        return;
    }
    draw_text_small(x0, y0, L("Photos - Up/Down to move, Enter to open", "写真 - 上下で移動、Enterで開く"),
                    col_dim(), bg);
    for (int i = 0; i < count && i + 2 < text_rows(); i++) {
        const char *name = vfs_file_name(list[i]);
        size_t len = 0;
        (void)vfs_read_ptr(name, &len);
        char line[80];
        size_t p = 0;
        while (*name != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *name++;
        }
        line[p++] = ' ';
        line[p++] = ' ';
        line[p++] = '(';
        char num[21];
        int ni = 0;
        size_t v = len;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[21];
            int ri = 0;
            while (v > 0 && ri < 20) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int k = 0; num[k] != '\0' && p + 1 < sizeof(line); k++) {
            line[p++] = num[k];
        }
        const char *tail = L(" bytes)", " バイト)");
        while (*tail != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *tail++;
        }
        line[p] = '\0';
        uint64_t py = y0 + (uint64_t)((i + 2) * CELL_H);
        if (i == photos_selected) {
            fb_fill_rect(x0, py, win_w() - 16, CELL_H, acc);
            fb_draw_text(x0 + 4, py, line, rgb(0xFFFFFF), acc, 1);
        } else {
            fb_draw_text(x0 + 4, py, line, fg, bg, 1);
        }
    }
    if (count == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), L("(no photos)", "(写真なし)"),
                        col_dim(), bg);
    }
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("Esc = back to Terminal", "Esc = ターミナルへ戻る"), col_dim(), bg);
}

/* ---- Music: real WAV files through the SB16 audio device ---- */

/* VFS indices of the .wav files, in file order. */
static int music_list(int *out, int max) {
    int count = 0;
    int n = vfs_file_count();
    for (int i = 0; i < n && count < max; i++) {
        const char *name = vfs_file_name(i);
        if (name != NULL && name_ends_with(name, ".wav")) {
            out[count++] = i;
        }
    }
    return count;
}

static void music_play_selected(void) {
    int list[PRISM_VFS_MAX_FILES];
    int count = music_list(list, PRISM_VFS_MAX_FILES);
    if (count == 0 || music_selected < 0 || music_selected >= count) {
        return;
    }
    const char *name = vfs_file_name(list[music_selected]);
    size_t len = 0;
    const char *data = vfs_read_ptr(name, &len);
    if (data == NULL) {
        music_state = -1;
        return;
    }
    wav_info info;
    if (wav_parse((const uint8_t *)data, len, &info) != 0) {
        music_state = -1;
        serial_write("music: not an 8-bit mono WAV file: ");
        serial_write_line(name);
        return;
    }
    if (!music_has_device) {
        music_state = -2;
        serial_write_line("music: no audio device (SB16 not found)");
        return;
    }
    serial_write("music: playing ");
    serial_write_line(name);
    if (sb16_play_8bit(info.samples, info.sample_count,
                       info.sample_rate) != 0) {
        music_state = -1;
        serial_write_line("music: play failed");
    } else {
        music_state = 1;
        serial_write_line("music: play done");
    }
}

static void draw_music(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    int list[PRISM_VFS_MAX_FILES];
    int count = music_list(list, PRISM_VFS_MAX_FILES);
    draw_text_small(x0, y0, L("Music - Up/Down to move, Enter to play, +/- volume", "音楽 - 上下で移動、Enterで再生、+/- 音量"),
                    col_dim(), bg);
    if (music_checked) {
        draw_text_small(x0, y0 + (uint64_t)(1 * CELL_H),
                        music_has_device ? L("Audio: SB16 ready", "音声: SB16 準備完了")
                                         : L("Audio: no device (SB16 not found)", "音声: デバイスなし (SB16が見つかりません)"),
                        music_has_device ? acc : fg, bg);
    }
    for (int i = 0; i < count && i + 3 < text_rows(); i++) {
        const char *name = vfs_file_name(list[i]);
        size_t len = 0;
        (void)vfs_read_ptr(name, &len);
        char line[80];
        size_t p = 0;
        while (*name != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *name++;
        }
        line[p++] = ' ';
        line[p++] = ' ';
        line[p++] = '(';
        char num[21];
        int ni = 0;
        size_t v = len;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[21];
            int ri = 0;
            while (v > 0 && ri < 20) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int k = 0; num[k] != '\0' && p + 1 < sizeof(line); k++) {
            line[p++] = num[k];
        }
        const char *tail = L(" bytes)", " バイト)");
        while (*tail != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *tail++;
        }
        line[p] = '\0';
        uint64_t py = y0 + (uint64_t)((i + 3) * CELL_H);
        if (i == music_selected) {
            fb_fill_rect(x0, py, win_w() - 16, CELL_H, acc);
            fb_draw_text(x0 + 4, py, line, rgb(0xFFFFFF), acc, 1);
        } else {
            fb_draw_text(x0 + 4, py, line, fg, bg, 1);
        }
    }
    if (count == 0) {
        draw_text_small(x0, y0 + (uint64_t)(3 * CELL_H), L("(no music)", "(音楽なし)"),
                        col_dim(), bg);
    }
    if (music_state == 1) {
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                        L("Played.", "再生しました"), acc, bg);
    } else if (music_state == -1) {
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                        L("Could not play this file.", "このファイルを再生できませんでした"), acc, bg);
    } else if (music_state == -2) {
        draw_text_small(x0, y0 + (uint64_t)((text_rows() - 2) * CELL_H),
                        L("No audio device.", "音声デバイスがありません"), acc, bg);
    }
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("Esc = back to Terminal", "Esc = ターミナルへ戻る"), col_dim(), bg);
}

/* ---- Cards: a real game of blackjack ---- */

static void cards_format_hand(const int *hand, int n, char *buf,
                              size_t cap) {
    size_t p = 0;
    for (int i = 0; i < n; i++) {
        char card[8];
        int cn = cards_format(hand[i], card, sizeof(card));
        for (int k = 0; k < cn && p + 1 < cap; k++) {
            buf[p++] = card[k];
        }
        if (i + 1 < n && p + 1 < cap) {
            buf[p++] = ' ';
        }
    }
    buf[p] = '\0';
}

static void cards_decide(void) {
    /* Dealer draws until 17 or more. */
    while (cards_dealer_hits(cards_hand_value(cards_dealer,
                                              cards_dealer_n)) &&
           cards_dealer_n < 12 && cards_pos < PRISM_DECK_SIZE) {
        cards_dealer[cards_dealer_n++] = cards_deck[cards_pos++];
    }
    int pv = cards_hand_value(cards_player, cards_player_n);
    int dv = cards_hand_value(cards_dealer, cards_dealer_n);
    if (dv > 21) {
        cards_wins++;
    } else if (pv > dv) {
        cards_wins++;
    } else if (pv < dv) {
        cards_losses++;
    } else {
        cards_pushes++;
    }
    cards_phase = 2;
}

static void cards_new_game(void) {
    /* Shuffle with a seed from the clock and the input loop
     * counter: different every game, but only game randomness. */
    prism_rtc_time t;
    uint32_t seed = 1;
    if (rtc_read(&t) == 0) {
        seed = (uint32_t)(t.hour * 3600 + t.minute * 60 + t.second + 1);
    }
    seed ^= (uint32_t)(tick_counter * 2654435761u);
    seed ^= (uint32_t)(cards_wins * 97 + cards_losses * 57 +
                       cards_pushes * 31 + 7);
    cards_new_deck(cards_deck);
    cards_shuffle(cards_deck, seed);
    cards_pos = 0;
    cards_player_n = 0;
    cards_dealer_n = 0;
    cards_player[cards_player_n++] = cards_deck[cards_pos++];
    cards_dealer[cards_dealer_n++] = cards_deck[cards_pos++];
    cards_player[cards_player_n++] = cards_deck[cards_pos++];
    cards_dealer[cards_dealer_n++] = cards_deck[cards_pos++];
    cards_phase = 1;
    if (cards_is_blackjack(cards_player, cards_player_n) ||
        cards_is_blackjack(cards_dealer, cards_dealer_n)) {
        /* A blackjack on the deal ends the game at once. */
        if (cards_is_blackjack(cards_player, cards_player_n) &&
            cards_is_blackjack(cards_dealer, cards_dealer_n)) {
            cards_pushes++;
        } else if (cards_is_blackjack(cards_player,
                                      cards_player_n)) {
            cards_wins++;
        } else {
            cards_losses++;
        }
        cards_phase = 2;
    }
}

static void draw_blackjack(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Cards - Blackjack", "カード - ブラックジャック"), acc, bg);
    char line[96];
    if (cards_phase == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("Press n for a new game.", "nで新しいゲーム"), fg, bg);
        draw_text_small(x0, y0 + (uint64_t)(3 * CELL_H),
                        "Get closer to 21 than the dealer, without going over.",
                        col_dim(), bg);
    } else {
        /* Dealer: hide the first card while the player plays. */
        size_t p = 0;
        const char *pre = L("Dealer: ", "ディーラー: ");
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        if (cards_phase == 1) {
            const char *hid = "?? ";
            while (*hid != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *hid++;
            }
            char rest[64];
            cards_format_hand(cards_dealer + 1, cards_dealer_n - 1, rest,
                              sizeof(rest));
            for (int i = 0; rest[i] != '\0' && p + 1 < sizeof(line); i++) {
                line[p++] = rest[i];
            }
        } else {
            char all[64];
            cards_format_hand(cards_dealer, cards_dealer_n, all,
                              sizeof(all));
            for (int i = 0; all[i] != '\0' && p + 1 < sizeof(line); i++) {
                line[p++] = all[i];
            }
            const char *mid = "  (";
            while (*mid != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *mid++;
            }
            int dv = cards_hand_value(cards_dealer, cards_dealer_n);
            if (dv >= 10) {
                line[p++] = (char)('0' + (dv / 10));
            }
            line[p++] = (char)('0' + (dv % 10));
            line[p++] = ')';
        }
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(2 * CELL_H), line, fg, bg, 1);
        /* Player. */
        p = 0;
        const char *pp = L("You:    ", "あなた:   ");
        while (*pp != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pp++;
        }
        char hand[64];
        cards_format_hand(cards_player, cards_player_n, hand,
                          sizeof(hand));
        for (int i = 0; hand[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = hand[i];
        }
        const char *mid2 = "  (";
        while (*mid2 != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *mid2++;
        }
        int pv = cards_hand_value(cards_player, cards_player_n);
        if (pv >= 10) {
            line[p++] = (char)('0' + (pv / 10));
        }
        line[p++] = (char)('0' + (pv % 10));
        line[p++] = ')';
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(3 * CELL_H), line, fg, bg, 1);
        if (cards_phase == 2) {
            const char *result;
            int pv2 = cards_hand_value(cards_player, cards_player_n);
            int dv2 = cards_hand_value(cards_dealer, cards_dealer_n);
            if (pv2 > 21) {
                result = L("Bust! Dealer wins.", "バスト! ディーラーの勝ち");
            } else if (cards_is_blackjack(cards_player,
                                          cards_player_n) &&
                       !cards_is_blackjack(cards_dealer,
                                           cards_dealer_n)) {
                result = L("Blackjack! You win!", "ブラックジャック! あなたの勝ち!");
            } else if (dv2 > 21) {
                result = L("Dealer busts! You win!", "ディーラーがバスト! あなたの勝ち!");
            } else if (pv2 > dv2) {
                result = L("You win!", "あなたの勝ち!");
            } else if (pv2 < dv2) {
                result = L("Dealer wins.", "ディーラーの勝ち");
            } else {
                result = L("Push (draw).", "プッシュ (引き分け)");
            }
            draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H), result, acc,
                            bg);
        }
    }
    size_t p = 0;
    const char *score = L("Wins: ", "勝ち: ");
    while (*score != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *score++;
    }
    char num[12];
    int ni = 0;
    int v = cards_wins;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[12];
        int ri = 0;
        while (v > 0 && ri < 11) {
            rev[ri++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line); i++) {
        line[p++] = num[i];
    }
    const char *mid3 = L("  Losses: ", "  負け: ");
    while (*mid3 != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *mid3++;
    }
    ni = 0;
    v = cards_losses;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[12];
        int ri = 0;
        while (v > 0 && ri < 11) {
            rev[ri++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line); i++) {
        line[p++] = num[i];
    }
    const char *mid4 = L("  Pushes: ", "  引き分け: ");
    while (*mid4 != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *mid4++;
    }
    ni = 0;
    v = cards_pushes;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[12];
        int ri = 0;
        while (v > 0 && ri < 11) {
            rev[ri++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line); i++) {
        line[p++] = num[i];
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(7 * CELL_H), line, fg, bg, 1);
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("n = new game, h = hit, s = stand, Esc = game menu", "n = 新規、h = ヒット、s = スタンド、Esc = ゲームメニュー"),
                    col_dim(), bg);
}

/* ---- Cards: game selection + the other games ----
 * cards_game: 0 = menu, 1 = blackjack (above), 2 = draw
 * poker, 3 = baccarat, 4 = war, 5 = old maid, 6 = sevens,
 * 7 = memory. Every game plays by its real rules against
 * a CPU opponent; results are logged to the serial port
 * as "cards: <game> ..." lines. */
static int cards_game;
static const char *const CARDS_GAME_NAMES[8] = {
    "", "Blackjack", "Draw Poker", "Baccarat", "War",
    "Old Maid", "Sevens", "Memory"
};

/* ゲーム選択メニューの表示名 (言語切替つき)。上の表は
 * ログ用に英語のまま。 */
static const char *cards_game_label(int g) {
    switch (g) {
        case 1:
            return L("Blackjack", "ブラックジャック");
        case 2:
            return L("Draw Poker", "ドローポーカー");
        case 3:
            return L("Baccarat", "バカラ");
        case 4:
            return L("War", "ウォー");
        case 5:
            return L("Old Maid", "ババ抜き");
        case 6:
            return L("Sevens", "7並べ");
        case 7:
            return L("Memory", "神経衰弱");
        default:
            return "";
    }
}

static uint32_t cards_seed(void) {
    prism_rtc_time t;
    uint32_t seed = 1;
    if (rtc_read(&t) == 0) {
        seed = (uint32_t)(t.hour * 3600 + t.minute * 60 + t.second + 1);
    }
    seed ^= (uint32_t)(tick_counter * 2654435761u);
    seed ^= (uint32_t)(cards_wins * 97 + cards_losses * 57 +
                       cards_pushes * 31 + 7);
    return seed != 0 ? seed : 1;
}

static void cards_shuffle_new(void) {
    cards_new_deck(cards_deck);
    cards_shuffle(cards_deck, cards_seed());
    cards_pos = 0;
}

static void cards_log_result(const char *game, const char *res) {
    serial_write("cards: ");
    serial_write(game);
    serial_write(" ");
    serial_write_line(res);
}

/* ----- draw poker ----- */
static int pk_player[5];
static int pk_cpu[5];
static int pk_mark[5];
static int pk_phase; /* 0 = choose discards, 1 = result */
static int pk_result; /* 1 win, -1 lose, 0 draw */

static void pk_new_game(void) {
    cards_shuffle_new();
    for (int i = 0; i < 5; i++) {
        pk_player[i] = cards_deck[cards_pos++];
        pk_cpu[i] = cards_deck[cards_pos++];
        pk_mark[i] = 0;
    }
    pk_phase = 0;
    cards_log_result("poker", "new game");
}

static void pk_draw_cards(void) {
    for (int i = 0; i < 5; i++) {
        if (pk_mark[i]) {
            pk_player[i] = cards_deck[cards_pos++];
        }
    }
    int disc[5];
    poker_cpu_discards(pk_cpu, disc);
    for (int i = 0; i < 5; i++) {
        if (disc[i]) {
            pk_cpu[i] = cards_deck[cards_pos++];
        }
    }
    int ps = poker_eval(pk_player);
    int cs = poker_eval(pk_cpu);
    pk_result = ps > cs ? 1 : (ps < cs ? -1 : 0);
    pk_phase = 1;
    serial_write("cards: poker you=");
    serial_write(poker_category(ps));
    serial_write(" cpu=");
    serial_write(poker_category(cs));
    serial_write_line(pk_result > 0   ? ": win"
                      : pk_result < 0 ? ": lose"
                                      : ": draw");
}

static void draw_poker(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Cards - Draw Poker", "カード - ドローポーカー"), acc, bg);
    char line[96];
    char hand[64];
    size_t p = 0;
    const char *pre = L("CPU: ", "CPU: ");
    while (*pre != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pre++;
    }
    if (pk_phase == 0) {
        const char *hid = "?? ?? ?? ?? ??";
        while (*hid != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *hid++;
        }
    } else {
        cards_format_hand(pk_cpu, 5, hand, sizeof(hand));
        for (int i = 0; hand[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = hand[i];
        }
        const char *mid = "  (";
        while (*mid != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *mid++;
        }
        const char *cat = poker_category(poker_eval(pk_cpu));
        while (*cat != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *cat++;
        }
        line[p++] = ')';
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(2 * CELL_H), line, fg, bg, 1);
    p = 0;
    const char *pp = L("You: ", "あなた: ");
    while (*pp != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pp++;
    }
    cards_format_hand(pk_player, 5, hand, sizeof(hand));
    for (int i = 0; hand[i] != '\0' && p + 1 < sizeof(line); i++) {
        line[p++] = hand[i];
    }
    if (pk_phase == 1) {
        const char *mid = "  (";
        while (*mid != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *mid++;
        }
        const char *cat = poker_category(poker_eval(pk_player));
        while (*cat != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *cat++;
        }
        line[p++] = ')';
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(3 * CELL_H), line, fg, bg, 1);
    if (pk_phase == 0) {
        /* discard marks under the player's cards */
        char marks[64];
        size_t q = 0;
        const char *mp = "     ";
        while (*mp != '\0' && q + 1 < sizeof(marks)) {
            marks[q++] = *mp++;
        }
        for (int i = 0; i < 5; i++) {
            marks[q++] = pk_mark[i] ? 'X' : (char)('1' + i);
            marks[q++] = ' ';
            marks[q++] = ' ';
            if (cards_rank(pk_player[i]) == 10) {
                marks[q++] = ' ';
            }
        }
        marks[q] = '\0';
        draw_text_small(x0, y0 + (uint64_t)(4 * CELL_H), marks, acc,
                        bg);
        draw_text_small(x0, y0 + (uint64_t)(6 * CELL_H),
                        L("1-5 = mark discards, d = draw, n = new game", "1-5 = 捨て札を選ぶ、d = 引く、n = 新規"),
                        col_dim(), bg);
    } else {
        draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H),
                        pk_result > 0   ? L("You win!", "あなたの勝ち!")
                        : pk_result < 0 ? L("CPU wins.", "CPUの勝ち")
                                        : L("Draw.", "引き分け"),
                        acc, bg);
        draw_text_small(x0, y0 + (uint64_t)(6 * CELL_H),
                        L("n = new game", "n = 新規ゲーム"), col_dim(), bg);
    }
}

/* ----- baccarat ----- */
static int bc_player[3];
static int bc_banker[3];
static int bc_pn, bc_bn;
static int bc_phase; /* 0 idle, 1 done */
static int bc_result; /* 1 player, -1 banker, 0 tie */

static void bc_new_game(void) {
    cards_shuffle_new();
    bc_pn = 0;
    bc_bn = 0;
    bc_player[bc_pn++] = cards_deck[cards_pos++];
    bc_banker[bc_bn++] = cards_deck[cards_pos++];
    bc_player[bc_pn++] = cards_deck[cards_pos++];
    bc_banker[bc_bn++] = cards_deck[cards_pos++];
    int pv = baccarat_value(bc_player, bc_pn);
    int bv = baccarat_value(bc_banker, bc_bn);
    bc_result = 0;
    if (pv < 8 && bv < 8) {
        int p3 = -1;
        if (baccarat_player_draws(pv)) {
            bc_player[bc_pn] = cards_deck[cards_pos++];
            p3 = baccarat_card_value(bc_player[bc_pn]);
            bc_pn++;
            pv = baccarat_value(bc_player, bc_pn);
        }
        if (baccarat_banker_draws(bv, p3)) {
            bc_banker[bc_bn++] = cards_deck[cards_pos++];
            bv = baccarat_value(bc_banker, bc_bn);
        }
    }
    bc_result = pv > bv ? 1 : (pv < bv ? -1 : 0);
    bc_phase = 1;
    cards_log_result("baccarat",
                     bc_result > 0   ? "player wins"
                     : bc_result < 0 ? "banker wins"
                                     : "tie");
}

static void draw_baccarat(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Cards - Baccarat", "カード - バカラ"), acc, bg);
    if (bc_phase == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("Press n to deal.", "nで配る"), fg, bg);
        return;
    }
    char line[96];
    char hand[64];
    size_t p = 0;
    const char *pre = L("Banker: ", "バンカー: ");
    while (*pre != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pre++;
    }
    cards_format_hand(bc_banker, bc_bn, hand, sizeof(hand));
    for (int i = 0; hand[i] != '\0' && p + 1 < sizeof(line); i++) {
        line[p++] = hand[i];
    }
    const char *mid = "  (";
    while (*mid != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *mid++;
    }
    line[p++] = (char)('0' + baccarat_value(bc_banker, bc_bn));
    line[p++] = ')';
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(2 * CELL_H), line, fg, bg, 1);
    p = 0;
    const char *pp = L("Player: ", "プレイヤー: ");
    while (*pp != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pp++;
    }
    cards_format_hand(bc_player, bc_pn, hand, sizeof(hand));
    for (int i = 0; hand[i] != '\0' && p + 1 < sizeof(line); i++) {
        line[p++] = hand[i];
    }
    const char *mid2 = "  (";
    while (*mid2 != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *mid2++;
    }
    line[p++] = (char)('0' + baccarat_value(bc_player, bc_pn));
    line[p++] = ')';
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(3 * CELL_H), line, fg, bg, 1);
    draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H),
                    bc_result > 0   ? L("Player wins!", "プレイヤーの勝ち!")
                    : bc_result < 0 ? L("Banker wins.", "バンカーの勝ち")
                                    : L("Tie.", "タイ (引き分け)"),
                    acc, bg);
    draw_text_small(x0, y0 + (uint64_t)(6 * CELL_H), L("n = deal again", "n = もう一度配る"),
                    col_dim(), bg);
}

/* ----- war ----- */
static int war_p[52];
static int war_c[52];
static int war_pn, war_cn;
static int war_last_p, war_last_c; /* last battle cards, -1 none */
static int war_phase; /* 0 idle, 1 playing, 2 over */
static int war_battles;

static int war_take(int *hand, int *n) {
    int c = hand[0];
    for (int i = 1; i < *n; i++) {
        hand[i - 1] = hand[i];
    }
    (*n)--;
    return c;
}

static void war_give(int *hand, int *n, int c) {
    hand[(*n)++] = c;
}

static void war_new_game(void) {
    cards_shuffle_new();
    war_pn = 0;
    war_cn = 0;
    for (int i = 0; i < 52; i++) {
        if (i % 2 == 0) {
            war_p[war_pn++] = cards_deck[i];
        } else {
            war_c[war_cn++] = cards_deck[i];
        }
    }
    war_last_p = -1;
    war_last_c = -1;
    war_battles = 0;
    war_phase = 1;
    cards_log_result("war", "new game");
}

static int war_rank_high(int card) {
    int r = cards_rank(card);
    return r == 1 ? 14 : r;
}

static void war_battle(void) {
    if (war_phase != 1 || war_pn == 0 || war_cn == 0) {
        return;
    }
    /* The pot accumulates through wars. */
    int pot[52];
    int potn = 0;
    int p = war_take(war_p, &war_pn);
    int c = war_take(war_c, &war_cn);
    pot[potn++] = p;
    pot[potn++] = c;
    war_last_p = p;
    war_last_c = c;
    for (;;) {
        int pr = war_rank_high(p);
        int cr = war_rank_high(c);
        if (pr != cr) {
            int *win_hand = pr > cr ? war_p : war_c;
            int *win_n = pr > cr ? &war_pn : &war_cn;
            for (int i = 0; i < potn; i++) {
                war_give(win_hand, win_n, pot[i]);
            }
            break;
        }
        /* war: three face-down each, then flip again */
        if (war_pn < 2 || war_cn < 2) {
            /* cannot fight a war: the side with cards
             * left takes the pot */
            int *win_hand = war_pn > war_cn ? war_p : war_c;
            int *win_n = war_pn > war_cn ? &war_pn : &war_cn;
            for (int i = 0; i < potn; i++) {
                war_give(win_hand, win_n, pot[i]);
            }
            break;
        }
        for (int i = 0; i < 3 && war_pn > 1; i++) {
            pot[potn++] = war_take(war_p, &war_pn);
        }
        for (int i = 0; i < 3 && war_cn > 1; i++) {
            pot[potn++] = war_take(war_c, &war_cn);
        }
        p = war_take(war_p, &war_pn);
        c = war_take(war_c, &war_cn);
        pot[potn++] = p;
        pot[potn++] = c;
        war_last_p = p;
        war_last_c = c;
    }
    war_battles++;
    if (war_pn == 0 || war_cn == 0 || war_battles >= 300) {
        war_phase = 2;
        cards_log_result("war", war_pn > war_cn   ? "you win"
                                : war_pn < war_cn ? "cpu wins"
                                                  : "draw");
    }
}

static void draw_war(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Cards - War", "カード - ウォー"), acc, bg);
    if (war_phase == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("Press n for a new game.", "nで新しいゲーム"), fg, bg);
        return;
    }
    char line[96];
    size_t p = 0;
    const char *pre = L("CPU cards: ", "CPUのカード: ");
    while (*pre != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pre++;
    }
    {
        char num[8];
        int ni = 0;
        int v = war_cn;
        char rev[8];
        int ri = 0;
        if (v == 0) {
            num[ni++] = '0';
        }
        while (v > 0) {
            rev[ri++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
        num[ni] = '\0';
        for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = num[i];
        }
    }
    if (war_last_c >= 0) {
        char card[4];
        cards_format(war_last_c, card, sizeof(card));
        const char *mid = L("   last: ", "   直前: ");
        while (*mid != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *mid++;
        }
        for (int i = 0; card[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = card[i];
        }
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(2 * CELL_H), line, fg, bg, 1);
    p = 0;
    const char *pp = L("You cards: ", "あなたのカード: ");
    while (*pp != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pp++;
    }
    {
        char num[8];
        int ni = 0;
        int v = war_pn;
        char rev[8];
        int ri = 0;
        if (v == 0) {
            num[ni++] = '0';
        }
        while (v > 0) {
            rev[ri++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
        num[ni] = '\0';
        for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = num[i];
        }
    }
    if (war_last_p >= 0) {
        char card[4];
        cards_format(war_last_p, card, sizeof(card));
        const char *mid = L("   last: ", "   直前: ");
        while (*mid != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *mid++;
        }
        for (int i = 0; card[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = card[i];
        }
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(3 * CELL_H), line, fg, bg, 1);
    if (war_phase == 2) {
        draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H),
                        war_pn > war_cn   ? L("You win the war!", "戦争に勝利!")
                        : war_pn < war_cn ? L("CPU wins the war.", "CPUが戦争に勝利")
                                          : L("A draw.", "引き分け"),
                        acc, bg);
    }
    draw_text_small(x0, y0 + (uint64_t)(6 * CELL_H),
                    L("f = flip a battle, n = new game", "f = 対戦、n = 新規ゲーム"), col_dim(), bg);
}

/* ----- old maid ----- */
static int om_p[53];
static int om_c[53];
static int om_pn, om_cn;
static int om_phase; /* 0 idle, 1 your turn, 2 over */
static int om_result; /* 1 you win (cpu left with joker) */
static int om_sel; /* index into cpu hand to draw from */

static void om_new_game(void) {
    int deck[53];
    for (int i = 0; i < 52; i++) {
        deck[i] = i;
    }
    deck[52] = 52; /* joker */
    /* shuffle 53 with the game rng (xorshift in cards.c
     * works on 52; do a Fisher-Yates here with the seed) */
    uint32_t s = cards_seed();
    for (int i = 52; i > 0; i--) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int j = (int)(s % (uint32_t)(i + 1));
        int t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    om_pn = 0;
    om_cn = 0;
    for (int i = 0; i < 53; i++) {
        if (i % 2 == 0) {
            om_p[om_pn++] = deck[i];
        } else {
            om_c[om_cn++] = deck[i];
        }
    }
    om_pn = oldmaid_discard_pairs(om_p, om_pn);
    om_cn = oldmaid_discard_pairs(om_c, om_cn);
    om_sel = 0;
    om_phase = 1;
    cards_log_result("oldmaid", "new game");
}

static void om_check_end(void) {
    if (om_pn == 0 || om_cn == 0) {
        om_phase = 2;
        /* Whoever still holds cards holds the joker:
         * empty hand = safe. If you are empty you win
         * unless the cpu is also empty (impossible). */
        om_result = om_pn == 0 ? 1 : 0;
        cards_log_result("oldmaid",
                         om_result ? "you win" : "cpu wins");
    }
}

static void om_cpu_draw(void) {
    if (om_phase != 1 || om_pn == 0) {
        return;
    }
    uint32_t s = cards_seed();
    int idx = (int)(s % (uint32_t)om_pn);
    int card = om_p[idx];
    for (int i = idx + 1; i < om_pn; i++) {
        om_p[i - 1] = om_p[i];
    }
    om_pn--;
    om_c[om_cn++] = card;
    om_cn = oldmaid_discard_pairs(om_c, om_cn);
    om_pn = oldmaid_discard_pairs(om_p, om_pn);
    if (om_sel >= om_cn) {
        om_sel = 0;
    }
    om_check_end();
}

static void om_player_draw(void) {
    if (om_phase != 1 || om_cn == 0) {
        return;
    }
    if (om_sel >= om_cn) {
        om_sel = 0;
    }
    int card = om_c[om_sel];
    for (int i = om_sel + 1; i < om_cn; i++) {
        om_c[i - 1] = om_c[i];
    }
    om_cn--;
    om_p[om_pn++] = card;
    om_pn = oldmaid_discard_pairs(om_p, om_pn);
    om_check_end();
    if (om_phase == 1) {
        om_cpu_draw();
    }
}

static void draw_oldmaid(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Cards - Old Maid", "カード - ババ抜き"), acc, bg);
    if (om_phase == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("Press n for a new game.", "nで新しいゲーム"), fg, bg);
        return;
    }
    char line[128];
    size_t p = 0;
    const char *pre = L("CPU: ", "CPU: ");
    while (*pre != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pre++;
    }
    for (int i = 0; i < om_cn && p + 2 < sizeof(line); i++) {
        line[p++] = i == om_sel ? '[' : ' ';
        line[p++] = '?';
        line[p++] = i == om_sel ? ']' : ' ';
    }
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(2 * CELL_H), line, fg, bg, 1);
    p = 0;
    const char *pp = L("You: ", "あなた: ");
    while (*pp != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pp++;
    }
    char hand[96];
    cards_format_hand(om_p, om_pn, hand, sizeof(hand));
    for (int i = 0; hand[i] != '\0' && p + 1 < sizeof(line); i++) {
        line[p++] = hand[i];
    }
    /* the joker shows as JOKER in cards_format? it does
     * not know 52; mark it here */
    line[p] = '\0';
    fb_draw_text(x0, y0 + (uint64_t)(3 * CELL_H), line, fg, bg, 1);
    if (om_phase == 2) {
        draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H),
                        om_result ? L("CPU is left with the joker. You win!", "CPUにジョーカーが残りました。あなたの勝ち!")
                                  : L("You are left with the joker. CPU wins.", "あなたにジョーカーが残りました。CPUの勝ち"),
                        acc, bg);
    } else {
        draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H),
                        L("Left/Right = choose a CPU card, d = draw it", "左右 = CPUのカードを選ぶ、d = 引く"),
                        col_dim(), bg);
    }
    draw_text_small(x0, y0 + (uint64_t)(6 * CELL_H), L("n = new game", "n = 新規ゲーム"),
                    col_dim(), bg);
}

/* ----- sevens ----- */
static int sv_hand[4][13];
static int sv_n[4];
static int sv_lo[4];
static int sv_hi[4];
static int sv_pass[4];
static int sv_out[4];
static int sv_turn; /* 0 = you, 1-3 = cpu */
static int sv_phase; /* 0 idle, 1 playing, 2 over */
static int sv_sel;
static int sv_winner; /* player index, -1 none yet */

static void sv_new_game(void) {
    cards_shuffle_new();
    for (int pl = 0; pl < 4; pl++) {
        sv_n[pl] = 0;
        sv_pass[pl] = 0;
        sv_out[pl] = 0;
    }
    for (int i = 0; i < 52; i++) {
        int pl = i % 4;
        sv_hand[pl][sv_n[pl]++] = cards_deck[i];
    }
    for (int s = 0; s < 4; s++) {
        sv_lo[s] = 0;
        sv_hi[s] = 0;
    }
    /* The holder of the 7 of diamonds (suit 2, rank 7:
     * card 2*13+6 = 32) opens with it. */
    int starter = 0;
    for (int pl = 0; pl < 4; pl++) {
        for (int i = 0; i < sv_n[pl]; i++) {
            if (sv_hand[pl][i] == 32) {
                starter = pl;
                /* remove and place it */
                for (int k = i + 1; k < sv_n[pl]; k++) {
                    sv_hand[pl][k - 1] = sv_hand[pl][k];
                }
                sv_n[pl]--;
                sevens_place(sv_lo, sv_hi, 32);
            }
        }
    }
    sv_turn = (starter + 1) % 4;
    sv_sel = 0;
    sv_winner = -1;
    sv_phase = 1;
    cards_log_result("sevens", "new game");
}

static void sv_remove(int pl, int idx) {
    for (int k = idx + 1; k < sv_n[pl]; k++) {
        sv_hand[pl][k - 1] = sv_hand[pl][k];
    }
    sv_n[pl]--;
}

static void sv_finish_check(void) {
    for (int pl = 0; pl < 4; pl++) {
        if (sv_n[pl] == 0 && !sv_out[pl]) {
            sv_winner = pl;
            sv_phase = 2;
            cards_log_result("sevens",
                             pl == 0 ? "you win" : "cpu wins");
            return;
        }
    }
    /* everyone else out: the last one holding cards
     * loses, the first finisher already handled above */
    int active = 0;
    int last = -1;
    for (int pl = 0; pl < 4; pl++) {
        if (!sv_out[pl] && sv_n[pl] > 0) {
            active++;
            last = pl;
        }
    }
    if (active <= 1 && last >= 0) {
        sv_phase = 2;
        sv_winner = -2; /* `last` lost */
        cards_log_result("sevens",
                         last == 0 ? "cpu wins" : "you win");
    }
}

static void sv_cpu_turn(int pl) {
    /* play the first playable card; else pass (3 passes
     * = out, remaining cards are laid on the table) */
    for (int i = 0; i < sv_n[pl]; i++) {
        if (sevens_playable(sv_lo, sv_hi, sv_hand[pl][i])) {
            sevens_place(sv_lo, sv_hi, sv_hand[pl][i]);
            sv_remove(pl, i);
            return;
        }
    }
    sv_pass[pl]++;
    if (sv_pass[pl] >= 3) {
        sv_out[pl] = 1;
        for (int i = 0; i < sv_n[pl]; i++) {
            sevens_place(sv_lo, sv_hi, sv_hand[pl][i]);
        }
        sv_n[pl] = 0;
    }
}

static void sv_advance(void) {
    /* run cpu turns until it is the player's turn or
     * the game ends */
    int guard = 0;
    while (sv_phase == 1 && guard++ < 64) {
        sv_finish_check();
        if (sv_phase != 1) {
            return;
        }
        if (sv_turn == 0 && !sv_out[0]) {
            return;
        }
        int pl = sv_turn;
        sv_turn = (sv_turn + 1) % 4;
        if (sv_out[pl] || sv_n[pl] == 0) {
            continue;
        }
        if (pl != 0) {
            sv_cpu_turn(pl);
        }
    }
}

static void draw_sevens(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Cards - Sevens", "カード - 7並べ"), acc, bg);
    if (sv_phase == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("Press n for a new game.", "nで新しいゲーム"), fg, bg);
        return;
    }
    /* table state: four suit rows */
    static const char suit_ch[4] = { 'S', 'H', 'D', 'C' };
    for (int s = 0; s < 4; s++) {
        char line[64];
        size_t p = 0;
        line[p++] = suit_ch[s];
        line[p++] = ':';
        line[p++] = ' ';
        if (sv_lo[s] == 0) {
            const char *none = "(none)";
            while (*none != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *none++;
            }
        } else {
            for (int r = sv_lo[s]; r <= sv_hi[s]; r++) {
                if (r >= 10) {
                    line[p++] = '1';
                    line[p++] = (char)('0' + r % 10);
                } else if (r == 1) {
                    line[p++] = 'A';
                } else if (r == 11) {
                    line[p++] = 'J';
                } else if (r == 12) {
                    line[p++] = 'Q';
                } else if (r == 13) {
                    line[p++] = 'K';
                } else {
                    line[p++] = (char)('0' + r);
                }
                line[p++] = ' ';
            }
        }
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)((2 + s) * CELL_H), line,
                     fg, bg, 1);
    }
    /* cpu counts */
    {
        char line[96];
        size_t p = 0;
        const char *pre = L("CPU cards: ", "CPUのカード: ");
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        for (int pl = 1; pl < 4; pl++) {
            int v = sv_n[pl];
            if (v >= 10) {
                line[p++] = (char)('0' + v / 10);
            }
            line[p++] = (char)('0' + v % 10);
            line[p++] = ' ';
        }
        const char *mid = L(" passes(you): ", " パス(あなた): ");
        while (*mid != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *mid++;
        }
        line[p++] = (char)('0' + sv_pass[0]);
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(6 * CELL_H), line, fg, bg,
                     1);
    }
    /* your hand with selection */
    {
        char line[128];
        size_t p = 0;
        const char *pre = L("You: ", "あなた: ");
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        for (int i = 0; i < sv_n[0]; i++) {
            char card[4];
            cards_format(sv_hand[0][i], card, sizeof(card));
            if (i == sv_sel) {
                line[p++] = '[';
            }
            for (int k = 0; card[k] != '\0' && p + 1 < sizeof(line);
                 k++) {
                line[p++] = card[k];
            }
            if (i == sv_sel) {
                line[p++] = ']';
            }
            line[p++] = ' ';
        }
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(7 * CELL_H), line, acc, bg,
                     1);
    }
    if (sv_phase == 2) {
        const char *res = sv_winner == 0    ? L("You finished first. You win!", "最初に上がり! あなたの勝ち!")
                          : sv_winner == -2 ? (sv_out[0] || sv_n[0] > 0 ? L("Game over.", "ゲームオーバー") : L("You win!", "あなたの勝ち!"))
                                            : L("A CPU finished first.", "CPUが最初に上がりました");
        draw_text_small(x0, y0 + (uint64_t)(9 * CELL_H), res, acc,
                        bg);
    } else {
        draw_text_small(x0, y0 + (uint64_t)(9 * CELL_H),
                        L("Left/Right = select, Enter = play, p = pass (3 passes = out)", "左右 = 選択、Enter = 出す、p = パス (3回で失格)"),
                        col_dim(), bg);
    }
    draw_text_small(x0, y0 + (uint64_t)(10 * CELL_H), L("n = new game", "n = 新規ゲーム"),
                    col_dim(), bg);
}

/* ----- memory ----- */
#define MM_N 20
static int mm_card[MM_N];    /* rank 1-10, twice */
static int mm_state[MM_N];   /* 0 down, 1 up, 2 taken */
static int mm_score[2];
static int mm_turn;          /* 0 you, 1 cpu */
static int mm_first;         /* first flipped index or -1 */
static int mm_cursor;
static int mm_phase;         /* 0 idle, 1 playing, 2 over */
static int mm_known[MM_N];   /* cpu memory: rank or 0 */

static void mm_new_game(void) {
    int vals[MM_N];
    for (int i = 0; i < 10; i++) {
        vals[i * 2] = i + 1;
        vals[i * 2 + 1] = i + 1;
    }
    uint32_t s = cards_seed();
    for (int i = MM_N - 1; i > 0; i--) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int j = (int)(s % (uint32_t)(i + 1));
        int t = vals[i];
        vals[i] = vals[j];
        vals[j] = t;
    }
    for (int i = 0; i < MM_N; i++) {
        mm_card[i] = vals[i];
        mm_state[i] = 0;
        mm_known[i] = 0;
    }
    mm_score[0] = 0;
    mm_score[1] = 0;
    mm_turn = 0;
    mm_first = -1;
    mm_cursor = 0;
    mm_phase = 1;
    cards_log_result("memory", "new game");
}

static void mm_reveal(int idx) {
    mm_state[idx] = 1;
    mm_known[idx] = mm_card[idx];
}

static int mm_over_check(void) {
    int taken = 0;
    for (int i = 0; i < MM_N; i++) {
        if (mm_state[i] == 2) {
            taken++;
        }
    }
    if (taken == MM_N) {
        mm_phase = 2;
        cards_log_result("memory",
                         mm_score[0] > mm_score[1]   ? "you win"
                         : mm_score[0] < mm_score[1] ? "cpu wins"
                                                     : "draw");
        return 1;
    }
    return 0;
}

static void mm_cpu_turn(void) {
    /* The CPU remembers every card it has seen (its own
     * flips and yours). It takes a known pair when it
     * has one; otherwise it flips an unknown card, then
     * its known mate or another unknown. A match keeps
     * the turn. */
    while (mm_phase == 1) {
        int a = -1, b = -1;
        for (int i = 0; i < MM_N && a < 0; i++) {
            if (mm_state[i] != 0 || mm_known[i] == 0) {
                continue;
            }
            for (int j = i + 1; j < MM_N; j++) {
                if (mm_state[j] == 0 &&
                    mm_known[j] == mm_known[i]) {
                    a = i;
                    b = j;
                    break;
                }
            }
        }
        if (a < 0) {
            /* flip an unknown card */
            for (int i = 0; i < MM_N; i++) {
                if (mm_state[i] == 0 && mm_known[i] == 0) {
                    a = i;
                    break;
                }
            }
            if (a < 0) {
                return;
            }
            mm_reveal(a);
            for (int j = 0; j < MM_N; j++) {
                if (j != a && mm_state[j] == 0 &&
                    mm_known[j] == mm_card[a]) {
                    b = j;
                    break;
                }
            }
            if (b < 0) {
                for (int j = 0; j < MM_N; j++) {
                    if (j != a && mm_state[j] == 0 &&
                        mm_known[j] == 0) {
                        b = j;
                        break;
                    }
                }
                if (b < 0) {
                    mm_state[a] = 0;
                    return;
                }
                mm_reveal(b);
            }
        }
        if (mm_card[a] == mm_card[b]) {
            mm_state[a] = 2;
            mm_state[b] = 2;
            mm_known[a] = 0;
            mm_known[b] = 0;
            mm_score[1]++;
            if (mm_over_check()) {
                return;
            }
            /* match: cpu goes again */
        } else {
            mm_state[a] = 0;
            mm_state[b] = 0;
            return;
        }
    }
}

static void mm_player_flip(void) {
    int idx = mm_cursor;
    if (mm_state[idx] != 0) {
        return;
    }
    mm_reveal(idx);
    if (mm_first < 0) {
        mm_first = idx;
        return;
    }
    int a = mm_first;
    mm_first = -1;
    if (mm_card[a] == mm_card[idx]) {
        mm_state[a] = 2;
        mm_state[idx] = 2;
        mm_known[a] = 0;
        mm_known[idx] = 0;
        mm_score[0]++;
        mm_over_check();
        /* match: you go again */
    } else {
        mm_state[a] = 0;
        mm_state[idx] = 0;
        mm_turn = 1;
        mm_cpu_turn();
        mm_turn = 0;
        mm_over_check();
    }
}

static void draw_memory(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Cards - Memory", "カード - 神経衰弱"), acc, bg);
    if (mm_phase == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("Press n for a new game.", "nで新しいゲーム"), fg, bg);
        return;
    }
    for (int r = 0; r < 4; r++) {
        char line[96];
        size_t p = 0;
        for (int cix = 0; cix < 5; cix++) {
            int i = r * 5 + cix;
            int cur = i == mm_cursor && mm_phase == 1;
            line[p++] = cur ? '[' : ' ';
            if (mm_state[i] == 2) {
                line[p++] = ' ';
                line[p++] = ' ';
            } else if (mm_state[i] == 1) {
                int v = mm_card[i];
                if (v >= 10) {
                    line[p++] = '1';
                    line[p++] = '0';
                } else {
                    line[p++] = (char)('0' + v);
                    line[p++] = ' ';
                }
            } else {
                line[p++] = '?';
                line[p++] = '?';
            }
            line[p++] = cur ? ']' : ' ';
            line[p++] = ' ';
        }
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)((2 + r) * CELL_H), line, fg,
                     bg, 1);
    }
    {
        char line[64];
        size_t p = 0;
        const char *pre = L("Pairs - you: ", "ペア - あなた: ");
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        line[p++] = (char)('0' + mm_score[0]);
        const char *mid = L("  cpu: ", "  CPU: ");
        while (*mid != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *mid++;
        }
        line[p++] = (char)('0' + mm_score[1]);
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)(7 * CELL_H), line, fg, bg,
                     1);
    }
    if (mm_phase == 2) {
        draw_text_small(x0, y0 + (uint64_t)(8 * CELL_H),
                        mm_score[0] > mm_score[1]   ? L("You win!", "あなたの勝ち!")
                        : mm_score[0] < mm_score[1] ? L("CPU wins.", "CPUの勝ち")
                                                    : L("Draw.", "引き分け"),
                        acc, bg);
    } else {
        draw_text_small(x0, y0 + (uint64_t)(8 * CELL_H),
                        L("Arrows = move, Enter = flip", "矢印 = 移動、Enter = めくる"), col_dim(),
                        bg);
    }
    draw_text_small(x0, y0 + (uint64_t)(9 * CELL_H), L("n = new game", "n = 新規ゲーム"),
                    col_dim(), bg);
}

/* ----- menu + dispatch ----- */

static void draw_cards_menu(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Cards - choose a game", "カード - ゲームを選んでください"), acc, bg);
    for (int g = 1; g <= 7; g++) {
        char line[48];
        size_t p = 0;
        line[p++] = (char)('0' + g);
        line[p++] = ' ';
        const char *nm = cards_game_label(g);
        while (*nm != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *nm++;
        }
        line[p] = '\0';
        fb_draw_text(x0, y0 + (uint64_t)((1 + g) * CELL_H), line, fg,
                     bg, 1);
    }
    draw_text_small(x0, y0 + (uint64_t)(9 * CELL_H),
                    L("Press 1-7 to play. Esc in a game returns here.", "1-7でゲーム開始。ゲーム中のEscでここへ戻ります"),
                    col_dim(), bg);
}

static void cards_select_game(int g) {
    cards_game = g;
    serial_write("cards: game ");
    serial_write_line(CARDS_GAME_NAMES[g]);
    switch (g) {
    case 1:
        cards_phase = 0;
        break;
    case 2:
        pk_phase = 0;
        pk_new_game();
        break;
    case 3:
        bc_phase = 0;
        break;
    case 4:
        war_phase = 0;
        break;
    case 5:
        om_phase = 0;
        break;
    case 6:
        sv_phase = 0;
        break;
    case 7:
        mm_phase = 0;
        break;
    }
}

static int cards_handle_key(int c) {
    if (cards_game == 0) {
        if (c >= '1' && c <= '7') {
            cards_select_game(c - '0');
            desktop_redraw();
        }
        return 1;
    }
    switch (cards_game) {
    case 1: /* blackjack */
        if (c == 'n' || c == 'N') {
            cards_new_game();
            desktop_redraw();
        } else if (c == 'h' || c == 'H') {
            if (cards_phase == 1 && cards_player_n < 12 &&
                cards_pos < PRISM_DECK_SIZE) {
                cards_player[cards_player_n++] =
                    cards_deck[cards_pos++];
                if (cards_hand_value(cards_player,
                                     cards_player_n) > 21) {
                    cards_losses++;
                    cards_phase = 2;
                }
                desktop_redraw();
            }
        } else if (c == 's' || c == 'S') {
            if (cards_phase == 1) {
                cards_decide();
                desktop_redraw();
            }
        }
        return 1;
    case 2: /* poker */
        if (c == 'n' || c == 'N') {
            pk_new_game();
            desktop_redraw();
        } else if (pk_phase == 0 && c >= '1' && c <= '5') {
            pk_mark[c - '1'] = !pk_mark[c - '1'];
            desktop_redraw();
        } else if (pk_phase == 0 && (c == 'd' || c == 'D')) {
            pk_draw_cards();
            desktop_redraw();
        }
        return 1;
    case 3: /* baccarat */
        if (c == 'n' || c == 'N') {
            bc_new_game();
            desktop_redraw();
        }
        return 1;
    case 4: /* war */
        if (c == 'n' || c == 'N') {
            war_new_game();
            desktop_redraw();
        } else if (c == 'f' || c == 'F' || c == ' ') {
            war_battle();
            desktop_redraw();
        }
        return 1;
    case 5: /* old maid */
        if (c == 'n' || c == 'N') {
            om_new_game();
            desktop_redraw();
        } else if (c == DESK_KEY_LEFT) {
            if (om_sel > 0) {
                om_sel--;
            }
            desktop_redraw();
        } else if (c == DESK_KEY_RIGHT) {
            if (om_sel + 1 < om_cn) {
                om_sel++;
            }
            desktop_redraw();
        } else if (c == 'd' || c == 'D' || c == '\n' || c == '\r') {
            om_player_draw();
            desktop_redraw();
        }
        return 1;
    case 6: /* sevens */
        if (c == 'n' || c == 'N') {
            sv_new_game();
            sv_advance();
            desktop_redraw();
        } else if (sv_phase == 1 && sv_turn == 0) {
            if (c == DESK_KEY_LEFT) {
                if (sv_sel > 0) {
                    sv_sel--;
                }
                desktop_redraw();
            } else if (c == DESK_KEY_RIGHT) {
                if (sv_sel + 1 < sv_n[0]) {
                    sv_sel++;
                }
                desktop_redraw();
            } else if (c == '\n' || c == '\r') {
                if (sv_sel < sv_n[0] &&
                    sevens_playable(sv_lo, sv_hi,
                                    sv_hand[0][sv_sel])) {
                    sevens_place(sv_lo, sv_hi, sv_hand[0][sv_sel]);
                    sv_remove(0, sv_sel);
                    if (sv_sel >= sv_n[0] && sv_sel > 0) {
                        sv_sel--;
                    }
                    sv_turn = 1;
                    sv_advance();
                }
                desktop_redraw();
            } else if (c == 'p' || c == 'P') {
                sv_pass[0]++;
                if (sv_pass[0] >= 3) {
                    sv_out[0] = 1;
                    for (int i = 0; i < sv_n[0]; i++) {
                        sevens_place(sv_lo, sv_hi, sv_hand[0][i]);
                    }
                    sv_n[0] = 0;
                }
                sv_turn = 1;
                sv_advance();
                desktop_redraw();
            }
        }
        return 1;
    case 7: /* memory */
        if (c == 'n' || c == 'N') {
            mm_new_game();
            desktop_redraw();
        } else if (mm_phase == 1) {
            if (c == DESK_KEY_LEFT) {
                if (mm_cursor > 0) {
                    mm_cursor--;
                }
                desktop_redraw();
            } else if (c == DESK_KEY_RIGHT) {
                if (mm_cursor + 1 < MM_N) {
                    mm_cursor++;
                }
                desktop_redraw();
            } else if (c == DESK_KEY_UP) {
                if (mm_cursor >= 5) {
                    mm_cursor -= 5;
                }
                desktop_redraw();
            } else if (c == DESK_KEY_DOWN) {
                if (mm_cursor + 5 < MM_N) {
                    mm_cursor += 5;
                }
                desktop_redraw();
            } else if (c == '\n' || c == '\r') {
                mm_player_flip();
                desktop_redraw();
            }
        }
        return 1;
    }
    return 1;
}

static void draw_cards(void) {
    switch (cards_game) {
    case 1:
        draw_blackjack();
        break;
    case 2:
        draw_poker();
        break;
    case 3:
        draw_baccarat();
        break;
    case 4:
        draw_war();
        break;
    case 5:
        draw_oldmaid();
        break;
    case 6:
        draw_sevens();
        break;
    case 7:
        draw_memory();
        break;
    default:
        draw_cards_menu();
        break;
    }
}

/* ---- Shooter: a real 3D shooting game (raycasting) ---- */

/* Write a decimal number to the serial port (for game logs). */
static void shooter_serial_num(int v) {
    char buf[12];
    int n = 0;
    if (v == 0) {
        serial_write_char('0');
        return;
    }
    if (v < 0) {
        serial_write_char('-');
        v = -v;
    }
    while (v > 0 && n < 11) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        serial_write_char(buf[--n]);
    }
}

static uint32_t shooter_shade(uint32_t v, int percent) {
    uint8_t r = (uint8_t)(((v >> 16) & 0xFF) * (uint32_t)percent / 100);
    uint8_t g = (uint8_t)(((v >> 8) & 0xFF) * (uint32_t)percent / 100);
    uint8_t b = (uint8_t)((v & 0xFF) * (uint32_t)percent / 100);
    return rgb(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
}

#define SHOOTER_COL_STEP 4
#define SHOOTER_MAX_COLS 260

static void draw_shooter(void) {
    uint64_t vx = win_x();
    uint64_t vy = win_y() + TITLE_H;
    uint64_t vw = win_w();
    uint64_t vh = win_h() - TITLE_H;
    int cols = (int)(vw / SHOOTER_COL_STEP);
    if (cols > SHOOTER_MAX_COLS) {
        cols = SHOOTER_MAX_COLS;
    }
    static int32_t wall_dists[SHOOTER_MAX_COLS];
    /* Ceiling and floor fill the whole view first. */
    fb_fill_rect(vx, vy, vw, vh / 2, rgb(0x1A2038));
    fb_fill_rect(vx, vy + vh / 2, vw, vh - vh / 2, rgb(0x2A2A30));
    int64_t mid = (int64_t)(vy + vh / 2);
    for (int c = 0; c < cols; c++) {
        int64_t col_x = (int64_t)(vx + (uint64_t)(c * SHOOTER_COL_STEP));
        int centre = c * SHOOTER_COL_STEP + SHOOTER_COL_STEP / 2;
        int offset = (centre - (int)(vw / 2)) * SHOOTER_FOV_STEPS / (int)vw;
        int side = 0;
        int32_t dist = shooter_ray(shooter_game_state.px,
                                   shooter_game_state.py,
                                   shooter_game_state.angle + offset, &side);
        if (dist < SHOOTER_FX_ONE / 8) {
            dist = SHOOTER_FX_ONE / 8;
        }
        wall_dists[c] = dist;
        /* Wall height in pixels: view height / distance in cells. */
        int64_t line_h = ((int64_t)vh << 16) / dist;
        if (line_h > (int64_t)(vh * 2)) {
            line_h = (int64_t)(vh * 2);
        }
        int64_t y0 = mid - line_h / 2;
        int64_t y1 = mid + line_h / 2;
        int cells = (int)(dist >> 16);
        int bright = cells < 4 ? 100 : (cells < 8 ? 60 : 35);
        uint32_t base = side == 0 ? 0xB0503C : 0x7A3828;
        fb_fill_rect((uint64_t)col_x, (uint64_t)y0, SHOOTER_COL_STEP,
                     (uint64_t)(y1 - y0), shooter_shade(base, bright));
    }
    /* Enemies, far to near so near ones draw on top. Collect the
     * visible ones first, then sort by distance (at most 6). */
    int vis_idx[SHOOTER_MAX_ENEMIES];
    int32_t vis_sx[SHOOTER_MAX_ENEMIES];
    int32_t vis_dist[SHOOTER_MAX_ENEMIES];
    int vis_n = 0;
    for (int i = 0; i < shooter_game_state.enemy_count; i++) {
        shooter_enemy *e = &shooter_game_state.enemies[i];
        if (!e->alive) {
            continue;
        }
        int32_t sx = 0;
        int32_t ed = 0;
        if (!shooter_enemy_screen(&shooter_game_state, e, &sx, &ed)) {
            continue;
        }
        vis_idx[vis_n] = i;
        vis_sx[vis_n] = sx;
        vis_dist[vis_n] = ed;
        vis_n++;
    }
    for (int i = 0; i < vis_n; i++) {
        for (int j = i + 1; j < vis_n; j++) {
            if (vis_dist[j] > vis_dist[i]) {
                int ti = vis_idx[i];
                int32_t ts = vis_sx[i];
                int32_t td = vis_dist[i];
                vis_idx[i] = vis_idx[j];
                vis_sx[i] = vis_sx[j];
                vis_dist[i] = vis_dist[j];
                vis_idx[j] = ti;
                vis_sx[j] = ts;
                vis_dist[j] = td;
            }
        }
    }
    for (int k = 0; k < vis_n; k++) {
        int32_t sx = vis_sx[k];
        int32_t ed = vis_dist[k];
        (void)vis_idx[k];
        /* Occlusion: skip when a wall is closer at that column. */
        int64_t ex = (int64_t)(vx + (uint64_t)(((int64_t)sx * (int64_t)vw) >>
                                               16));
        int col = (int)((ex - (int64_t)vx) / SHOOTER_COL_STEP);
        if (col < 0) {
            col = 0;
        }
        if (col >= cols) {
            col = cols - 1;
        }
        if (ed >= wall_dists[col]) {
            continue;
        }
        int64_t line_h = ((int64_t)vh << 16) / ed;
        if (line_h > (int64_t)(vh * 2)) {
            line_h = (int64_t)(vh * 2);
        }
        int64_t sh = line_h * 9 / 10; /* enemy is ~0.9 cells tall */
        int64_t sw = sh * 2 / 3;
        int64_t bottom = mid + line_h / 2;
        int64_t top = bottom - sh;
        if (top < (int64_t)vy) {
            top = (int64_t)vy;
        }
        if (bottom > (int64_t)(vy + vh)) {
            bottom = (int64_t)(vy + vh);
        }
        /* Body (dark red) and head (skin tone). */
        fb_fill_rect((uint64_t)(ex - sw / 2), (uint64_t)(top + sh / 3),
                     (uint64_t)sw, (uint64_t)(sh * 2 / 3), rgb(0x8B1A1A));
        fb_fill_rect((uint64_t)(ex - sw / 6), (uint64_t)top,
                     (uint64_t)(sw / 3), (uint64_t)(sh / 3), rgb(0xE8C39A));
    }
    /* Crosshair in the middle of the view. */
    int64_t cx = (int64_t)(vx + vw / 2);
    uint32_t white = rgb(0xFFFFFF);
    fb_draw_line(cx - 8, mid, cx + 8, mid, white);
    fb_draw_line(cx, mid - 8, cx, mid + 8, white);
    if (shooter_hitmark) {
        uint32_t red = rgb(0xFF4040);
        fb_draw_line(cx - 10, mid - 10, cx + 10, mid + 10, red);
        fb_draw_line(cx - 10, mid + 10, cx + 10, mid - 10, red);
        shooter_hitmark = 0;
    }
    /* HUD: score, health, enemies left. */
    char hud[64];
    size_t p = 0;
    const char *h1 = L("Score: ", "スコア: ");
    while (*h1 != '\0' && p + 1 < sizeof(hud)) {
        hud[p++] = *h1++;
    }
    {
        char num[12];
        int ni = 0;
        int v = shooter_game_state.score;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[12];
            int ri = 0;
            while (v > 0 && ri < 11) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int i = 0; num[i] != '\0' && p + 1 < sizeof(hud); i++) {
            hud[p++] = num[i];
        }
    }
    const char *h2 = L("  Health: ", "  体力: ");
    while (*h2 != '\0' && p + 1 < sizeof(hud)) {
        hud[p++] = *h2++;
    }
    {
        char num[12];
        int ni = 0;
        int v = shooter_game_state.health;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[12];
            int ri = 0;
            while (v > 0 && ri < 11) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int i = 0; num[i] != '\0' && p + 1 < sizeof(hud); i++) {
            hud[p++] = num[i];
        }
    }
    const char *h3 = L("  Enemies: ", "  敵: ");
    while (*h3 != '\0' && p + 1 < sizeof(hud)) {
        hud[p++] = *h3++;
    }
    {
        char num[12];
        int ni = 0;
        int v = shooter_enemies_left(&shooter_game_state);
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[12];
            int ri = 0;
            while (v > 0 && ri < 11) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int i = 0; num[i] != '\0' && p + 1 < sizeof(hud); i++) {
            hud[p++] = num[i];
        }
    }
    hud[p] = '\0';
    draw_text_small(text_x(), text_y(), hud, col_text(), rgb(0x1A2038));
    if (shooter_game_state.phase == 2) {
        draw_text_small(text_x(), text_y() + 2 * CELL_H,
                        L("You win! Press n for a new game.", "勝利! nで新しいゲーム"), col_accent(),
                        rgb(0x1A2038));
    } else if (shooter_game_state.phase == 3) {
        draw_text_small(text_x(), text_y() + 2 * CELL_H,
                        L("Game over. Press n to retry.", "ゲームオーバー。nで再挑戦"), col_accent(),
                        rgb(0x1A2038));
    }
    draw_text_small(text_x(), vy + vh - 16,
                    L("W/S = move, A/D = turn, Space = shoot, mouse = aim", "W/S = 移動、A/D = 回転、Space = 射撃、マウス = 照準"),
                    col_dim(), rgb(0x2A2A30));
}

/* Fire one shot along the centre ray, and log the result. */
static void shooter_fire(void) {
    if (shooter_game_state.phase != 1) {
        return;
    }
    int side = 0;
    int32_t wall = shooter_ray(shooter_game_state.px,
                               shooter_game_state.py,
                               shooter_game_state.angle, &side);
    int left_before = shooter_enemies_left(&shooter_game_state);
    int hit = shooter_shoot(&shooter_game_state, wall);
    if (hit < 0) {
        serial_write_line("shooter: shot (miss)");
        return;
    }
    shooter_hitmark = 1;
    serial_write("shooter: hit! score=");
    shooter_serial_num(shooter_game_state.score);
    serial_write_line("");
    if (shooter_enemies_left(&shooter_game_state) < left_before) {
        serial_write("shooter: enemy down (");
        shooter_serial_num(shooter_enemies_left(&shooter_game_state));
        serial_write_line(" left)");
    }
    if (shooter_game_state.phase == 2) {
        serial_write("shooter: you win! score=");
        shooter_serial_num(shooter_game_state.score);
        serial_write_line("");
    }
}

/* ---- Sheet: real spreadsheet (10 x 20 grid) ---- */

/* The grid area: a 40 px gutter for row numbers, then 10
 * columns of 94 px. Rows are 16 px tall. */
#define SHEET_GUTTER 40
#define SHEET_COL_W 94
#define SHEET_ROW_H 16
/* 表示窓: グリッドは 26 列 x 50 行あるが、画面には
 * 10 列 x 20 行を切り取って選択セルを追尾する。 */
#define SHEET_VIEW_COLS 10
#define SHEET_VIEW_ROWS 20
static int sheet_view_col;
static int sheet_view_row;

/* Sheet の取り消し履歴 (セルの生文字列だけの
 * スナップショット。評価状態は復元時に再計算する)。 */
#define SHEET_UNDO_DEPTH 4
#define SHEET_CELLS (SHEET_ROWS * SHEET_COLS)
static char sheet_undo_raw[SHEET_UNDO_DEPTH][SHEET_CELLS]
                          [SHEET_CELL_MAX + 1];
static char sheet_redo_raw[SHEET_CELLS][SHEET_CELL_MAX + 1];
static int sheet_undo_count;
static int sheet_redo_valid;

static void sheet_snapshot(char *dst) {
    for (int i = 0; i < SHEET_CELLS; i++) {
        const char *raw = sheet_grid.cells[i].raw;
        for (int j = 0; j <= SHEET_CELL_MAX; j++) {
            dst[i * (SHEET_CELL_MAX + 1) + j] = raw[j];
        }
    }
}

static void sheet_restore(const char *src) {
    for (int i = 0; i < SHEET_CELLS; i++) {
        char *raw = sheet_grid.cells[i].raw;
        for (int j = 0; j <= SHEET_CELL_MAX; j++) {
            raw[j] = src[i * (SHEET_CELL_MAX + 1) + j];
        }
        sheet_grid.cells[i].state = 0;
    }
    sheet_recompute(&sheet_grid);
}

static void sheet_history_push(void) {
    if (sheet_undo_count > 0) {
        int same = 1;
        const char *top =
            &sheet_undo_raw[sheet_undo_count - 1][0][0];
        for (int i = 0; i < SHEET_CELLS && same; i++) {
            const char *raw = sheet_grid.cells[i].raw;
            const char *t = top + i * (SHEET_CELL_MAX + 1);
            for (int j = 0; j <= SHEET_CELL_MAX; j++) {
                if (raw[j] != t[j]) {
                    same = 0;
                    break;
                }
            }
        }
        if (same) {
            return;
        }
    }
    if (sheet_undo_count == SHEET_UNDO_DEPTH) {
        for (int i = 1; i < SHEET_UNDO_DEPTH; i++) {
            for (int k = 0; k < SHEET_CELLS; k++) {
                for (int j = 0; j <= SHEET_CELL_MAX; j++) {
                    sheet_undo_raw[i - 1][k][j] =
                        sheet_undo_raw[i][k][j];
                }
            }
        }
        sheet_undo_count--;
    }
    sheet_snapshot(&sheet_undo_raw[sheet_undo_count][0][0]);
    sheet_undo_count++;
    sheet_redo_valid = 0;
}

static int sheet_dirty;
/* 並べ替えとグラフ表示 (バッチ4 項目24)。 */
static int sheet_chart;

static int sheet_cell_cmp(const sheet_cell *a, const sheet_cell *b) {
    if (a->kind == 0 && b->kind == 0) {
        return 0;
    }
    if (a->kind == 1 && b->kind == 1) {
        if (a->value < b->value) {
            return -1;
        }
        return a->value > b->value ? 1 : 0;
    }
    const char *x = a->raw;
    const char *y = b->raw;
    while (*x != '\0' && *x == *y) {
        x++;
        y++;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

/* 全行を選択列の値で並べ替える。空セルは常に最後。
 * 取り消し履歴に積むので Ctrl+Z で戻せる。 */
static void sheet_sort(int col, int desc) {
    sheet_history_push();
    for (int i = 0; i < SHEET_ROWS - 1; i++) {
        int best = i;
        for (int j = i + 1; j < SHEET_ROWS; j++) {
            const sheet_cell *cj =
                &sheet_grid.cells[j * SHEET_COLS + col];
            const sheet_cell *cb =
                &sheet_grid.cells[best * SHEET_COLS + col];
            if (cj->kind == 0) {
                continue; /* 空は後ろのまま */
            }
            if (cb->kind == 0) {
                best = j;
                continue;
            }
            int cmp = sheet_cell_cmp(cj, cb);
            if (desc ? (cmp > 0) : (cmp < 0)) {
                best = j;
            }
        }
        if (best != i) {
            for (int c = 0; c < SHEET_COLS; c++) {
                sheet_cell *a = &sheet_grid.cells[i * SHEET_COLS + c];
                sheet_cell *b =
                    &sheet_grid.cells[best * SHEET_COLS + c];
                char tmp[SHEET_CELL_MAX + 1];
                for (int k = 0; k <= SHEET_CELL_MAX; k++) {
                    tmp[k] = a->raw[k];
                    a->raw[k] = b->raw[k];
                    b->raw[k] = tmp[k];
                }
                a->state = 0;
                b->state = 0;
            }
        }
    }
    sheet_recompute(&sheet_grid);
    sheet_dirty = 1;
    serial_write("sheet: sorted by column ");
    {
        char colname[2];
        colname[0] = (char)('A' + col);
        colname[1] = '\0';
        serial_write(colname);
    }
    serial_write_line(desc ? " desc" : " asc");
}

static void sheet_undo_apply(void) {
    if (sheet_undo_count == 0) {
        desktop_notify(L("Nothing to undo",
                         "取り消せる操作がありません"));
        return;
    }
    sheet_snapshot(&sheet_redo_raw[0][0]);
    sheet_redo_valid = 1;
    sheet_undo_count--;
    sheet_restore(&sheet_undo_raw[sheet_undo_count][0][0]);
    sheet_dirty = 1;
    serial_write_line("sheet: undo");
    desktop_refresh_console();
}

static void sheet_redo_apply(void) {
    if (!sheet_redo_valid) {
        desktop_notify(L("Nothing to redo",
                         "やり直せる操作がありません"));
        return;
    }
    if (sheet_undo_count < SHEET_UNDO_DEPTH) {
        sheet_snapshot(&sheet_undo_raw[sheet_undo_count][0][0]);
        sheet_undo_count++;
    }
    sheet_restore(&sheet_redo_raw[0][0]);
    sheet_redo_valid = 0;
    sheet_dirty = 1;
    serial_write_line("sheet: redo");
    desktop_refresh_console();
}
static char sheet_edit[SHEET_CELL_MAX + 1];
static int sheet_edit_len;
static int sheet_editing;
static int sheet_loaded;
static char sheet_io_buf[PRISM_VFS_MAX_APP_FILE_SIZE];

/* Sheet の検索置換 (Ctrl+F)。2 段階の入力行を使う。 */
static int sheet_fr_stage; /* 0 = off, 1 = find, 2 = replace */
static char sheet_fr_find[SHEET_CELL_MAX + 1];
static char sheet_fr_repl[SHEET_CELL_MAX + 1];
static int sheet_fr_len;

static void sheet_fr_execute(void) {
    size_t fl = 0;
    while (sheet_fr_find[fl] != '\0') {
        fl++;
    }
    if (fl == 0) {
        return;
    }
    sheet_history_push();
    int replaced = 0;
    for (int i = 0; i < SHEET_CELLS; i++) {
        const char *raw = sheet_grid.cells[i].raw;
        if (raw[0] == '\0') {
            continue;
        }
        /* 置換後の文字列を組み立てる。 */
        char out[SHEET_CELL_MAX + 1];
        size_t op = 0;
        int hit = 0;
        for (size_t k = 0; raw[k] != '\0'; k++) {
            size_t m = 0;
            while (m < fl && raw[k + m] == sheet_fr_find[m]) {
                m++;
            }
            if (m == fl) {
                hit = 1;
                for (size_t r = 0; sheet_fr_repl[r] != '\0' &&
                                   op < SHEET_CELL_MAX;
                     r++) {
                    out[op++] = sheet_fr_repl[r];
                }
                k += fl - 1;
            } else if (op < SHEET_CELL_MAX) {
                out[op++] = raw[k];
            }
        }
        out[op] = '\0';
        if (hit) {
            sheet_set(&sheet_grid, i % SHEET_COLS,
                      i / SHEET_COLS, out);
            replaced++;
        }
    }
    if (replaced > 0) {
        sheet_dirty = 1;
    }
    serial_write("sheet: replaced ");
    serial_write_num(replaced);
    serial_write_line("");
    desktop_notify(replaced > 0 ? L("Replaced",
                                    "置換しました")
                                : L("Not found",
                                    "見つかりませんでした"));
}

static int sheet_fr_key(int c) {
    char *buf = sheet_fr_stage == 1 ? sheet_fr_find : sheet_fr_repl;
    if (c == 0x1B) {
        sheet_fr_stage = 0;
        desktop_refresh_console();
        return 1;
    }
    if (c == '\n' || c == '\r') {
        buf[sheet_fr_len] = '\0';
        if (sheet_fr_stage == 1) {
            sheet_fr_stage = 2;
            sheet_fr_len = 0;
            while (sheet_fr_repl[sheet_fr_len] != '\0') {
                sheet_fr_len++;
            }
        } else {
            sheet_fr_stage = 0;
            sheet_fr_execute();
        }
        desktop_refresh_console();
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        if (sheet_fr_len > 0) {
            buf[--sheet_fr_len] = '\0';
        }
        desktop_refresh_console();
        return 1;
    }
    if (c >= 32 && c <= 126 && sheet_fr_len < SHEET_CELL_MAX) {
        buf[sheet_fr_len++] = (char)c;
        buf[sheet_fr_len] = '\0';
        desktop_refresh_console();
        return 1;
    }
    return 1;
}

static void sheet_commit_edit(void) {
    if (!sheet_editing) {
        return;
    }
    sheet_history_push();
    sheet_edit[sheet_edit_len] = '\0';
    sheet_set(&sheet_grid, sheet_sel_col, sheet_sel_row, sheet_edit);
    sheet_editing = 0;
    sheet_edit_len = 0;
    sheet_dirty = 1;
}

static void sheet_save(void) {
    int n = sheet_serialize(&sheet_grid, sheet_io_buf,
                            sizeof(sheet_io_buf));
    if (n < 0) {
        serial_write_line("sheet: too big to save (sheet.csv)");
        return;
    }
    if (vfs_write("sheet.csv", sheet_io_buf, (size_t)n) != 0) {
        serial_write_line("sheet: save failed");
        return;
    }
    sheet_dirty = 0;
    serial_write("sheet: saved sheet.csv (");
    /* Number of non-empty cells, for the log line. */
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (sheet_io_buf[i] == '\n') {
            count++;
        }
    }
    char num[21];
    int ni = 0;
    if (count == 0) {
        num[ni++] = '0';
    } else {
        char rev[21];
        int ri = 0;
        while (count > 0 && ri < 20) {
            rev[ri++] = (char)('0' + (count % 10));
            count /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    serial_write(num);
    serial_write_line(" cells)");
}

static void sheet_load(void) {
    size_t len = 0;
    const char *data = vfs_read_ptr("sheet.csv", &len);
    if (data == NULL) {
        serial_write_line("sheet: no sheet.csv yet");
        return;
    }
    int n = sheet_parse(&sheet_grid, data, len);
    sheet_loaded = 1;
    sheet_dirty = 0;
    sheet_editing = 0;
    sheet_edit_len = 0;
    serial_write("sheet: loaded sheet.csv (");
    char num[21];
    int ni = 0;
    if (n == 0) {
        num[ni++] = '0';
    } else {
        char rev[21];
        int ri = 0;
        while (n > 0 && ri < 20) {
            rev[ri++] = (char)('0' + (n % 10));
            n /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    serial_write(num);
    serial_write_line(" cells)");
}

static void draw_sheet(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    if (sheet_chart) {
        /* 棒グラフ: 選択列の数値を 0 を基線に描く。 */
        char title[48];
        size_t tp = 0;
        const char *th = L("Chart: column ", "グラフ: 列 ");
        while (*th != '\0' && tp + 1 < sizeof(title)) {
            title[tp++] = *th++;
        }
        title[tp++] = (char)('A' + sheet_sel_col);
        title[tp] = '\0';
        draw_text_small(x0, y0, title, acc, bg);
        int64_t vals[SHEET_ROWS];
        int rows_of[SHEET_ROWS];
        int n = 0;
        int64_t vmax = 0;
        int64_t vmin = 0;
        for (int r = 0; r < SHEET_ROWS && n < 30; r++) {
            const sheet_cell *cell =
                &sheet_grid.cells[r * SHEET_COLS + sheet_sel_col];
            if (cell->kind == 1) {
                vals[n] = cell->value;
                rows_of[n] = r;
                if (cell->value > vmax) {
                    vmax = cell->value;
                }
                if (cell->value < vmin) {
                    vmin = cell->value;
                }
                n++;
            }
        }
        uint64_t plot_y = y0 + (uint64_t)(3 * CELL_H);
        uint64_t plot_h = (uint64_t)(text_rows() - 6) * CELL_H;
        if (n == 0 || vmax - vmin <= 0) {
            draw_text_small(x0, plot_y,
                            L("No numeric data in this column.",
                              "この列に数値データがありません"),
                            fg, bg);
        } else {
            uint64_t zero_y = plot_y + (uint64_t)(
                (int64_t)plot_h * vmax / (vmax - vmin));
            fb_fill_rect(x0, zero_y, win_w() - 16, 1, dim);
            uint64_t bw = (win_w() - 32) / (uint64_t)n;
            if (bw > 48) {
                bw = 48;
            }
            if (bw < 4) {
                bw = 4;
            }
            for (int i = 0; i < n; i++) {
                uint64_t bx = x0 + 8 + (uint64_t)i * ((win_w() - 32) /
                                                      (uint64_t)n);
                uint64_t bh = (uint64_t)(
                    (int64_t)plot_h *
                    (vals[i] >= 0 ? vals[i] : -vals[i]) /
                    (vmax - vmin));
                if (bh == 0) {
                    bh = 1;
                }
                if (vals[i] >= 0) {
                    fb_fill_rect(bx, zero_y - bh, bw - 2, bh, acc);
                } else {
                    fb_fill_rect(bx, zero_y + 1, bw - 2, bh, acc);
                }
                char rn[4];
                int rnum = rows_of[i] + 1;
                rn[0] = (char)('0' + (rnum / 10) % 10);
                rn[1] = (char)('0' + rnum % 10);
                rn[2] = '\0';
                fb_draw_text(bx, plot_y + plot_h + 2, rn, dim, bg,
                             1);
            }
        }
        draw_text_small(x0,
                        y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                        L("G = back to grid",
                          "G = 表に戻る"),
                        dim, bg);
        return;
    }
    /* Formula bar: the selected cell and what is in it. */
    char bar[64];
    size_t p = 0;
    if (sheet_fr_stage > 0) {
        const char *lab = sheet_fr_stage == 1
            ? L("Find: ", "検索: ")
            : L("Replace with: ", "置換: ");
        const char *val = sheet_fr_stage == 1 ? sheet_fr_find
                                              : sheet_fr_repl;
        while (*lab != '\0' && p + 1 < sizeof(bar)) {
            bar[p++] = *lab++;
        }
        while (*val != '\0' && p + 1 < sizeof(bar)) {
            bar[p++] = *val++;
        }
        bar[p] = '\0';
    } else {
        bar[p++] = (char)('A' + sheet_sel_col);
        int rn = sheet_sel_row + 1;
        if (rn >= 10) {
            bar[p++] = (char)('0' + (rn / 10));
        }
        bar[p++] = (char)('0' + (rn % 10));
        bar[p++] = ':';
        bar[p++] = ' ';
        const char *shown =
            sheet_editing
                ? sheet_edit
                : sheet_raw(&sheet_grid, sheet_sel_col,
                            sheet_sel_row);
        if (shown != NULL) {
            while (*shown != '\0' && p + 1 < sizeof(bar)) {
                bar[p++] = *shown++;
            }
        }
        bar[p] = '\0';
    }
    draw_text_small(x0, y0, bar, acc, bg);
    /* ビューを選択セルに追尾させる。 */
    if (sheet_sel_col < sheet_view_col) {
        sheet_view_col = sheet_sel_col;
    }
    if (sheet_sel_col >= sheet_view_col + SHEET_VIEW_COLS) {
        sheet_view_col = sheet_sel_col - SHEET_VIEW_COLS + 1;
    }
    if (sheet_sel_row < sheet_view_row) {
        sheet_view_row = sheet_sel_row;
    }
    if (sheet_sel_row >= sheet_view_row + SHEET_VIEW_ROWS) {
        sheet_view_row = sheet_sel_row - SHEET_VIEW_ROWS + 1;
    }
    /* Column headers. */
    uint64_t hy = y0 + SHEET_ROW_H;
    for (int col = 0; col < SHEET_VIEW_COLS; col++) {
        uint64_t cx = x0 + SHEET_GUTTER + (uint64_t)(col * SHEET_COL_W);
        char hdr[2];
        hdr[0] = (char)('A' + sheet_view_col + col);
        hdr[1] = '\0';
        draw_text_small(cx + SHEET_COL_W / 2 - 3, hy, hdr, dim, bg);
    }
    /* Grid lines and cells. */
    uint64_t grid_y = y0 + 2 * SHEET_ROW_H;
    for (int row = 0; row < SHEET_VIEW_ROWS; row++) {
        uint64_t ry = grid_y + (uint64_t)(row * SHEET_ROW_H);
        /* Row number, right aligned in the gutter. */
        char num[4];
        int ni = 0;
        int v = sheet_view_row + row + 1;
        if (v >= 10) {
            num[ni++] = (char)('0' + (v / 10));
        }
        num[ni++] = (char)('0' + (v % 10));
        num[ni] = '\0';
        draw_text_small(x0 + SHEET_GUTTER - 6 - (uint64_t)(ni * 6), ry,
                        num, dim, bg);
        for (int col = 0; col < SHEET_VIEW_COLS; col++) {
            int acol = sheet_view_col + col;
            int arow = sheet_view_row + row;
            uint64_t cx = x0 + SHEET_GUTTER + (uint64_t)(col * SHEET_COL_W);
            int selected =
                (acol == sheet_sel_col && arow == sheet_sel_row);
            uint32_t cell_bg = selected ? acc : bg;
            uint32_t cell_fg = selected ? rgb(0xFFFFFF) : fg;
            if (selected) {
                fb_fill_rect(cx, ry, SHEET_COL_W, SHEET_ROW_H, acc);
            }
            char disp[32];
            if (selected && sheet_editing) {
                size_t i = 0;
                while (sheet_edit[i] != '\0' && i + 1 < sizeof(disp)) {
                    disp[i] = sheet_edit[i];
                    i++;
                }
                if (i + 1 < sizeof(disp)) {
                    disp[i++] = '_';
                }
                disp[i] = '\0';
            } else {
                (void)sheet_display(&sheet_grid, acol, arow, disp,
                                    sizeof(disp));
            }
            /* Show at most 14 characters (the column is 94 px). */
            char shown14[15];
            size_t i = 0;
            while (disp[i] != '\0' && i < 14) {
                shown14[i] = disp[i];
                i++;
            }
            shown14[i] = '\0';
            fb_draw_text(cx + 2, ry, shown14, cell_fg, cell_bg, 1);
        }
    }
    /* Grid lines (1 px, dim). */
    for (int col = 0; col <= SHEET_VIEW_COLS; col++) {
        uint64_t lx = x0 + SHEET_GUTTER + (uint64_t)(col * SHEET_COL_W);
        fb_fill_rect(lx, grid_y, 1,
                     (uint64_t)(SHEET_VIEW_ROWS * SHEET_ROW_H), dim);
    }
    for (int row = 0; row <= SHEET_VIEW_ROWS; row++) {
        uint64_t ly = grid_y + (uint64_t)(row * SHEET_ROW_H);
        fb_fill_rect(x0 + SHEET_GUTTER, ly,
                     (uint64_t)(SHEET_VIEW_COLS * SHEET_COL_W), 1, dim);
    }
    draw_text_small(
        x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
        L("Arrows = move, type = edit, s = save, l = load, o/O = sort, g = chart", "矢印 = 移動、入力 = 編集、s = 保存、l = 読込、o/O = 並べ替え、g = グラフ"),
        dim, bg);
}

/* ---- Video: real compressed "vid-*.pv1" frames (or the old
 * "vid-*.bmp" sequence) at 4 fps ---- */

static void video_rebuild(void) {
    int n = vfs_file_count();
    const char *names[PRISM_VFS_MAX_FILES];
    int m = 0;
    for (int i = 0; i < n && m < PRISM_VFS_MAX_FILES; i++) {
        names[m++] = vfs_file_name(i);
    }
    int idx[VIDEO_MAX_FRAMES];
    int found = video_collect_pv1(names, m, idx, VIDEO_MAX_FRAMES);
    video_is_pv1 = 1;
    if (found <= 0) {
        /* No compressed frames: fall back to the old BMP
         * sequence, which still plays. */
        found = video_collect(names, m, idx, VIDEO_MAX_FRAMES);
        video_is_pv1 = 0;
    }
    video_decoded_frame = -1;
    video_pv1_w = 0;
    video_pv1_h = 0;
    video_frame_count = found;
    for (int i = 0; i < found; i++) {
        /* video_collect returns indices into names[], and names[]
         * is in VFS order, so the VFS index is the same number. */
        video_frames[i] = idx[i];
    }
    video_init(&video_player_state, found);
    if (found > 0) {
        serial_write("video: ");
        char num[21];
        int ni = 0;
        int v = found;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            char rev[21];
            int ri = 0;
            while (v > 0 && ri < 20) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        serial_write(num);
        serial_write_line(" frames");
    } else {
        serial_write_line("video: no frames (no vid-*.pv1 or vid-*.bmp files)");
    }
}

/* target 番の PV1 フレームを video_pixels に読む。直前の
 * キーフレームから順に復号する。成功すれば 0。 */
static int video_load_frame(int target) {
    if (!video_is_pv1 || target < 0 || target >= video_frame_count) {
        return -1;
    }
    if (video_decoded_frame == target) {
        return 0;
    }
    int start = pv1_keyframe_for(target);
    if (video_decoded_frame >= start && video_decoded_frame < target) {
        /* 今のフレームから続けられる (差分は前の絵に当てる)。 */
        start = video_decoded_frame + 1;
    }
    for (int i = start; i <= target; i++) {
        const char *name = vfs_file_name(video_frames[i]);
        size_t len = 0;
        const char *content = vfs_read_ptr(name, &len);
        if (content == NULL) {
            return -1;
        }
        pv1_header hdr;
        if (pv1_parse_header((const uint8_t *)content, len, &hdr) != 0) {
            return -1;
        }
        int rc;
        if (hdr.type == PV1_TYPE_KEY) {
            rc = pv1_decode_key((const uint8_t *)content, len,
                                video_pixels, PV1_MAX_PIXELS,
                                &video_pv1_w, &video_pv1_h);
        } else {
            rc = pv1_decode_diff((const uint8_t *)content, len,
                                 video_pixels, PV1_MAX_PIXELS,
                                 video_scratch, sizeof(video_scratch),
                                 &video_pv1_w, &video_pv1_h);
        }
        if (rc != 0) {
            return -1;
        }
        video_decoded_frame = i;
    }
    return 0;
}

/* 今表示している PV1 フレームに付いた音声トラックを鳴らす。
 * 音声が付いていれば 1 (鳴らした)、なければ 0 を返す。
 * 音声は 1 フレーム分 (4 fps で 0.25 秒) で、再生が終わる
 * まで戻らない。音声の長さがフレームの表示時間と等しい
 * ため、再生が終わるころ次のフレームの番になる。 */
static int video_play_frame_audio(void) {
    if (!video_is_pv1 || video_frame_count <= 0 ||
        video_player_state.frame < 0 ||
        video_player_state.frame >= video_frame_count) {
        return 0;
    }
    const char *name =
        vfs_file_name(video_frames[video_player_state.frame]);
    size_t len = 0;
    const char *content = vfs_read_ptr(name, &len);
    if (content == NULL) {
        return 0;
    }
    int audio_len = pv1_audio_len((const uint8_t *)content, len);
    if (audio_len <= 0) {
        return 0;
    }
    const uint8_t *audio = pv1_audio_data((const uint8_t *)content, len);
    if (audio == NULL) {
        return 0;
    }
    if (!sb16_present()) {
        return 0;
    }
    serial_write("video: audio frame ");
    shooter_serial_num(video_player_state.frame + 1);
    serial_write_line("");
    if (sb16_play_8bit(audio, (size_t)audio_len, PV1_AUDIO_RATE) != 0) {
        return 0;
    }
    return 1;
}

static void video_arm_next_tick(void) {
    if (video_calibrated) {
        video_next_tick = (int64_t)tick_counter +
                          video_ticks_per_frame(video_ticks_per_sec);
    } else {
        /* Not calibrated yet: the next RTC second change arms
         * the tick (see desktop_tick). */
        video_next_tick = 0;
    }
}

static void draw_video(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    if (video_frame_count <= 0) {
        draw_text_small(x0, y0, L("Video - no frames yet", "動画 - フレームがまだありません"), acc, bg);
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("(add vid-01.pv1, vid-02.pv1, ... to the file system)", "(vid-01.pv1, vid-02.pv1, ... をファイルシステムに追加)"),
                        fg, bg);
        return;
    }
    const char *name =
        vfs_file_name(video_frames[video_player_state.frame]);
    size_t len = 0;
    const char *content = vfs_read_ptr(name, &len);
    char hud[64];
    size_t p = 0;
    const char *h = L("Video - frame ", "動画 - フレーム ");
    while (*h != '\0' && p + 1 < sizeof(hud)) {
        hud[p++] = *h++;
    }
    char num[21];
    int ni = 0;
    int v = video_player_state.frame + 1;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[21];
        int ri = 0;
        while (v > 0 && ri < 20) {
            rev[ri++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    for (int k = 0; num[k] != '\0' && p + 1 < sizeof(hud); k++) {
        hud[p++] = num[k];
    }
    const char *mid = " / ";
    while (*mid != '\0' && p + 1 < sizeof(hud)) {
        hud[p++] = *mid++;
    }
    ni = 0;
    v = video_frame_count;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[21];
        int ri = 0;
        while (v > 0 && ri < 20) {
            rev[ri++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni] = '\0';
    for (int k = 0; num[k] != '\0' && p + 1 < sizeof(hud); k++) {
        hud[p++] = num[k];
    }
    const char *st = video_player_state.playing ? L(" - playing", " - 再生中")
                                               : L(" - paused", " - 一時停止");
    while (*st != '\0' && p + 1 < sizeof(hud)) {
        hud[p++] = *st++;
    }
    if (video_is_pv1) {
        const char *fmt = " (PV1)";
        while (*fmt != '\0' && p + 1 < sizeof(hud)) {
            hud[p++] = *fmt++;
        }
    }
    hud[p] = '\0';
    draw_text_small(x0, y0, hud, acc, bg);
    /* The frame, drawn large in the middle of the window. */
    uint64_t iw = 384;
    uint64_t ih = 384;
    uint64_t ix = x0 + (win_w() - 16 - iw) / 2;
    uint64_t iy = y0 + (uint64_t)(2 * CELL_H);
    if (video_is_pv1) {
        /* 320x240 なら 384x288 (4:3) にする。先に読んでから
         * 大きさを決める。 */
        int loaded =
            video_load_frame(video_player_state.frame) == 0;
        if (video_pv1_w > 0 && video_pv1_h > 0) {
            ih = iw * (uint64_t)video_pv1_h / (uint64_t)video_pv1_w;
        } else {
            ih = 288;
        }
        if (loaded) {
            fb_draw_image_rgb565(ix, iy, iw, ih, video_pixels,
                                 (int)video_pv1_w, (int)video_pv1_h);
        } else {
            draw_text_small(ix, iy, L("Cannot decode this frame.", "このフレームをデコードできません"), fg, bg);
        }
    } else {
        wallpaper_img img;
        if (content != NULL &&
            wallpaper_parse((const uint8_t *)content, len, &img) == 0) {
            fb_draw_image_rgb24(ix, iy, iw, ih, img.pixels, img.width,
                                img.height, img.stride, img.top_down);
        } else {
            draw_text_small(ix, iy, L("Not a 24-bit BMP file.", "24ビットBMPファイルではありません"), fg, bg);
        }
    }
    /* Progress bar under the frame. */
    uint64_t bw = 384;
    uint64_t bx = ix;
    uint64_t by = iy + ih + 16;
    fb_fill_rect(bx, by, bw, 8, dim);
    uint64_t fill = (uint64_t)(video_player_state.frame + 1) * bw /
                    (uint64_t)video_frame_count;
    if (fill > 0) {
        fb_fill_rect(bx, by, fill, 8, acc);
    }
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("Space = play/pause, Left/Right = seek, n/p = frame, r = restart, +/- = volume", "Space = 再生/停止、左右 = シーク、n/p = フレーム、r = 最初から、+/- = 音量"),
                    dim, bg);
}

/* ---- Search / window list (F8) ---- */

/* Defined below (near desktop_handle_key). */
static void switch_mode(desktop_mode m);

static const char *search_setting_names[10] = {
    "Accent color", "Taskbar position", "Dark mode", "Wallpaper",
    "Accounts", "Security", "Updates", "Clock", "Network",
    "Administration",
};

/* 検索結果に出す設定名 (言語切替つき)。上の表は照合用に
 * 英語のまま。 */
static const char *search_setting_label(int i) {
    switch (i) {
        case 0:
            return L("Accent color", "アクセントカラー");
        case 1:
            return L("Taskbar position", "タスクバーの位置");
        case 2:
            return L("Dark mode", "ダークモード");
        case 3:
            return L("Wallpaper", "壁紙");
        case 4:
            return L("Accounts", "アカウント");
        case 5:
            return L("Security", "セキュリティ");
        case 6:
            return L("Updates", "更新");
        case 7:
            return L("Clock", "時計");
        case 8:
            return L("Network", "ネットワーク");
        default:
            return L("Administration", "管理");
    }
}

static void search_rebuild(void);

/* 検索欄への IME 確定文字列の挿入 (バイト列のまま
 * 追加し、検索はバイト一致で行う)。 */
static void search_insert_text(const char *text) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (search_len + 1 < (int)sizeof(search_query)) {
            search_query[search_len++] = text[i];
        }
    }
    search_query[search_len] = '\0';
    search_rebuild();
}

static void search_rebuild(void) {
    search_count = 0;
    search_selected = 0;
    for (int i = 0; i < desk_visible_count() &&
         search_count < SEARCH_MAX_RESULTS;
         i++) {
        desktop_mode app = desk_visible_mode(i);
        if (search_match(search_query, mode_name(app))) {
            search_results[search_count].kind = 0;
            search_results[search_count].index = (int)app;
            search_count++;
        }
    }
    int n = vfs_file_count();
    for (int i = 0; i < n && search_count < SEARCH_MAX_RESULTS; i++) {
        const char *name = vfs_file_name(i);
        if (name != NULL && search_match(search_query, name)) {
            search_results[search_count].kind = 1;
            search_results[search_count].index = i;
            search_count++;
        }
    }
    for (int i = 0; i < 10 && search_count < SEARCH_MAX_RESULTS; i++) {
        if (search_match(search_query, search_setting_names[i])) {
            search_results[search_count].kind = 2;
            search_results[search_count].index = i;
            search_count++;
        }
    }
}

static void search_activate(void) {
    if (search_count == 0) {
        return;
    }
    search_result r = search_results[search_selected];
    search_active = 0;
    if (r.kind == 0) {
        switch_mode((desktop_mode)r.index);
    } else if (r.kind == 1) {
        desk_mode = DESK_FILES;
        files_selected = r.index;
        files_viewing = r.index;
        desktop_redraw();
    } else {
        switch_mode(DESK_SETTINGS);
        /* 4/5/6/7 はアカウント/セキュリティ/更新/時計へ。 */
        settings_page = r.index >= 4 ? r.index - 2 : 0;
        if (settings_page == 4) {
            upd_refresh_snaps();
        }
        if (settings_page == 5) {
            clk_refresh();
        }
        if (settings_page == 6) {
            net_refresh();
        }
        if (settings_page == 7) {
            adm_refresh();
        }
        desktop_redraw();
    }
}

static void draw_search(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t panel = col_title_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    int rows = search_count + 3;
    if (rows > text_rows()) {
        rows = text_rows();
    }
    fb_fill_rect(x0 - 4, y0 - 4, win_w() - 8,
                 (uint64_t)(rows * CELL_H) + 8, panel);
    char line[96];
    size_t p = 0;
    const char *pre = L("Search: ", "検索: ");
    while (*pre != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *pre++;
    }
    for (int i = 0; i < search_len && p + 1 < sizeof(line); i++) {
        line[p++] = search_query[i];
    }
    line[p++] = '_';
    line[p] = '\0';
    draw_text_small(x0, y0, line, acc, panel);
    for (int i = 0; i < search_count && i + 2 < text_rows(); i++) {
        search_result r = search_results[i];
        char row[96];
        size_t q = 0;
        const char *kind = r.kind == 0 ? L("App: ", "アプリ: ")
                           : r.kind == 1 ? L("File: ", "ファイル: ")
                                         : L("Setting: ", "設定: ");
        while (*kind != '\0' && q + 1 < sizeof(row)) {
            row[q++] = *kind++;
        }
        const char *name;
        if (r.kind == 0) {
            name = mode_name((desktop_mode)r.index);
        } else if (r.kind == 1) {
            name = vfs_file_name(r.index);
            if (name == NULL) {
                name = "?";
            }
        } else {
            name = search_setting_label(r.index);
        }
        while (*name != '\0' && q + 1 < sizeof(row)) {
            row[q++] = *name++;
        }
        row[q] = '\0';
        uint64_t py = y0 + (uint64_t)((i + 2) * CELL_H);
        if (i == search_selected) {
            fb_fill_rect(x0, py, win_w() - 16, CELL_H, acc);
            fb_draw_text(x0 + 4, py, row, rgb(0xFFFFFF), acc, 1);
        } else {
            fb_draw_text(x0 + 4, py, row, fg, panel, 1);
        }
    }
    if (search_count == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), L("(no matches)", "(一致なし)"),
                        col_dim(), panel);
    }
    draw_text_small(x0, y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    "Enter = open, Esc = close", col_dim(), panel);
    (void)bg;
}

/* ---- Installer ---- */

static void inst_u64(char *buf, size_t cap, uint64_t v) {
    char tmp[21];
    int i = 0;
    if (cap == 0) {
        return;
    }
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    size_t p = 0;
    while (i > 0 && p + 1 < cap) {
        buf[p++] = tmp[--i];
    }
    buf[p] = '\0';
}

static void inst_line(uint64_t x0, uint64_t y0, int row, const char *s,
                      uint32_t fg, uint32_t bg) {
    draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), s, fg, bg);
}

static void inst_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void inst_append(char *dst, size_t cap, const char *src) {
    size_t p = strlen(dst);
    size_t i = 0;
    while (src[i] != '\0' && p + 1 < cap) {
        dst[p++] = src[i++];
    }
    dst[p] = '\0';
}

/* Read the disk again and reset the installer to its first
 * step. Called when the app is opened. */
static void installer_refresh(void) {
    inst_step = INSTALL_STEP_WELCOME;
    inst_target = 0;
    inst_profile = -1;
    inst_progress = 0;
    inst_phase[0] = '\0';
    inst_error[0] = '\0';
    inst_verified = 0;
    inst_has_disk = persist_disk_present();
    inst_copy(inst_model, sizeof(inst_model), persist_disk_model());
    inst_sectors = persist_disk_sectors();
    inst_kind = install_classify(inst_has_disk, persist_installed(),
                                 persist_data_partition_present());
    inst_part_count = 0;
    static uint8_t mbr[INSTALL_MBR_SIZE];
    if (inst_has_disk && persist_read_mbr(mbr) == 0) {
        int n = install_mbr_partitions(mbr, inst_parts,
                                       INSTALL_PARTITION_COUNT);
        inst_part_count = n < 0 ? 0 : n;
    }
    /* The install set is the system files. A boot from an empty
     * installed disk wipes them from the live file system
     * (persist_load clears the built-ins), so restore any
     * missing built-in files now; user files are untouched.
     * This also makes the profile file counts on the next
     * step honest. */
    int reseeded = vfs_repair_builtins();
    if (reseeded > 0) {
        serial_write("installer: restored ");
        {
            char num[24];
            inst_u64(num, sizeof(num), (uint64_t)reseeded);
            serial_write(num);
        }
        serial_write_line(" built-in file(s)");
    }
    serial_write_line("installer: opened");
}

static int inst_minimal_count(void) {
    int n = 0;
    int total = vfs_file_count();
    for (int i = 0; i < total; i++) {
        const char *name = vfs_file_name(i);
        if (name != NULL && install_include_file(name, 1)) {
            n++;
        }
    }
    return n;
}

static void draw_installer(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    char line[128];
    char num[24];

    inst_line(x0, y0, 0, L("PrismOS Installer", "PrismOS インストーラ"), acc, bg);
    if (inst_step == INSTALL_STEP_WELCOME) {
        inst_line(x0, y0, 2,
                  L("This installs PrismOS to the attached disk.", "PrismOS を接続中のディスクにインストールします"), fg, bg);
        inst_line(x0, y0, 3,
                  L("The disk will be erased. Files on it are lost.", "ディスクは消去されます。中身のファイルは失われます"), fg,
                  bg);
        inst_line(x0, y0, 5, L("Enter = continue, Esc = back to Terminal", "Enter = 続行、Esc = ターミナルへ戻る"),
                  dim, bg);
        return;
    }
    if (inst_step == INSTALL_STEP_DISK) {
        if (!inst_has_disk) {
            inst_line(x0, y0, 2,
                      L("No disk found. Attach a disk and reopen", "ディスクが見つかりません。ディスクを接続して"), fg, bg);
            inst_line(x0, y0, 3, L("the Installer.", "インストーラを開き直してください"), fg, bg);
            inst_line(x0, y0, 5, L("Esc = back to Terminal", "Esc = ターミナルへ戻る"), dim, bg);
            return;
        }
        line[0] = '\0';
        inst_append(line, sizeof(line), L("Disk: ", "ディスク: "));
        inst_append(line, sizeof(line), inst_model);
        inst_append(line, sizeof(line), " (");
        inst_u64(num, sizeof(num), (uint64_t)(inst_sectors / 2048));
        inst_append(line, sizeof(line), num);
        inst_append(line, sizeof(line), " MB)");
        inst_line(x0, y0, 2, line, fg, bg);
        if (inst_kind == INSTALL_DISK_BOOTABLE) {
            inst_line(x0, y0, 3,
                      L("Bootable disk: install to the data partition.", "起動可能ディスク: データパーティションにインストール"),
                      fg, bg);
        } else if (inst_kind == INSTALL_DISK_DATA_ONLY) {
            inst_line(x0, y0, 3,
                      L("Data disk: files only, it cannot boot alone.", "データディスク: ファイル専用で、単独では起動できません"),
                      fg, bg);
        } else {
            inst_line(x0, y0, 3,
                      L("Blank disk: files only, it cannot boot alone.", "空のディスク: ファイル専用で、単独では起動できません"),
                      fg, bg);
        }
        for (int i = 0; i < inst_part_count; i++) {
            line[0] = '\0';
            inst_append(line, sizeof(line), "  ");
            num[0] = (char)('1' + i);
            num[1] = '\0';
            inst_append(line, sizeof(line), num);
            inst_append(line, sizeof(line), L(": type 0x", ": 種別 0x"));
            static const char HEX[] = "0123456789ABCDEF";
            char hx[3];
            hx[0] = HEX[(inst_parts[i].type >> 4) & 0xF];
            hx[1] = HEX[inst_parts[i].type & 0xF];
            hx[2] = '\0';
            inst_append(line, sizeof(line), hx);
            inst_append(line, sizeof(line), L(", LBA ", "、LBA "));
            inst_u64(num, sizeof(num), inst_parts[i].start_lba);
            inst_append(line, sizeof(line), num);
            inst_append(line, sizeof(line), ", ");
            inst_u64(num, sizeof(num),
                     (uint64_t)(inst_parts[i].sectors / 2048));
            inst_append(line, sizeof(line), num);
            inst_append(line, sizeof(line), " MB");
            if (inst_parts[i].bootable) {
                inst_append(line, sizeof(line), L(" (boot)", " (起動)"));
            }
            inst_line(x0, y0, 4 + i, line, fg, bg);
        }
        if (inst_part_count == 0) {
            inst_line(x0, y0, 4, L("  (no partitions)", "  (パーティションなし)"), fg, bg);
        }
        line[0] = '\0';
        inst_append(line, sizeof(line), L("Target: ", "対象: "));
        inst_append(line, sizeof(line),
                    inst_target == 1   ? "data partition"
                    : inst_target == 2 ? "whole disk"
                                       : "Automatic");
        inst_line(x0, y0, 9, line, fg, bg);
        inst_line(x0, y0, 10,
                  L("a = automatic, d = data partition, w = whole disk", "a = 自動、d = データパーティション、w = ディスク全体"),
                  fg, bg);
        if (inst_error[0] != '\0') {
            inst_line(x0, y0, 11, inst_error, acc, bg);
        }
        inst_line(x0, y0, 13, L("Enter = continue, Esc = back", "Enter = 続行、Esc = 戻る"), dim, bg);
        return;
    }
    if (inst_step == INSTALL_STEP_PROFILE) {
        line[0] = '\0';
        inst_append(line, sizeof(line), L("1  Standard - all files (", "1  標準 - 全ファイル ("));
        inst_u64(num, sizeof(num), (uint64_t)vfs_file_count());
        inst_append(line, sizeof(line), num);
        inst_append(line, sizeof(line), L(" files)", " ファイル)"));
        if (inst_profile == 0) {
            inst_append(line, sizeof(line), "  *");
        }
        inst_line(x0, y0, 2, line, fg, bg);
        line[0] = '\0';
        inst_append(line, sizeof(line),
                    L("2  Minimal - browser, file manager, settings (", "2  最小 - ブラウザ、ファイル管理、設定 ("));
        inst_u64(num, sizeof(num), (uint64_t)inst_minimal_count());
        inst_append(line, sizeof(line), num);
        inst_append(line, sizeof(line), L(" files)", " ファイル)"));
        if (inst_profile == 1) {
            inst_append(line, sizeof(line), "  *");
        }
        inst_line(x0, y0, 3, line, fg, bg);
        inst_line(x0, y0, 5, L("Enter = continue, Esc = back", "Enter = 続行、Esc = 戻る"), dim, bg);
        return;
    }
    if (inst_step == INSTALL_STEP_CONFIRM) {
        line[0] = '\0';
        inst_append(line, sizeof(line), L("Disk: ", "ディスク: "));
        inst_append(line, sizeof(line), inst_model);
        inst_line(x0, y0, 2, line, fg, bg);
        line[0] = '\0';
        inst_append(line, sizeof(line), L("Target: ", "対象: "));
        inst_append(line, sizeof(line),
                    inst_target == 1   ? "data partition"
                    : inst_target == 2 ? "whole disk"
                                       : "Automatic");
        inst_line(x0, y0, 3, line, fg, bg);
        line[0] = '\0';
        inst_append(line, sizeof(line), L("Profile: ", "プロファイル: "));
        inst_append(line, sizeof(line),
                    install_profile_name(inst_profile == 1));
        inst_line(x0, y0, 4, line, fg, bg);
        inst_line(x0, y0, 5, L("This erases the disk.", "ディスクを消去します"), fg, bg);
        inst_line(x0, y0, 7, L("y = install now, Esc = back", "y = 今すぐインストール、Esc = 戻る"), dim, bg);
        return;
    }
    if (inst_step == INSTALL_STEP_INSTALLING) {
        inst_line(x0, y0, 2, inst_phase, fg, bg);
        uint64_t bw = win_w() - 32;
        uint64_t bx = x0;
        uint64_t by = y0 + (uint64_t)(3 * CELL_H);
        fb_fill_rect(bx, by, bw, 12, col_title_bg());
        uint64_t fill = bw * (uint64_t)inst_progress / 100;
        if (fill > 0) {
            fb_fill_rect(bx, by, fill, 12, acc);
        }
        line[0] = '\0';
        inst_u64(num, sizeof(num), (uint64_t)inst_progress);
        inst_append(line, sizeof(line), num);
        inst_append(line, sizeof(line), "% - please wait...");
        inst_line(x0, y0, 5, line, dim, bg);
        return;
    }
    if (inst_step == INSTALL_STEP_DONE) {
        inst_line(x0, y0, 2, L("Install complete.", "インストール完了"), acc, bg);
        line[0] = '\0';
        inst_u64(num, sizeof(num), (uint64_t)inst_verified);
        inst_append(line, sizeof(line), num);
        inst_append(line, sizeof(line), L(" files verified on disk.", " ファイルをディスクで検証済み"));
        inst_line(x0, y0, 3, line, fg, bg);
        if (inst_kind == INSTALL_DISK_BOOTABLE) {
            inst_line(x0, y0, 4,
                      L("Reboot to start PrismOS from the disk.", "再起動するとディスクから PrismOS が起動します"), fg, bg);
        } else {
            inst_line(x0, y0, 4,
                      L("This disk holds files only.", "このディスクはファイル専用です"), fg, bg);
            inst_line(x0, y0, 5, L("Boot from the install media.", "インストールメディアから起動してください"), fg, bg);
        }
        inst_line(x0, y0, 7, L("r = reboot now, Esc = back to Terminal", "r = 今すぐ再起動、Esc = ターミナルへ戻る"),
                  dim, bg);
        return;
    }
    /* FAILED */
    inst_line(x0, y0, 2, L("Install failed.", "インストールに失敗しました"), acc, bg);
    inst_line(x0, y0, 3, inst_error, fg, bg);
    inst_line(x0, y0, 5, L("Esc = back", "Esc = 戻る"), dim, bg);
}

/* Run the install for the chosen target and profile. Shows
 * real progress per phase, then verifies the files on disk. */
static void installer_run_install(void) {
    inst_step = INSTALL_STEP_INSTALLING;
    inst_progress = 5;
    inst_copy(inst_phase, sizeof(inst_phase), L("Preparing settings...", "設定を準備中..."));
    desktop_redraw();
    /* The chosen profile is saved into settings.txt first, so
     * the installed system boots with it. */
    desk_settings.install_profile = inst_profile;
    {
        if (save_desk_settings() != 0) {
            inst_step = INSTALL_STEP_FAILED;
            inst_copy(inst_error, sizeof(inst_error),
                      L("Could not write settings.txt.", "settings.txt を書けませんでした"));
            serial_write_line("installer: failed: settings.txt");
            desktop_redraw();
            return;
        }
    }
    inst_progress = 15;
    inst_copy(inst_phase, sizeof(inst_phase), L("Formatting disk...", "ディスクを初期化中..."));
    desktop_redraw();
    serial_write("installer: install started (profile=");
    serial_write(install_profile_name(inst_profile == 1));
    serial_write(", target=");
    serial_write(inst_target == 1   ? "data"
                 : inst_target == 2 ? "whole"
                                    : "auto");
    serial_write_line(")");
    if (inst_target == 2) {
        /* whole-disk は起動ファイルが要るが、起動ファイルは
         * インストール媒体にあり OS からは読めないため、
         * まだ作れない。データ用パーティションへの
         * インストールにすり替えることはしない (以前は
         * inst_target を無視してそのまま進んでいた)。 */
        inst_step = INSTALL_STEP_FAILED;
        inst_copy(inst_error, sizeof(inst_error),
                  L("Whole-disk install is not available yet.", "ディスク全体へのインストールはまだ使えません"));
        serial_write_line("installer: failed: whole-disk not available");
        desktop_redraw();
        return;
    }
    if (persist_install_filtered(inst_profile) != 0) {
        inst_step = INSTALL_STEP_FAILED;
        inst_copy(inst_error, sizeof(inst_error),
                  L("Could not write to disk.", "ディスクに書けませんでした"));
        serial_write_line("installer: failed: disk write");
        desktop_redraw();
        return;
    }
    inst_progress = 70;
    inst_copy(inst_phase, sizeof(inst_phase), L("Verifying files...", "ファイルを検証中..."));
    desktop_redraw();
    int verified = persist_verify_install(inst_profile);
    if (verified < 0) {
        inst_step = INSTALL_STEP_FAILED;
        inst_copy(inst_error, sizeof(inst_error),
                  L("Verification failed.", "検証に失敗しました"));
        serial_write_line("installer: failed: verify");
        desktop_redraw();
        return;
    }
    inst_verified = verified;
    inst_progress = 100;
    inst_copy(inst_phase, sizeof(inst_phase), L("Done.", "完了"));
    inst_step = INSTALL_STEP_DONE;
    serial_write("installer: done (");
    {
        char num[24];
        inst_u64(num, sizeof(num), (uint64_t)verified);
        serial_write(num);
    }
    serial_write_line(" files verified)");
    desktop_redraw();
}

/* ---- Mail ---- */

/* Rebuild the message list from the file system, ordered
 * by message number. */
static int mail_sent_index(const char *name) {
    if (!(name[0] == 's' && name[1] == 'e' && name[2] == 'n' &&
          name[3] == 't' && name[4] == '-')) {
        return -1;
    }
    int v = 0;
    int digits = 0;
    for (int k = 5; name[k] >= '0' && name[k] <= '9'; k++) {
        v = v * 10 + (name[k] - '0');
        digits++;
    }
    return digits > 0 ? v : -1;
}

static const char *mail_folder_cur(void) {
    return mail_folder[0] != '\0' ? mail_folder : "Inbox";
}

static int mail_num_of(int list_pos) {
    const char *name = vfs_file_name(mail_list_idx[list_pos]);
    if (mail_folder[0] == 'S' && mail_folder[1] == 'e' &&
        mail_folder[2] == 'n' && mail_folder[3] == 't' &&
        mail_folder[4] == '\0') {
        return mail_sent_index(name);
    }
    return mail_index_from_name(name);
}

static void mail_set_folder_of(int n, const char *folder) {
    size_t len = 0;
    const char *t = vfs_read_ptr("mail-folders.txt", &len);
    static char cur[2048];
    static char out[2048];
    if (t == NULL || len >= sizeof(cur)) {
        cur[0] = '\0';
        len = 0;
    } else {
        for (size_t i = 0; i < len; i++) {
            cur[i] = t[i];
        }
        cur[len] = '\0';
    }
    int w = mail_folder_set(cur, len, n, folder, out,
                            sizeof(out));
    if (w > 0) {
        (void)vfs_write("mail-folders.txt", out, (size_t)w);
        persist_maybe_sync();
    }
}

static void mail_folder_of_msg(int n, char *out, size_t cap) {
    size_t len = 0;
    const char *t = vfs_read_ptr("mail-folders.txt", &len);
    static char cur[2048];
    if (t == NULL || len >= sizeof(cur)) {
        inst_copy(out, cap, "Inbox");
        return;
    }
    for (size_t i = 0; i < len; i++) {
        cur[i] = t[i];
    }
    if (mail_folder_of(cur, len, n, out, cap) != 0) {
        inst_copy(out, cap, "Inbox");
    }
}

static int mail_ci_contains(const char *text,
                            const char *needle);

static void mail_rebuild(void) {
    mail_list_count = 0;
    int count = vfs_file_count();
    int sent_view = mail_folder[0] == 'S' &&
                    mail_folder[1] == 'e' &&
                    mail_folder[2] == 'n' &&
                    mail_folder[3] == 't' &&
                    mail_folder[4] == '\0';
    for (int i = 0; i < count; i++) {
        const char *nm = vfs_file_name(i);
        int n = sent_view ? mail_sent_index(nm)
                          : mail_index_from_name(nm);
        if (n < 0) {
            continue;
        }
        if (!sent_view) {
            char f[24];
            mail_folder_of_msg(n, f, sizeof(f));
            if (strcmp(f, mail_folder_cur()) != 0) {
                continue;
            }
        }
        if (mail_filter[0] != '\0') {
            /* 検索フィルタ: From / Subject に部分一致。 */
            size_t mlen = 0;
            const char *mtext = vfs_read_ptr(nm, &mlen);
            int match = 0;
            if (mtext != NULL) {
                char raw[192];
                char dec[192];
                if (mail_parse_header(mtext, "From", raw,
                                      sizeof(raw)) == 0) {
                    mime_decode_header(raw, dec, sizeof(dec));
                    if (mail_ci_contains(dec, mail_filter)) {
                        match = 1;
                    }
                }
                if (!match &&
                    mail_parse_header(mtext, "Subject", raw,
                                      sizeof(raw)) == 0) {
                    mime_decode_header(raw, dec, sizeof(dec));
                    if (mail_ci_contains(dec, mail_filter)) {
                        match = 1;
                    }
                }
            }
            if (!match) {
                continue;
            }
        }
        int j = mail_list_count;
        while (j > 0) {
            int prev = mail_num_of(j - 1);
            if (prev < n) {
                break;
            }
            mail_list_idx[j] = mail_list_idx[j - 1];
            j--;
        }
        mail_list_idx[j] = i;
        mail_list_count++;
    }
    if (mail_selected >= mail_list_count) {
        mail_selected = mail_list_count - 1;
    }
    if (mail_selected < 0) {
        mail_selected = 0;
    }
    if (mail_viewing >= mail_list_count) {
        mail_viewing = -1;
    }
}

/* 一覧の list_pos のメッセージのヘッダ (From/To/Subject)
 * を MIME デコードして取り出す。 */
static int mail_msg_field(int list_pos, const char *field,
                          char *out, size_t cap) {
    if (list_pos < 0 || list_pos >= mail_list_count) {
        return -1;
    }
    const char *name = vfs_file_name(mail_list_idx[list_pos]);
    size_t len = 0;
    const char *text = vfs_read_ptr(name, &len);
    if (text == NULL) {
        return -1;
    }
    char raw[256];
    if (mail_parse_header(text, field, raw, sizeof(raw)) != 0) {
        return -1;
    }
    mime_decode_header(raw, out, cap);
    return 0;
}

static int mail_msg_body(int list_pos, char *out, size_t cap) {
    if (list_pos < 0 || list_pos >= mail_list_count) {
        return -1;
    }
    const char *name = vfs_file_name(mail_list_idx[list_pos]);
    size_t len = 0;
    const char *text = vfs_read_ptr(name, &len);
    if (text == NULL) {
        return -1;
    }
    return mime_text_body(text, len, out, cap);
}

/* 大文字小文字を区別しない部分一致。 */
static int mail_ci_contains(const char *text, const char *needle) {
    if (needle[0] == '\0') {
        return 1;
    }
    for (size_t i = 0; text[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && text[i + j] != '\0') {
            char a = text[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            j++;
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

/* 返信 / 転送の作成画面を準備する。 */
static void mail_start_reply(int forward) {
    char from[128], subject[128];
    static char body[4096];
    if (mail_msg_field(mail_viewing, "From", from,
                       sizeof(from)) != 0) {
        from[0] = '\0';
    }
    if (mail_msg_field(mail_viewing, "Subject", subject,
                       sizeof(subject)) != 0) {
        subject[0] = '\0';
    }
    int bn = mail_msg_body(mail_viewing, body, sizeof(body) - 1);
    if (bn < 0) {
        body[0] = '\0';
        bn = 0;
    }
    body[bn] = '\0';
    mail_attach_name[0] = '\0';
    if (forward) {
        mail_to[0] = '\0';
        size_t p = 0;
        const char *pre = "Fwd: ";
        while (*pre != '\0' && p + 1 < sizeof(mail_subject)) {
            mail_subject[p++] = *pre++;
        }
        const char *sj = subject;
        while (*sj != '\0' && p + 1 < sizeof(mail_subject)) {
            mail_subject[p++] = *sj++;
        }
        mail_subject[p] = '\0';
    } else {
        /* 宛先はアドレス部分だけにする。 */
        const char *lt = NULL;
        const char *gt = NULL;
        for (const char *p = from; *p != '\0'; p++) {
            if (*p == '<') {
                lt = p;
            }
            if (*p == '>') {
                gt = p;
            }
        }
        if (lt != NULL && gt != NULL && gt > lt + 1) {
            size_t n = (size_t)(gt - lt - 1);
            if (n + 1 > sizeof(mail_to)) {
                n = sizeof(mail_to) - 1;
            }
            for (size_t i = 0; i < n; i++) {
                mail_to[i] = lt[1 + i];
            }
            mail_to[n] = '\0';
        } else {
            inst_copy(mail_to, sizeof(mail_to), from);
        }
        size_t p = 0;
        if (!(subject[0] == 'R' && subject[1] == 'e' &&
              subject[2] == ':')) {
            const char *pre = "Re: ";
            while (*pre != '\0' && p + 1 < sizeof(mail_subject)) {
                mail_subject[p++] = *pre++;
            }
        }
        const char *sj = subject;
        while (*sj != '\0' && p + 1 < sizeof(mail_subject)) {
            mail_subject[p++] = *sj++;
        }
        mail_subject[p] = '\0';
    }
    /* 本文の先頭に引用を置く。 */
    mail_body_len = 0;
    mail_body[0] = '\0';
    const char *marker = forward ? "\n\n----- Forwarded message -----\n"
                                 : "\n\n----- Original message -----\n";
    size_t p = 0;
    while (marker[p] != '\0' && p + 1 < sizeof(mail_body)) {
        mail_body[p] = marker[p];
        p++;
    }
    size_t q = 0;
    while (body[q] != '\0' && p + 1 < sizeof(mail_body)) {
        mail_body[p++] = body[q++];
    }
    mail_body[p] = '\0';
    mail_body_len = p;
    mail_viewing = -1;
    mail_composing = 1;
    mail_field = forward ? 0 : 2;
    serial_write_line(forward ? "mail: forward" : "mail: reply");
    desktop_redraw();
}

static void mail_load_config(mail_config *cfg) {
    mail_config_defaults(cfg);
    size_t len = 0;
    const char *text = vfs_read_ptr("mail.conf", &len);
    if (text != NULL) {
        mail_config_parse(cfg, text, len);
    }
    if (cfg->pass_enc[0] != '\0') {
        char plain[64];
        if (persist_unwrap_secret(cfg->pass_enc, plain,
                                  sizeof(plain)) == 0) {
            size_t i = 0;
            while (plain[i] != '\0' && i + 1 < sizeof(cfg->pass)) {
                cfg->pass[i] = plain[i];
                i++;
            }
            cfg->pass[i] = '\0';
        }
    }
}

/* ---- メールの初回設定フォーム ----
 * mail.conf が無い (または空) 状態でメールを開くと
 * このフォームに誘導する。保存形式はシェルの
 * save_mail_config と同一 (パスワードは鍵があれば
 * ラップして保存)。 */
static int mail_setup_mode;
static int mail_setup_field;
static mail_config mail_setup_cfg;
static char mail_setup_smtp_port[8];
static char mail_setup_pop3_port[8];
static char mail_setup_imap_port[8];
static char mail_setup_status[96];

static int mail_configured(void) {
    mail_config cfg;
    mail_load_config(&cfg);
    return cfg.from[0] != '\0' || cfg.smtp_host[0] != '\0' ||
           cfg.pop3_host[0] != '\0' || cfg.imap_host[0] != '\0';
}

static void mail_setup_port_str(char *out, size_t cap, uint16_t port) {
    if (port == 0) {
        out[0] = '\0';
        return;
    }
    char rev[6];
    int ri = 0;
    int v = port;
    while (v > 0 && ri < 5) {
        rev[ri++] = (char)('0' + v % 10);
        v /= 10;
    }
    size_t p = 0;
    while (ri > 0 && p + 1 < cap) {
        out[p++] = rev[--ri];
    }
    out[p] = '\0';
}

static void mail_setup_open(void) {
    mail_load_config(&mail_setup_cfg);
    mail_setup_port_str(mail_setup_smtp_port,
                        sizeof(mail_setup_smtp_port),
                        mail_setup_cfg.smtp_port);
    mail_setup_port_str(mail_setup_pop3_port,
                        sizeof(mail_setup_pop3_port),
                        mail_setup_cfg.pop3_port);
    mail_setup_port_str(mail_setup_imap_port,
                        sizeof(mail_setup_imap_port),
                        mail_setup_cfg.imap_port);
    mail_setup_field = 0;
    mail_setup_status[0] = '\0';
    mail_setup_mode = 1;
    serial_write_line("mail: setup form opened");
}

static int mail_setup_parse_port(const char *s) {
    int v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        v = v * 10 + (*p - '0');
        if (v > 65535) {
            return -1;
        }
    }
    return v;
}

static void mail_setup_save(void) {
    /* ゲストの設定変更は永続化しない (監査 L5)。鍵が無効な
     * 状態での保存は L3 の pass_enc 喪失経路でもあった。 */
    if (shell_session_is_guest()) {
        inst_copy(mail_setup_status, sizeof(mail_setup_status),
                  L("Guests cannot save mail settings.",
                    "ゲストはメール設定を保存できません"));
        desktop_redraw();
        return;
    }
    int sp = mail_setup_parse_port(mail_setup_smtp_port);
    int pp = mail_setup_parse_port(mail_setup_pop3_port);
    int ip = mail_setup_parse_port(mail_setup_imap_port);
    if (sp < 0 || pp < 0 || ip < 0) {
        inst_copy(mail_setup_status, sizeof(mail_setup_status),
                  L("Ports must be numbers (0-65535).",
                    "ポートは数字で入力してください"));
        desktop_redraw();
        return;
    }
    mail_setup_cfg.smtp_port = (uint16_t)sp;
    mail_setup_cfg.pop3_port = (uint16_t)pp;
    mail_setup_cfg.imap_port = (uint16_t)ip;
    if (mail_setup_cfg.from[0] == '\0' ||
        mail_setup_cfg.smtp_host[0] == '\0') {
        inst_copy(mail_setup_status, sizeof(mail_setup_status),
                  L("From address and SMTP server are required.",
                    "差出人アドレスと SMTP サーバーは必須です"));
        desktop_redraw();
        return;
    }
    /* シェルの save_mail_config と同じ保存作法。pass が
     * 空のときは既存の pass_enc を保持し、ラップ成功時
     * だけ差し替える (監査 L3: 以前は無条件に消していた
     * ため、鍵が無効で復号できなかった状態で保存すると
     * 保存済みパスワードが失われた)。 */
    mail_config tmp = mail_setup_cfg;
    if (tmp.pass[0] != '\0' && persist_key_active()) {
        if (persist_wrap_secret(tmp.pass, tmp.pass_enc,
                                sizeof(tmp.pass_enc)) == 0) {
            tmp.pass[0] = '\0';
        }
    }
    char buf[1024];
    size_t n = mail_config_serialize(&tmp, buf, sizeof(buf));
    if (vfs_write("mail.conf", buf, n) != 0) {
        inst_copy(mail_setup_status, sizeof(mail_setup_status),
                  L("Could not save mail.conf.", "保存できませんでした"));
        desktop_redraw();
        return;
    }
    persist_maybe_sync();
    mail_setup_mode = 0;
    serial_write_line("mail: setup saved");
    desktop_notify(L("Mail settings saved", "メール設定を保存しました"));
    mail_rebuild();
    desktop_redraw();
}

#define MAIL_SETUP_FIELDS 13 /* 0..11 項目, 12 = 保存 */

static char *mail_setup_text_field(int field, size_t *cap) {
    switch (field) {
    case 0: *cap = sizeof(mail_setup_cfg.from); return mail_setup_cfg.from;
    case 1: *cap = sizeof(mail_setup_cfg.smtp_host); return mail_setup_cfg.smtp_host;
    case 2: *cap = sizeof(mail_setup_smtp_port); return mail_setup_smtp_port;
    case 3: *cap = sizeof(mail_setup_cfg.pop3_host); return mail_setup_cfg.pop3_host;
    case 4: *cap = sizeof(mail_setup_pop3_port); return mail_setup_pop3_port;
    case 5: *cap = sizeof(mail_setup_cfg.imap_host); return mail_setup_cfg.imap_host;
    case 6: *cap = sizeof(mail_setup_imap_port); return mail_setup_imap_port;
    case 7: *cap = sizeof(mail_setup_cfg.user); return mail_setup_cfg.user;
    case 8: *cap = sizeof(mail_setup_cfg.pass); return mail_setup_cfg.pass;
    default: *cap = 0; return NULL;
    }
}

static void mail_setup_toggle_tls(void) {
    if (mail_setup_field == 9) {
        mail_setup_cfg.smtp_tls = !mail_setup_cfg.smtp_tls;
    } else if (mail_setup_field == 10) {
        mail_setup_cfg.pop3_tls = !mail_setup_cfg.pop3_tls;
    } else if (mail_setup_field == 11) {
        mail_setup_cfg.imap_tls = !mail_setup_cfg.imap_tls;
    }
}

static int mail_setup_key(int c) {
    if (c == '\t' || c == DESK_KEY_DOWN) {
        mail_setup_field = (mail_setup_field + 1) % MAIL_SETUP_FIELDS;
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_UP) {
        mail_setup_field =
            (mail_setup_field + MAIL_SETUP_FIELDS - 1) %
            MAIL_SETUP_FIELDS;
        desktop_redraw();
        return 1;
    }
    if (c == '\n' || c == '\r') {
        if (mail_setup_field == 12) {
            mail_setup_save();
        } else if (mail_setup_field >= 9) {
            mail_setup_toggle_tls();
            desktop_redraw();
        } else {
            mail_setup_field++;
            desktop_redraw();
        }
        return 1;
    }
    if (c == ' ' && mail_setup_field >= 9 && mail_setup_field <= 11) {
        mail_setup_toggle_tls();
        desktop_redraw();
        return 1;
    }
    size_t cap = 0;
    char *buf = mail_setup_text_field(mail_setup_field, &cap);
    if (buf == NULL) {
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        size_t n = 0;
        while (buf[n] != '\0') {
            n++;
        }
        if (n > 0) {
            buf[n - 1] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    if (c >= 32 && c < 127) {
        size_t n = 0;
        while (buf[n] != '\0') {
            n++;
        }
        if (n + 1 < cap) {
            buf[n] = (char)c;
            buf[n + 1] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    return 1;
}

/* POP3 fetch callback for the app: store one message as
 * the next mail-NNNN.txt file. */
static int mail_desktop_store(const char *msg, size_t len, void *ctx) {
    int *stored = (int *)ctx;
    int count = vfs_file_count();
    if (count >= PRISM_VFS_MAX_FILES) {
        serial_write_line("mail: mailbox full, message skipped.");
        return -1;
    }
    int max = 0;
    for (int i = 0; i < count; i++) {
        int n = mail_index_from_name(vfs_file_name(i));
        if (n > max) {
            max = n;
        }
    }
    if (max >= 9999) {
        serial_write_line("mail: mailbox full, message skipped.");
        return -1;
    }
    char name[16];
    if (mail_file_name(max + 1, name, sizeof(name)) != 0) {
        return -1;
    }
    if (vfs_write(name, msg, len) != 0) {
        serial_write_line("mail: could not store message.");
        return -1;
    }
    (void)mail_save_attachments(msg, len);
    (*stored)++;
    return 0;
}

static void mail_receive_from_app(void) {
    mail_config cfg;
    mail_load_config(&cfg);
    if (!mail_config_ready(&cfg)) {
        inst_copy(mail_status, sizeof(mail_status),
                  L("No mail server configured. Use `mail setup` in the Terminal.", "メールサーバが未設定です。ターミナルで `mail setup` を実行してください"));
        serial_write_line("mail: no mail server configured.");
        desktop_redraw();
        return;
    }
    int stored = 0;
    int got;
    if (cfg.imap_host[0] != '\0') {
        got = mail_imap_fetch(&cfg, mail_desktop_store, &stored);
    } else {
        got = mail_pop3_fetch(&cfg, mail_desktop_store, &stored);
    }
    if (got < 0) {
        inst_copy(mail_status, sizeof(mail_status),
                  L("Receive failed.", "受信に失敗しました"));
        serial_write_line("mail: receive failed.");
    } else if (got == 0) {
        inst_copy(mail_status, sizeof(mail_status),
                  L("No new messages.", "新しいメッセージはありません"));
    } else {
        char line[48];
        size_t p = 0;
        const char *pre = L("Received ", "受信しました: ");
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        char num[8];
        int ni = 0;
        int v = got;
        char rev[8];
        int ri = 0;
        if (v == 0) {
            num[ni++] = '0';
        } else {
            while (v > 0 && ri < 7) {
                rev[ri++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (ri > 0) {
                num[ni++] = rev[--ri];
            }
        }
        num[ni] = '\0';
        for (int i = 0; num[i] != '\0' && p + 1 < sizeof(line); i++) {
            line[p++] = num[i];
        }
        const char *post = L(" message(s).", " 件");
        while (*post != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *post++;
        }
        line[p] = '\0';
        inst_copy(mail_status, sizeof(mail_status), line);
    }
    mail_rebuild();
    desktop_redraw();
}

static void mail_send_from_app(void) {
    mail_config cfg;
    mail_load_config(&cfg);
    if (!mail_config_ready(&cfg)) {
        inst_copy(mail_status, sizeof(mail_status),
                  L("No mail server configured.", "メールサーバが未設定です"));
        serial_write_line("mail: no mail server configured.");
        desktop_redraw();
        return;
    }
    if (mail_to[0] == '\0' || mail_subject[0] == '\0') {
        inst_copy(mail_status, sizeof(mail_status),
                  L("To and Subject are required.", "宛先と件名は必須です"));
        desktop_redraw();
        return;
    }
    mail_body[mail_body_len] = '\0';
    mail_attach att;
    const mail_attach *attp = NULL;
    static uint8_t att_data[PRISM_VFS_MAX_APP_FILE_SIZE];
    if (mail_attach_name[0] != '\0') {
        size_t alen = 0;
        const char *adata = vfs_read_ptr(mail_attach_name, &alen);
        if (adata == NULL) {
            inst_copy(mail_status, sizeof(mail_status),
                      L("Attachment file not found.", "添付ファイルが見つかりません"));
            desktop_redraw();
            return;
        }
        for (size_t i = 0; i < alen; i++) {
            att_data[i] = (uint8_t)adata[i];
        }
        att.name = mail_attach_name;
        att.data = att_data;
        att.len = alen;
        attp = &att;
    }
    static char built[PRISM_VFS_MAX_APP_FILE_SIZE];
    int blen = 0;
    if (mail_smtp_send_att(&cfg, mail_to, mail_subject,
                           mail_body, attp, built,
                           sizeof(built), &blen) != 0) {
        inst_copy(mail_status, sizeof(mail_status), L("Send failed.", "送信に失敗しました"));
        serial_write_line("mail: send failed.");
    } else {
        inst_copy(mail_status, sizeof(mail_status), L("Sent.", "送信しました"));
        mail_composing = 0;
        if (blen > 0) {
            (void)mail_archive_sent(built, (size_t)blen);
            persist_maybe_sync();
        }
    }
    desktop_redraw();
}

/* Draw one field row of the compose form. The active
 * field is drawn on the accent colour. */
static void mail_draw_field(uint64_t x0, uint64_t y0, int row,
                            const char *label, const char *value,
                            int active, uint32_t fg, uint32_t bg,
                            uint32_t acc) {
    char line[128];
    size_t p = 0;
    while (*label != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *label++;
    }
    line[p++] = ' ';
    while (*value != '\0' && p + 1 < sizeof(line)) {
        line[p++] = *value++;
    }
    line[p] = '\0';
    if (active) {
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), line,
                        rgb(0xFFFFFF), acc);
    } else {
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), line,
                        fg, bg);
    }
}

static void draw_mail_setup(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    int rows = text_rows();
    draw_text_small(x0, y0,
                    L("Mail - Account setup", "メール - アカウント設定"),
                    acc, bg);
    draw_text_small(x0, y0 + (uint64_t)(1 * CELL_H),
                    L("Set up your mail account. Tab = next field.",
                      "メールアカウントを設定します。Tab = 次の欄。"),
                    dim, bg);
    static const char *const labels_en[12] = {
        "From address:", "SMTP server:", "SMTP port:",
        "POP3 server:", "POP3 port:", "IMAP server (optional):",
        "IMAP port:", "User name:", "Password:",
        "SMTP TLS:", "POP3 TLS:", "IMAP TLS:"};
    static const char *const labels_ja[12] = {
        "差出人アドレス:", "SMTP サーバー:", "SMTP ポート:",
        "POP3 サーバー:", "POP3 ポート:", "IMAP サーバー (任意):",
        "IMAP ポート:", "ユーザー名:", "パスワード:",
        "SMTP TLS:", "POP3 TLS:", "IMAP TLS:"};
    for (int f = 0; f < 12; f++) {
        int row = 3 + f;
        const char *label =
            desk_settings.language ? labels_ja[f] : labels_en[f];
        uint32_t lc = mail_setup_field == f ? acc : dim;
        draw_text_small(x0, y0 + (uint64_t)(row * CELL_H), label,
                        lc, bg);
        char val[104];
        size_t vp = 0;
        if (f >= 9) {
            int on = f == 9 ? mail_setup_cfg.smtp_tls
                     : f == 10 ? mail_setup_cfg.pop3_tls
                               : mail_setup_cfg.imap_tls;
            const char *t = on ? L("on", "オン") : L("off", "オフ");
            while (*t != '\0' && vp + 1 < sizeof(val)) {
                val[vp++] = *t++;
            }
        } else if (f == 8) {
            size_t n = 0;
            while (mail_setup_cfg.pass[n] != '\0' &&
                   vp + 1 < sizeof(val)) {
                val[vp++] = '*';
                n++;
            }
        } else {
            size_t cap = 0;
            const char *src = mail_setup_text_field(f, &cap);
            if (src != NULL) {
                while (src[vp] != '\0' && vp + 1 < sizeof(val)) {
                    val[vp] = src[vp];
                    vp++;
                }
            }
        }
        if (mail_setup_field == f && f < 9 && vp + 1 < sizeof(val)) {
            val[vp++] = '_';
        }
        val[vp] = '\0';
        draw_text_small(x0 + 300, y0 + (uint64_t)(row * CELL_H), val,
                        fg, bg);
    }
    if (mail_setup_field == 12) {
        draw_text_small(x0, y0 + (uint64_t)(16 * CELL_H),
                        L("[ Save settings ]", "[ 設定を保存 ]"),
                        rgb(0xFFFFFF), acc);
    } else {
        draw_text_small(x0, y0 + (uint64_t)(16 * CELL_H),
                        L("[ Save settings ]", "[ 設定を保存 ]"),
                        fg, bg);
    }
    if (mail_setup_status[0] != '\0') {
        draw_text_small(x0, y0 + (uint64_t)(17 * CELL_H),
                        mail_setup_status, rgb(0xC0392B), bg);
    }
    draw_text_small(x0, y0 + (uint64_t)((rows - 1) * CELL_H),
                    L("Enter on TLS rows toggles, Esc = cancel",
                      "TLS の行は Enter で切替、Esc = 中止"),
                    dim, bg);
}

static void draw_mail(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint32_t dim = col_dim();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    int rows = text_rows();
    int cols = text_cols();
    if (mail_setup_mode) {
        draw_mail_setup();
        return;
    }
    if (mail_composing) {
        draw_text_small(x0, y0, L("Mail - Compose", "メール - 作成"), acc, bg);
        mail_draw_field(x0, y0, 2, L("To:", "宛先:"), mail_to, mail_field == 0,
                        fg, bg, acc);
        mail_draw_field(x0, y0, 3, L("Subject:", "件名:"), mail_subject,
                        mail_field == 1, fg, bg, acc);
        mail_draw_field(x0, y0, 4, L("Attach:", "添付:"), mail_attach_name,
                        mail_field == 3, fg, bg, acc);
        draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H), L("Body:", "本文:"),
                        mail_field == 2 ? acc : dim, bg);
        /* Body lines, rows 6 and on. */
        int row = 6;
        size_t pos = 0;
        while (pos < mail_body_len && row < rows - 3) {
            char line[128];
            int n = 0;
            while (pos < mail_body_len && mail_body[pos] != '\n' &&
                   n < cols && n + 1 < (int)sizeof(line)) {
                line[n++] = mail_body[pos++];
            }
            if (pos < mail_body_len) {
                pos++; /* newline */
            }
            line[n] = '\0';
            fb_draw_text(x0, y0 + (uint64_t)(row * CELL_H), line, fg,
                         bg, 1);
            row++;
        }
        if (mail_field == 4) {
            draw_text_small(x0, y0 + (uint64_t)((rows - 3) * CELL_H),
                            L("[ Send ]", "[ 送信 ]"), rgb(0xFFFFFF), acc);
        } else {
            draw_text_small(x0, y0 + (uint64_t)((rows - 3) * CELL_H),
                            L("[ Send ]", "[ 送信 ]"), fg, bg);
        }
        draw_text_small(x0, y0 + (uint64_t)((rows - 1) * CELL_H),
                        L("Tab = next field, Enter = newline / send, Esc = cancel", "Tab = 次の欄、Enter = 改行/送信、Esc = 中止"),
                        dim, bg);
        return;
    }
    if (mail_viewing >= 0 && mail_viewing < mail_list_count) {
        const char *name =
            vfs_file_name(mail_list_idx[mail_viewing]);
        size_t len = 0;
        const char *raw_content = vfs_read_ptr(name, &len);
        /* Show the decoded view: RFC 2047 headers and the
         * text body (MIME-aware), not the raw source. */
        static char view_text[PRISM_VFS_MAX_APP_FILE_SIZE + 512];
        const char *content = raw_content;
        if (raw_content != NULL) {
            size_t p = 0;
            char raw[160];
            char dec[160];
            const char *fields[3] = { "From", "Subject",
                                      "Date" };
            for (int fi = 0; fi < 3; fi++) {
                if (mail_parse_header(raw_content, fields[fi],
                                      raw, sizeof(raw)) != 0) {
                    continue;
                }
                if (fi < 2) {
                    mime_decode_header(raw, dec, sizeof(dec));
                } else {
                    inst_copy(dec, sizeof(dec), raw);
                }
                if (dec[0] == '\0') {
                    continue;
                }
                const char *f = fields[fi];
                while (*f != '\0' && p + 1 < sizeof(view_text)) {
                    view_text[p++] = *f++;
                }
                if (p + 2 < sizeof(view_text)) {
                    view_text[p++] = ':';
                    view_text[p++] = ' ';
                }
                const char *d = dec;
                while (*d != '\0' && p + 1 < sizeof(view_text)) {
                    view_text[p++] = *d++;
                }
                if (p + 1 < sizeof(view_text)) {
                    view_text[p++] = '\n';
                }
            }
            if (p + 4 < sizeof(view_text)) {
                view_text[p++] = '-';
                view_text[p++] = '-';
                view_text[p++] = '-';
                view_text[p++] = '\n';
            }
            int bn = mime_text_body(raw_content, len,
                                    view_text + p,
                                    sizeof(view_text) - p - 1);
            if (bn >= 0) {
                p += (size_t)bn;
                view_text[p] = '\0';
                content = view_text;
                len = p;
            }
        }
        char header[48];
        size_t p = 0;
        const char *h = L("Viewing: ", "表示中: ");
        while (*h != '\0' && p + 1 < sizeof(header)) {
            header[p++] = *h++;
        }
        while (*name != '\0' && p + 1 < sizeof(header)) {
            header[p++] = *name++;
        }
        header[p] = '\0';
        draw_text_small(x0, y0, header, acc, bg);
        int row = 2;
        size_t pos = 0;
        int skipped = 0;
        while (pos < len && row < rows - 1) {
            char line[128];
            int n = 0;
            size_t q = pos;
            while (q < len && content[q] != '\n' &&
                   n < cols && n + 1 < (int)sizeof(line)) {
                line[n++] = content[q++];
            }
            line[n] = '\0';
            while (q < len && content[q] != '\n') {
                q++;
            }
            if (q < len) {
                q++; /* newline */
            }
            if (skipped < mail_scroll) {
                skipped++;
                pos = q;
                continue;
            }
            fb_draw_text(x0, y0 + (uint64_t)(row * CELL_H), line, fg,
                         bg, 1);
            row++;
            pos = q;
        }
        if (row >= rows - 1 && pos < len) {
            fb_draw_text(x0, y0 + (uint64_t)(row * CELL_H), "...",
                         dim, bg, 1);
        }
        draw_text_small(x0, y0 + (uint64_t)((rows - 1) * CELL_H),
                        L("Up/Down = scroll, Esc = back to list", "上下 = スクロール、Esc = 一覧へ戻る"), dim, bg);
        return;
    }
    {
        char hdr[128];
        size_t p = 0;
        const char *pre = "Mail [";
        while (*pre != '\0' && p + 1 < sizeof(hdr)) {
            hdr[p++] = *pre++;
        }
        const char *f = mail_folder_cur();
        while (*f != '\0' && p + 1 < sizeof(hdr)) {
            hdr[p++] = *f++;
        }
        const char *post =
            L("] - Up/Down, Enter read, c compose, r receive, f folder, m move, d trash/delete", "] - 上下、Enter 読む、c 作成、r 受信、f フォルダ、m 移動、d ゴミ箱/削除");
        while (*post != '\0' && p + 1 < sizeof(hdr)) {
            hdr[p++] = *post++;
        }
        hdr[p] = '\0';
        draw_text_small(x0, y0, hdr, dim, bg);
    }
    if (mail_move_mode) {
        char line[64];
        size_t p = 0;
        const char *pre = L("Move to folder: ", "フォルダへ移動: ");
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        mail_move_buf[mail_move_len] = '\0';
        const char *b = mail_move_buf;
        while (*b != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *b++;
        }
        line[p] = '\0';
        draw_text_small(x0, y0 + (uint64_t)(1 * CELL_H), line,
                        acc, bg);
    }
    if (mail_list_count == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H), L("(no mail)", "(メールなし)"),
                        fg, bg);
        mail_config cfg;
        mail_load_config(&cfg);
        if (!mail_config_ready(&cfg)) {
            draw_text_small(x0, y0 + (uint64_t)(4 * CELL_H),
                            L("No mail server configured.", "メールサーバが未設定です"),
                            fg, bg);
            draw_text_small(x0, y0 + (uint64_t)(5 * CELL_H),
                            L("Run `mail setup ...` in the Terminal.", "ターミナルで `mail setup ...` を実行してください"),
                            fg, bg);
        }
    }
    for (int i = 0; i < mail_list_count && i + 2 < rows - 1; i++) {
        const char *name = vfs_file_name(mail_list_idx[i]);
        size_t len = 0;
        const char *text = vfs_read_ptr(name, &len);
        char from[48];
        char subject[64];
        from[0] = '\0';
        subject[0] = '\0';
        if (text != NULL) {
            char raw[128];
            const char *who =
                strcmp(mail_folder_cur(), "Sent") == 0 ? "To"
                                                       : "From";
            if (mail_parse_header(text, who, raw, sizeof(raw)) ==
                0) {
                mime_decode_header(raw, from, sizeof(from));
            }
            if (mail_parse_header(text, "Subject", raw,
                                  sizeof(raw)) == 0) {
                mime_decode_header(raw, subject,
                                   sizeof(subject));
            }
        }
        char line[128];
        size_t p = 0;
        const char *f = from;
        while (*f != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *f++;
        }
        if (p + 3 < sizeof(line)) {
            line[p++] = ' ';
            line[p++] = '-';
            line[p++] = ' ';
        }
        const char *s = subject;
        while (*s != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *s++;
        }
        line[p] = '\0';
        uint64_t py = y0 + (uint64_t)((i + 2) * CELL_H);
        if (i == mail_selected) {
            draw_text_small(x0, py, line, rgb(0xFFFFFF), acc);
        } else {
            draw_text_small(x0, py, line, fg, bg);
        }
    }
    if (mail_search_mode) {
        char line[80];
        size_t p = 0;
        const char *pre = "Search: ";
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        for (size_t i = 0; i < mail_search_len &&
                          p + 1 < sizeof(line); i++) {
            line[p++] = mail_search_buf[i];
        }
        if (p + 1 < sizeof(line)) {
            line[p++] = '_';
        }
        line[p] = '\0';
        draw_text_small(x0, y0 + (uint64_t)((rows - 1) * CELL_H),
                        line, acc, bg);
    } else if (mail_filter[0] != '\0') {
        char line[80];
        size_t p = 0;
        const char *pre = L("Filter: ", "絞り込み: ");
        while (*pre != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *pre++;
        }
        const char *f = mail_filter;
        while (*f != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *f++;
        }
        const char *post = L("  (/ to change, Esc clears)", "  (/ で変更、Esc で解除)");
        while (*post != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *post++;
        }
        line[p] = '\0';
        draw_text_small(x0, y0 + (uint64_t)((rows - 1) * CELL_H),
                        line, acc, bg);
    } else if (mail_status[0] != '\0') {
        draw_text_small(x0, y0 + (uint64_t)((rows - 1) * CELL_H),
                        mail_status, acc, bg);
    }
}


/* ---------- Contacts / Editor / Paint / Store apps ---------- */

/* --- Contacts: contacts.txt の行 "名前<TAB>詳細" --- */
static int contacts_sel;
static int contacts_stage; /* 0 移動, 1 名前入力, 2 詳細入力 */
static char contacts_name[32];
static char contacts_detail[80];
static char contacts_status[80];
static int contacts_editing; /* 既存行の編集なら行番号 */

static int contacts_count(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr("contacts.txt", &len);
    if (text == NULL) {
        return 0;
    }
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            n++;
        }
    }
    return n;
}

/* idx 行を name/detail に取り出す。成功したら 0。 */
static int contacts_get(int idx, char *name, size_t ncap, char *detail,
                        size_t dcap) {
    size_t len = 0;
    const char *text = vfs_read_ptr("contacts.txt", &len);
    if (text == NULL) {
        return -1;
    }
    int cur = 0;
    size_t off = 0;
    while (off < len) {
        size_t eol = off;
        while (eol < len && text[eol] != '\n') {
            eol++;
        }
        if (cur == idx) {
            size_t tab = off;
            while (tab < eol && text[tab] != '\t') {
                tab++;
            }
            size_t nl = tab - off;
            if (nl + 1 > ncap) {
                nl = ncap - 1;
            }
            memcpy(name, text + off, nl);
            name[nl] = '\0';
            size_t dl = 0;
            if (tab < eol) {
                dl = eol - tab - 1;
                if (dl + 1 > dcap) {
                    dl = dcap - 1;
                }
                memcpy(detail, text + tab + 1, dl);
            }
            detail[dl] = '\0';
            return 0;
        }
        cur++;
        off = eol + 1;
    }
    return -1;
}

/* 全行を書き直す (skip 行を除く、extra 行を末尾に追加)。 */
static void contacts_rewrite(int skip, const char *extra) {
    static char buf[8192];
    size_t len = 0;
    const char *text = vfs_read_ptr("contacts.txt", &len);
    size_t p = 0;
    if (text != NULL) {
        int cur = 0;
        size_t off = 0;
        while (off < len) {
            size_t eol = off;
            while (eol < len && text[eol] != '\n') {
                eol++;
            }
            if (cur != skip) {
                size_t ll = eol - off;
                if (p + ll + 1 < sizeof(buf)) {
                    memcpy(buf + p, text + off, ll);
                    p += ll;
                    buf[p++] = '\n';
                }
            }
            cur++;
            off = eol + 1;
        }
    }
    if (extra != NULL) {
        size_t el = 0;
        while (extra[el] != '\0' && p + 1 < sizeof(buf)) {
            buf[p++] = extra[el++];
        }
    }
    buf[p] = '\0';
    (void)vfs_write("contacts.txt", buf, p);
    persist_maybe_sync();
}

static void draw_contacts(void) {
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    draw_text_small(x0, y0, L("Contacts - n: new, Enter: edit, d: delete, arrows move", "連絡先 - n: 新規、Enter: 編集、d: 削除、矢印で移動"),
                    acc, bg);
    int n = contacts_count();
    int rows = 20;
    for (int i = 0; i < n && i < rows; i++) {
        char name[32], detail[80], line[120];
        if (contacts_get(i, name, sizeof(name), detail,
                         sizeof(detail)) != 0) {
            continue;
        }
        size_t p = 0;
        const char *parts[3] = { name, " - ", detail };
        for (int k = 0; k < 3; k++) {
            const char *s = parts[k];
            while (*s != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *s++;
            }
        }
        line[p] = '\0';
        uint64_t py = y0 + (uint64_t)((i + 2) * CELL_H);
        if (i == contacts_sel) {
            draw_text_small(x0, py, line, rgb(0xFFFFFF), acc);
        } else {
            draw_text_small(x0, py, line, fg, bg);
        }
    }
    if (n == 0 && contacts_stage == 0) {
        draw_text_small(x0, y0 + 2 * CELL_H,
                        L("(no contacts yet - press n)", "(連絡先なし - nで追加)"), fg, bg);
    }
    char bottom[160];
    size_t p = 0;
    const char *src = contacts_status;
    if (contacts_stage == 1) {
        src = L("Name: ", "名前: ");
        while (*src != '\0' && p + 1 < sizeof(bottom)) {
            bottom[p++] = *src++;
        }
        src = contacts_name;
    } else if (contacts_stage == 2) {
        src = L("Detail: ", "詳細: ");
        while (*src != '\0' && p + 1 < sizeof(bottom)) {
            bottom[p++] = *src++;
        }
        src = contacts_detail;
    }
    if (contacts_stage != 0) {
        while (*src != '\0' && p + 1 < sizeof(bottom)) {
            bottom[p++] = *src++;
        }
        if (p + 1 < sizeof(bottom)) {
            bottom[p++] = '_';
        }
    }
    bottom[p] = '\0';
    draw_text_small(x0, y0 + (uint64_t)(rows + 2) * CELL_H, bottom,
                    acc, bg);
}

static int contacts_handle_key(int c) {
    if (contacts_stage != 0) {
        char *buf = contacts_stage == 1 ? contacts_name
                                        : contacts_detail;
        size_t cap = contacts_stage == 1 ? sizeof(contacts_name)
                                         : sizeof(contacts_detail);
        if (c == '\n' || c == '\r') {
            if (contacts_stage == 1) {
                if (contacts_name[0] == '\0') {
                    inst_copy(contacts_status,
                              sizeof(contacts_status),
                              L("Name must not be empty.", "名前は空にできません"));
                } else {
                    contacts_stage = 2;
                }
            } else {
                char line[128];
                size_t p = 0;
                const char *nm = contacts_name;
                while (*nm != '\0' && p + 1 < sizeof(line)) {
                    line[p++] = *nm++;
                }
                if (p + 1 < sizeof(line)) {
                    line[p++] = '\t';
                }
                const char *dt = contacts_detail;
                while (*dt != '\0' && p + 1 < sizeof(line)) {
                    line[p++] = *dt++;
                }
                if (p + 1 < sizeof(line)) {
                    line[p++] = '\n';
                }
                line[p] = '\0';
                contacts_rewrite(contacts_editing, line);
                contacts_stage = 0;
                inst_copy(contacts_status,
                          sizeof(contacts_status), L("Saved.", "保存しました"));
                serial_write_line("contacts: saved");
            }
            desktop_redraw();
            return 1;
        }
        if (c == 0x1B) {
            contacts_stage = 0;
            desktop_redraw();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            size_t n = 0;
            while (buf[n] != '\0') {
                n++;
            }
            if (n > 0) {
                buf[n - 1] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        if (c >= ' ' && c < 0x7F) {
            size_t n = 0;
            while (buf[n] != '\0') {
                n++;
            }
            if (n + 1 < cap) {
                buf[n] = (char)c;
                buf[n + 1] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    int n = contacts_count();
    if (c == DESK_KEY_UP) {
        if (contacts_sel > 0) {
            contacts_sel--;
        }
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_DOWN) {
        if (contacts_sel + 1 < n) {
            contacts_sel++;
        }
        desktop_redraw();
        return 1;
    }
    if (c == 'n') {
        contacts_name[0] = '\0';
        contacts_detail[0] = '\0';
        contacts_editing = -1;
        contacts_stage = 1;
        desktop_redraw();
        return 1;
    }
    if (c == '\n' || c == '\r') {
        if (n > 0 && contacts_get(contacts_sel, contacts_name,
                                  sizeof(contacts_name),
                                  contacts_detail,
                                  sizeof(contacts_detail)) == 0) {
            contacts_editing = contacts_sel;
            contacts_stage = 2;
            desktop_redraw();
        }
        return 1;
    }
    if (c == 'd' && n > 0) {
        contacts_rewrite(contacts_sel, NULL);
        if (contacts_sel > 0 && contacts_sel >= n - 1) {
            contacts_sel--;
        }
        inst_copy(contacts_status, sizeof(contacts_status),
                  L("Deleted.", "削除しました"));
        serial_write_line("contacts: deleted");
        desktop_redraw();
        return 1;
    }
    return 1;
}

/* --- Editor: 任意ファイルのテキスト編集 --- */
static char editor_name[32] = "edit.txt";
static char editor_buf[PRISM_VFS_MAX_APP_FILE_SIZE + 1];
static size_t editor_len;
static int editor_loaded;
static int editor_stage; /* 0 編集, 1 ファイル名入力 */
static char editor_namebuf[32];
static char editor_status[80];

static void editor_load(void) {
    size_t len = 0;
    const char *text = vfs_read_ptr(editor_name, &len);
    editor_len = 0;
    if (text != NULL) {
        if (len > PRISM_VFS_MAX_APP_FILE_SIZE) {
            len = PRISM_VFS_MAX_APP_FILE_SIZE;
        }
        memcpy(editor_buf, text, len);
        editor_len = len;
    }
    editor_buf[editor_len] = '\0';
    editor_loaded = 1;
}

/* Editor の取り消し (Notes と同じ考え方)。 */
#define EDITOR_UNDO_DEPTH 8
static char editor_undo_bufs[EDITOR_UNDO_DEPTH]
                            [PRISM_VFS_MAX_APP_FILE_SIZE + 1];
static size_t editor_undo_lens[EDITOR_UNDO_DEPTH];
static int editor_undo_count;
static int editor_undo_group;
static int editor_undo_run;
static char editor_redo_buf[PRISM_VFS_MAX_APP_FILE_SIZE + 1];
static size_t editor_redo_len;
static int editor_redo_valid;

static void editor_undo_push(int group) {
    if (group == editor_undo_group && editor_undo_count > 0 &&
        editor_undo_run < 24) {
        editor_undo_run++;
        return;
    }
    if (editor_undo_count > 0 &&
        editor_undo_lens[editor_undo_count - 1] == editor_len &&
        memcmp(editor_undo_bufs[editor_undo_count - 1], editor_buf,
               editor_len) == 0) {
        editor_undo_group = group;
        editor_undo_run = 0;
        return;
    }
    if (editor_undo_count == EDITOR_UNDO_DEPTH) {
        for (int i = 1; i < EDITOR_UNDO_DEPTH; i++) {
            memcpy(editor_undo_bufs[i - 1], editor_undo_bufs[i],
                   PRISM_VFS_MAX_APP_FILE_SIZE + 1);
            editor_undo_lens[i - 1] = editor_undo_lens[i];
        }
        editor_undo_count--;
    }
    memcpy(editor_undo_bufs[editor_undo_count], editor_buf,
           editor_len + 1);
    editor_undo_lens[editor_undo_count] = editor_len;
    editor_undo_count++;
    editor_undo_group = group;
    editor_undo_run = 0;
    editor_redo_valid = 0;
}

static void editor_undo_apply(void) {
    if (editor_undo_count == 0) {
        inst_copy(editor_status, sizeof(editor_status),
                  L("Nothing to undo.", "元に戻せません"));
        return;
    }
    memcpy(editor_redo_buf, editor_buf, editor_len + 1);
    editor_redo_len = editor_len;
    editor_redo_valid = 1;
    editor_undo_count--;
    editor_len = editor_undo_lens[editor_undo_count];
    memcpy(editor_buf, editor_undo_bufs[editor_undo_count],
           editor_len + 1);
    editor_undo_group = 0;
    editor_undo_run = 0;
    serial_write_line("editor: undo");
}

static void editor_redo_apply(void) {
    if (!editor_redo_valid) {
        inst_copy(editor_status, sizeof(editor_status),
                  L("Nothing to redo.", "やり直せません"));
        return;
    }
    if (editor_undo_count == EDITOR_UNDO_DEPTH) {
        for (int i = 1; i < EDITOR_UNDO_DEPTH; i++) {
            memcpy(editor_undo_bufs[i - 1], editor_undo_bufs[i],
                   PRISM_VFS_MAX_APP_FILE_SIZE + 1);
            editor_undo_lens[i - 1] = editor_undo_lens[i];
        }
        editor_undo_count--;
    }
    memcpy(editor_undo_bufs[editor_undo_count], editor_buf,
           editor_len + 1);
    editor_undo_lens[editor_undo_count] = editor_len;
    editor_undo_count++;
    memcpy(editor_buf, editor_redo_buf, editor_redo_len + 1);
    editor_len = editor_redo_len;
    editor_redo_valid = 0;
    editor_undo_group = 0;
    editor_undo_run = 0;
    serial_write_line("editor: redo");
}

static void editor_save(void) {
    if (!editor_loaded) {
        editor_load();
    }
    if (vfs_write(editor_name, editor_buf, editor_len) == 0) {
        persist_maybe_sync();
        inst_copy(editor_status, sizeof(editor_status), L("Saved.", "保存しました"));
        serial_write_line("editor: saved");
    } else {
        inst_copy(editor_status, sizeof(editor_status),
                  L("Save failed (file too large?).", "保存に失敗しました (ファイルが大きすぎ?)"));
    }
    desktop_redraw();
}

static void draw_editor(void) {
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    char head[96];
    size_t p = 0;
    const char *parts[3] = { L("Editor - ", "エディタ - "), editor_name,
                             L(" - F6 save, Ctrl+G open", " - F6 保存、Ctrl+G 開く") };
    if (editor_stage == 1) {
        parts[1] = editor_namebuf;
        parts[2] = L(" - type a file name, Enter opens", " - ファイル名を入力、Enter で開く");
    }
    for (int k = 0; k < 3; k++) {
        const char *s = parts[k];
        while (*s != '\0' && p + 1 < sizeof(head)) {
            head[p++] = *s++;
        }
    }
    head[p] = '\0';
    draw_text_small(x0, y0, head, acc, bg);
    if (!editor_loaded) {
        editor_load();
    }
    /* 本文: 折り返し表示 (最大 20 行)。 */
    int cols = 72;
    int row = 0;
    int col = 0;
    char line[80];
    size_t li = 0;
    for (size_t i = 0; i <= editor_len && row < 20; i++) {
        char c = i < editor_len ? editor_buf[i] : '\0';
        if (c == '\n' || c == '\0' || col >= cols) {
            line[li] = '\0';
            draw_text_small(x0, y0 + (uint64_t)((row + 2) * CELL_H),
                            line, fg, bg);
            row++;
            col = 0;
            li = 0;
            if (c == '\0') {
                break;
            }
            if (c == '\n') {
                continue;
            }
        }
        if (li + 1 < sizeof(line)) {
            line[li++] = c;
        }
        col++;
    }
    if (editor_status[0] != '\0') {
        draw_text_small(x0, y0 + 23 * CELL_H, editor_status, acc,
                        bg);
    }
}

/* エディタへの IME 確定文字列の挿入 (末尾へ追加)。 */
static void editor_insert_text(const char *text) {
    editor_undo_push(1);
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (editor_len + 1 < sizeof(editor_buf)) {
            editor_buf[editor_len++] = text[i];
            editor_buf[editor_len] = '\0';
        }
    }
}

static int editor_handle_key(int c) {
    if (c == 0xC9) { /* Ctrl+Z */
        editor_undo_apply();
        desktop_redraw();
        return 1;
    }
    if (c == 0xC8) { /* Ctrl+Y */
        editor_redo_apply();
        desktop_redraw();
        return 1;
    }
    if (editor_stage == 1) {
        if (c == '\n' || c == '\r') {
            if (editor_namebuf[0] != '\0') {
                inst_copy(editor_name, sizeof(editor_name),
                          editor_namebuf);
                editor_loaded = 0;
                editor_load();
                inst_copy(editor_status, sizeof(editor_status),
                          L("Opened.", "開きました"));
                serial_write_line("editor: opened file");
            }
            editor_stage = 0;
            desktop_redraw();
            return 1;
        }
        if (c == 0x1B) {
            editor_stage = 0;
            desktop_redraw();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            size_t n = 0;
            while (editor_namebuf[n] != '\0') {
                n++;
            }
            if (n > 0) {
                editor_namebuf[n - 1] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        if (c >= ' ' && c < 0x7F) {
            size_t n = 0;
            while (editor_namebuf[n] != '\0') {
                n++;
            }
            if (n + 1 < sizeof(editor_namebuf)) {
                editor_namebuf[n] = (char)c;
                editor_namebuf[n + 1] = '\0';
            }
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    if (c == 0xB6) { /* Ctrl+G: ファイルを開く */
        inst_copy(editor_namebuf, sizeof(editor_namebuf),
                  editor_name);
        editor_stage = 1;
        desktop_redraw();
        return 1;
    }
    if (ime_key_common(c, editor_insert_text)) {
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        if (editor_len > 0) {
            editor_undo_push(2);
            editor_len--;
            editor_buf[editor_len] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    if (c == '\n' || c == '\r' || c == '\t' ||
        (c >= ' ' && c < 0x7F)) {
        if (editor_len + 1 < sizeof(editor_buf)) {
            editor_undo_push(1);
            editor_buf[editor_len++] = c == '\t' ? ' ' : (char)c;
            editor_buf[editor_len] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    return 1;
}

/* --- Paint: 64x32 セルのキャンバス、BMP 保存 --- */
#define PAINT_W 64
#define PAINT_H 32
#define PAINT_CELL 8
static uint8_t paint_cells[PAINT_W * PAINT_H];
static int paint_cx, paint_cy;
static int paint_color = 1;
static char paint_status[80];

static const uint32_t PAINT_COLORS[8] = {
    0xFFFFFF, 0x202226, 0xE5484D, 0xF76B15,
    0xFFD60A, 0x46A758, 0x3E7BFA, 0x8E4EC6,
};

static void paint_save_bmp(void) {
    /* 24-bit BMP (64x32)。行は 192 バイトで 4 の倍数。 */
    static uint8_t bmp[54 + 192 * PAINT_H];
    size_t total = sizeof(bmp);
    for (size_t i = 0; i < total; i++) {
        bmp[i] = 0;
    }
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = (uint8_t)total;
    bmp[3] = (uint8_t)(total >> 8);
    bmp[4] = (uint8_t)(total >> 16);
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = PAINT_W;
    bmp[22] = PAINT_H;
    bmp[26] = 1;
    bmp[28] = 24;
    for (int y = 0; y < PAINT_H; y++) {
        int sy = PAINT_H - 1 - y; /* BMP は下から上 */
        for (int x = 0; x < PAINT_W; x++) {
            uint32_t c = PAINT_COLORS[paint_cells[sy * PAINT_W + x]
                                      & 7];
            size_t o = 54 + (size_t)y * 192 + (size_t)x * 3;
            bmp[o] = (uint8_t)c;          /* B */
            bmp[o + 1] = (uint8_t)(c >> 8);  /* G */
            bmp[o + 2] = (uint8_t)(c >> 16); /* R */
        }
    }
    if (vfs_write("paint.bmp", (const char *)bmp, total) == 0) {
        persist_maybe_sync();
        inst_copy(paint_status, sizeof(paint_status),
                  L("Saved to paint.bmp (open it in Photos).", "paint.bmp に保存しました (写真で開けます)"));
        serial_write_line("paint: saved paint.bmp");
    } else {
        inst_copy(paint_status, sizeof(paint_status),
                  L("Save failed.", "保存に失敗しました"));
    }
}

static void draw_paint(void) {
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    draw_text_small(x0, y0, L("Paint - arrows move, Space paints, 1-7 color, c clear, F6 save BMP", "ペイント - 矢印で移動、Space で塗る、1-7 色、c 消去、F6 BMP保存"),
                    acc, bg);
    uint64_t cx0 = x0;
    uint64_t cy0 = y0 + 2 * CELL_H;
    for (int y = 0; y < PAINT_H; y++) {
        for (int x = 0; x < PAINT_W; x++) {
            uint8_t v = paint_cells[y * PAINT_W + x];
            if (v != 0) {
                fb_fill_rect(cx0 + (uint64_t)x * PAINT_CELL,
                             cy0 + (uint64_t)y * PAINT_CELL,
                             PAINT_CELL, PAINT_CELL,
                             rgb(PAINT_COLORS[v & 7]));
            }
        }
    }
    /* カーソル枠と現在の色。 */
    fb_fill_rect(cx0 + (uint64_t)paint_cx * PAINT_CELL,
                 cy0 + (uint64_t)paint_cy * PAINT_CELL, PAINT_CELL,
                 1, acc);
    fb_fill_rect(cx0 + (uint64_t)paint_cx * PAINT_CELL,
                 cy0 + (uint64_t)(paint_cy + 1) * PAINT_CELL - 1,
                 PAINT_CELL, 1, acc);
    fb_fill_rect(cx0 + PAINT_W * PAINT_CELL + 16,
                 cy0, 24, 24, rgb(PAINT_COLORS[paint_color & 7]));
    if (paint_status[0] != '\0') {
        draw_text_small(x0, cy0 + PAINT_H * PAINT_CELL + 8,
                        paint_status, acc, bg);
    }
}

static int paint_handle_key(int c) {
    if (c == DESK_KEY_LEFT) {
        if (paint_cx > 0) {
            paint_cx--;
        }
    } else if (c == DESK_KEY_RIGHT) {
        if (paint_cx + 1 < PAINT_W) {
            paint_cx++;
        }
    } else if (c == DESK_KEY_UP) {
        if (paint_cy > 0) {
            paint_cy--;
        }
    } else if (c == DESK_KEY_DOWN) {
        if (paint_cy + 1 < PAINT_H) {
            paint_cy++;
        }
    } else if (c == ' ') {
        paint_cells[paint_cy * PAINT_W + paint_cx] =
            (uint8_t)paint_color;
        serial_write_line("paint: cell painted");
    } else if (c >= '1' && c <= '7') {
        paint_color = c - '0';
    } else if (c == 'c') {
        for (int i = 0; i < PAINT_W * PAINT_H; i++) {
            paint_cells[i] = 0;
        }
        inst_copy(paint_status, sizeof(paint_status), L("Cleared.", "消去しました"));
    } else {
        return 1;
    }
    desktop_redraw();
    return 1;
}

/* --- Store: カタログ表示と導入/削除 --- */
#define STORE_UI_MAX 16
static char store_ui_names[STORE_UI_MAX][32];
static char store_ui_vers[STORE_UI_MAX][16];
static char store_ui_paths[STORE_UI_MAX][64];
static int store_ui_count;
static int store_ui_sel;
static char store_ui_status[96];
/* 端末内アプリの管理ビュー (項目17): 標準アプリの
 * 取り外しと再インストールを Store から行う。 */
static int store_apps_view;
static int store_app_sel;
static uint8_t store_ui_pkg[20480];

static int store_ui_installed(const char *name) {
    size_t len = 0;
    const char *text = vfs_read_ptr("store-installed.txt", &len);
    if (text == NULL) {
        return 0;
    }
    static char buf[2048];
    if (len + 1 > sizeof(buf)) {
        return 0;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    return store_installed_contains(buf, len, name);
}

static void store_ui_refresh(void) {
    store_ui_count = 0;
    char host[64], base[64];
    int port = 0;
    if (prism_store_server(host, sizeof(host), &port, base,
                     sizeof(base)) != 0) {
        inst_copy(store_ui_status, sizeof(store_ui_status),
                  L("No store server configured (press o to set it here).", "ストアサーバが未設定です (o キーでここで設定できます)"));
        return;
    }
    static char cat[4096];
    char url[160];
    size_t up = 0;
    while (base[up] != '\0' && up + 1 < sizeof(url)) {
        url[up] = base[up];
        up++;
    }
    if (up > 0 && url[up - 1] != '/' && up + 1 < sizeof(url)) {
        url[up++] = '/';
    }
    const char *cf = "catalog.txt";
    size_t cq = 0;
    while (cf[cq] != '\0' && up + 1 < sizeof(url)) {
        url[up++] = cf[cq++];
    }
    url[up] = '\0';
    size_t clen = 0;
    if (net_fetch_http_px(host, port, url, cat, sizeof(cat),
                          &clen, NULL) != 0) {
        inst_copy(store_ui_status, sizeof(store_ui_status),
                  L("Catalog fetch failed.", "カタログの取得に失敗しました"));
        return;
    }
    cat[clen] = '\0';
    size_t off = 0;
    while (off < clen && store_ui_count < STORE_UI_MAX) {
        size_t eol = off;
        while (eol < clen && cat[eol] != '\n') {
            eol++;
        }
        /* 単語 3 つ: name version path */
        char *dst[3] = { store_ui_names[store_ui_count],
                         store_ui_vers[store_ui_count],
                         store_ui_paths[store_ui_count] };
        size_t caps[3] = { 32, 16, 64 };
        int w = 0;
        size_t i = off;
        while (i < eol && w < 3) {
            while (i < eol && (cat[i] == ' ' || cat[i] == '\r')) {
                i++;
            }
            size_t d = 0;
            while (i < eol && cat[i] != ' ' && cat[i] != '\r') {
                if (d + 1 < caps[w]) {
                    dst[w][d++] = cat[i];
                }
                i++;
            }
            dst[w][d] = '\0';
            w++;
        }
        if (w == 3 && dst[0][0] != '\0') {
            store_ui_count++;
        }
        off = eol + 1;
    }
    inst_copy(store_ui_status, sizeof(store_ui_status),
              L("Catalog loaded. i: install, u: uninstall, r: refresh.", "カタログ読込済み。i: インストール、u: 削除、r: 更新"));
    serial_write_line("store-ui: catalog loaded");
}

static void store_ui_install(void) {
    if (store_ui_sel >= store_ui_count) {
        return;
    }
    char host[64], base[64];
    int port = 0;
    if (prism_store_server(host, sizeof(host), &port, base,
                     sizeof(base)) != 0) {
        return;
    }
    const char *path = store_ui_paths[store_ui_sel];
    char url[192];
    size_t p = 0;
    if (path[0] != '/') {
        while (base[p] != '\0' && p + 1 < sizeof(url)) {
            url[p] = base[p];
            p++;
        }
        if (p > 0 && url[p - 1] != '/' && p + 1 < sizeof(url)) {
            url[p++] = '/';
        }
    }
    size_t q = 0;
    while (path[q] != '\0' && p + 1 < sizeof(url)) {
        url[p++] = path[q++];
    }
    url[p] = '\0';
    size_t pklen = 0;
    if (net_fetch_http_px(host, port, url,
                          (char *)store_ui_pkg,
                          sizeof(store_ui_pkg), &pklen,
                          NULL) != 0) {
        inst_copy(store_ui_status, sizeof(store_ui_status),
                  L("Package fetch failed.", "パッケージの取得に失敗しました"));
        return;
    }
    char err[64];
    if (prism_store_install_mem(store_ui_pkg, pklen, err, sizeof(err)) !=
        0) {
        size_t ep = 0;
        const char *pre = L("Install failed: ", "インストール失敗: ");
        while (*pre != '\0' && ep + 1 < sizeof(store_ui_status)) {
            store_ui_status[ep++] = *pre++;
        }
        const char *es = err;
        while (*es != '\0' && ep + 1 < sizeof(store_ui_status)) {
            store_ui_status[ep++] = *es++;
        }
        store_ui_status[ep] = '\0';
        return;
    }
    inst_copy(store_ui_status, sizeof(store_ui_status),
              L("Installed.", "インストールしました"));
}

/* ---- ストアのサーバー設定フォーム ----
 * 保存先はシェルの `store server` と同じ store.conf
 * ("<host> <port> <base>" の1行)。 */
static int store_srv_mode;
static int store_srv_field; /* 0=host, 1=port, 2=base, 3=保存 */
static char store_srv_host[64];
static char store_srv_port[8];
static char store_srv_base[64];

static void store_srv_open(void) {
    int port = 0;
    if (prism_store_server(store_srv_host, sizeof(store_srv_host),
                           &port, store_srv_base,
                           sizeof(store_srv_base)) == 0) {
        mail_setup_port_str(store_srv_port, sizeof(store_srv_port),
                            (uint16_t)port);
    } else {
        store_srv_host[0] = '\0';
        store_srv_port[0] = '\0';
        store_srv_base[0] = '\0';
    }
    store_srv_field = 0;
    store_srv_mode = 1;
    serial_write_line("store-ui: server form opened");
}

static void store_srv_save(void) {
    /* 取得先の付け替えになるため、サーバー設定の保存は
     * 管理者ロール必須 (監査 M2)。 */
    if (!shell_current_user_is_admin()) {
        inst_copy(store_ui_status, sizeof(store_ui_status),
                  L("Admin only.", "管理者のみ変更できます"));
        desktop_redraw();
        return;
    }
    if (store_srv_host[0] == '\0' ||
        mail_setup_parse_port(store_srv_port) < 0) {
        inst_copy(store_ui_status, sizeof(store_ui_status),
                  L("Host is required and port must be a number.",
                    "ホスト必須、ポートは数字で入力してください"));
        desktop_redraw();
        return;
    }
    char conf[160];
    size_t q = 0;
    const char *parts[5] = {store_srv_host, " ", store_srv_port,
                            " ",
                            store_srv_base[0] != '\0'
                                ? store_srv_base
                                : "/"};
    for (int k = 0; k < 5; k++) {
        const char *s = parts[k];
        while (*s != '\0' && q + 1 < sizeof(conf)) {
            conf[q++] = *s++;
        }
    }
    conf[q] = '\0';
    if (vfs_write("store.conf", conf, q) == 0) {
        persist_maybe_sync();
        serial_write_line("store-ui: server configured");
        inst_copy(store_ui_status, sizeof(store_ui_status),
                  L("Store server saved.", "ストアサーバーを保存しました"));
        store_srv_mode = 0;
    } else {
        inst_copy(store_ui_status, sizeof(store_ui_status),
                  L("Could not save store.conf.", "保存できませんでした"));
    }
    desktop_redraw();
}

static int store_srv_key(int c) {
    if (c == '\t') {
        store_srv_field = (store_srv_field + 1) % 4;
        desktop_redraw();
        return 1;
    }
    if (c == '\n' || c == '\r') {
        if (store_srv_field == 3) {
            store_srv_save();
        } else {
            store_srv_field++;
            desktop_redraw();
        }
        return 1;
    }
    char *buf = NULL;
    size_t cap = 0;
    if (store_srv_field == 0) {
        buf = store_srv_host;
        cap = sizeof(store_srv_host);
    } else if (store_srv_field == 1) {
        buf = store_srv_port;
        cap = sizeof(store_srv_port);
    } else if (store_srv_field == 2) {
        buf = store_srv_base;
        cap = sizeof(store_srv_base);
    }
    if (buf == NULL) {
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        size_t n = 0;
        while (buf[n] != '\0') {
            n++;
        }
        if (n > 0) {
            buf[n - 1] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    if (c >= 32 && c < 127) {
        size_t n = 0;
        while (buf[n] != '\0') {
            n++;
        }
        if (n + 1 < cap) {
            buf[n] = (char)c;
            buf[n + 1] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    return 1;
}

static void draw_store(void) {
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    draw_text_small(x0, y0, L("Prism Store - signed apps only", "Prism Store - 署名済みアプリのみ"), acc,
                    bg);
    if (store_srv_mode) {
        static const char *const labels_en[3] = {
            "Server host:", "Port:", "Base path:"};
        static const char *const labels_ja[3] = {
            "サーバーホスト:", "ポート:", "ベースパス:"};
        const char *bufs[3] = {store_srv_host, store_srv_port,
                               store_srv_base};
        for (int f = 0; f < 3; f++) {
            draw_text_small(x0, y0 + (uint64_t)((2 + f) * CELL_H),
                            desk_settings.language ? labels_ja[f]
                                                   : labels_en[f],
                            store_srv_field == f ? acc : fg, bg);
            char val[80];
            size_t vp = 0;
            while (bufs[f][vp] != '\0' && vp + 1 < sizeof(val)) {
                val[vp] = bufs[f][vp];
                vp++;
            }
            if (store_srv_field == f && vp + 1 < sizeof(val)) {
                val[vp++] = '_';
            }
            val[vp] = '\0';
            draw_text_small(x0 + 240,
                            y0 + (uint64_t)((2 + f) * CELL_H), val,
                            fg, bg);
        }
        if (store_srv_field == 3) {
            draw_text_small(x0, y0 + (uint64_t)(6 * CELL_H),
                            L("[ Save ]", "[ 保存 ]"), rgb(0xFFFFFF),
                            acc);
        } else {
            draw_text_small(x0, y0 + (uint64_t)(6 * CELL_H),
                            L("[ Save ]", "[ 保存 ]"), fg, bg);
        }
        draw_text_small(x0, y0 + (uint64_t)(8 * CELL_H),
                        L("Tab = next field, Esc = cancel",
                          "Tab = 次の欄、Esc = 中止"),
                        col_dim(), bg);
        if (store_ui_status[0] != '\0') {
            draw_text_small(x0, y0 + 22 * CELL_H, store_ui_status,
                            acc, bg);
        }
        return;
    }
    if (store_apps_view) {
        /* 端末内アプリの一覧 (標準アプリの管理)。 */
        for (int i = 0; i < DESK_APP_COUNT; i++) {
            desktop_mode m = desk_standard_apps[i];
            char line[128];
            size_t p = 0;
            const char *nm = mode_name(m);
            while (*nm != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *nm++;
            }
            const char *st = i < 3
                ? L("  [system]", "  [システム]")
                : (app_is_hidden(m)
                       ? L("  [removed]", "  [取り外し済み]")
                       : L("  [installed]", "  [導入済み]"));
            while (*st != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *st++;
            }
            line[p] = '\0';
            uint64_t py = y0 + (uint64_t)((i + 2) * CELL_H);
            if (i == store_app_sel) {
                draw_text_small(x0, py, line, rgb(0xFFFFFF), acc);
            } else {
                draw_text_small(x0, py, line, fg, bg);
            }
        }
        draw_text_small(
            x0, y0 + (uint64_t)((DESK_APP_COUNT + 3) * CELL_H),
            L("Enter = install / remove  A = store catalog",
              "Enter = 導入 / 取り外し  A = ストアカタログ"),
            col_dim(), bg);
        if (store_ui_status[0] != '\0') {
            draw_text_small(x0, y0 + 22 * CELL_H, store_ui_status,
                            acc, bg);
        }
        return;
    }
    for (int i = 0; i < store_ui_count; i++) {
        char line[128];
        size_t p = 0;
        const char *parts[4] = { store_ui_names[i], " ",
                                 store_ui_vers[i],
                                 store_ui_installed(
                                     store_ui_names[i])
                                     ? L("  [installed]", "  [導入済み]")
                                     : "" };
        for (int k = 0; k < 4; k++) {
            const char *s = parts[k];
            while (*s != '\0' && p + 1 < sizeof(line)) {
                line[p++] = *s++;
            }
        }
        line[p] = '\0';
        uint64_t py = y0 + (uint64_t)((i + 2) * CELL_H);
        if (i == store_ui_sel) {
            draw_text_small(x0, py, line, rgb(0xFFFFFF), acc);
        } else {
            draw_text_small(x0, py, line, fg, bg);
        }
    }
    if (store_ui_count == 0) {
        draw_text_small(x0, y0 + 2 * CELL_H,
                        L("(no catalog - press r to fetch)", "(カタログなし - rで取得)"), fg, bg);
    }
    if (store_ui_status[0] != '\0') {
        draw_text_small(x0, y0 + 22 * CELL_H, store_ui_status, acc,
                        bg);
    }
}

static int store_handle_key(int c) {
    /* シェルの store_flow と同じ入口ゲート: ゲストはストア
     * 自体を使えない (監査 M2)。カタログ取得・導入・削除・
     * サーバー設定・標準アプリの着脱はすべてこの関数を
     * 通るため、ここで一括して拒否する。 */
    if (shell_session_is_guest()) {
        inst_copy(store_ui_status, sizeof(store_ui_status),
                  L("Guests cannot use the store.",
                    "ゲストはストアを利用できません"));
        serial_write_line("store-ui: guest refused");
        desktop_redraw();
        return 1;
    }
    if (store_srv_mode) {
        return store_srv_key(c);
    }
    if (c == 'a' || c == 'A') {
        store_apps_view = !store_apps_view;
        serial_write_line(store_apps_view ? "store-ui: device apps"
                                          : "store-ui: catalog");
        desktop_redraw();
        return 1;
    }
    if (store_apps_view) {
        if (c == DESK_KEY_UP) {
            if (store_app_sel > 0) {
                store_app_sel--;
            }
        } else if (c == DESK_KEY_DOWN) {
            if (store_app_sel + 1 < DESK_APP_COUNT) {
                store_app_sel++;
            }
        } else if (c == '\n' || c == '\r') {
            desktop_mode m = desk_standard_apps[store_app_sel];
            if (app_is_hidden(m)) {
                if (app_set_hidden(m, 0) == 0) {
                    inst_copy(store_ui_status,
                              sizeof(store_ui_status),
                              L("Installed.", "導入しました"));
                }
            } else if (app_set_hidden(m, 1) == 0) {
                inst_copy(store_ui_status, sizeof(store_ui_status),
                          L("Removed.", "取り外しました"));
                if (desk_mode == m) {
                    switch_mode(DESK_TERMINAL);
                }
            } else {
                inst_copy(store_ui_status, sizeof(store_ui_status),
                          L("This app cannot be removed.",
                            "このアプリは外せません"));
            }
        }
        desktop_redraw();
        return 1;
    }
    if (c == 'o') {
        store_srv_open();
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_UP) {
        if (store_ui_sel > 0) {
            store_ui_sel--;
        }
        desktop_redraw();
        return 1;
    }
    if (c == DESK_KEY_DOWN) {
        if (store_ui_sel + 1 < store_ui_count) {
            store_ui_sel++;
        }
        desktop_redraw();
        return 1;
    }
    if (c == 'r') {
        store_ui_refresh();
        desktop_redraw();
        return 1;
    }
    if (c == 'i') {
        store_ui_install();
        desktop_redraw();
        return 1;
    }
    if (c == 'u' && store_ui_sel < store_ui_count) {
        if (prism_store_uninstall(store_ui_names[store_ui_sel]) == 0) {
            inst_copy(store_ui_status, sizeof(store_ui_status),
                      L("Uninstalled.", "アンインストールしました"));
            serial_write_line("store-ui: uninstalled");
        } else {
            inst_copy(store_ui_status, sizeof(store_ui_status),
                      L("Not installed.", "未インストールです"));
        }
        desktop_redraw();
        return 1;
    }
    return 1;
}

static void draw_window(void) {
    uint64_t x = win_x();
    uint64_t y = win_y();
    uint64_t w = win_w();
    uint64_t h = win_h();
    uint32_t bg = col_window_bg();
    uint32_t title_bg = col_title_bg();
    uint32_t fg = col_text();
    fb_fill_rect(x, y, w, h, bg);
    fb_fill_rect(x, y, w, TITLE_H, title_bg);
    /* Accent underline under the title bar. */
    fb_fill_rect(x, y + TITLE_H, w, 2, col_accent());
    draw_text_small(x + 8, y + 8, mode_name(desk_mode), fg, title_bg);
    if (desk_mode == DESK_TERMINAL) {
        draw_console_text();
    } else if (desk_mode == DESK_FILES) {
        draw_files();
    } else if (desk_mode == DESK_BROWSER) {
        draw_browser();
    } else if (desk_mode == DESK_NOTES) {
        draw_notes();
    } else if (desk_mode == DESK_CALENDAR) {
        draw_calendar();
    } else if (desk_mode == DESK_PHOTOS) {
        draw_photos();
    } else if (desk_mode == DESK_MUSIC) {
        draw_music();
    } else if (desk_mode == DESK_CARDS) {
        draw_cards();
    } else if (desk_mode == DESK_SHOOTER) {
        draw_shooter();
    } else if (desk_mode == DESK_SHEET) {
        draw_sheet();
    } else if (desk_mode == DESK_VIDEO) {
        draw_video();
    } else if (desk_mode == DESK_INSTALLER) {
        draw_installer();
    } else if (desk_mode == DESK_MAIL) {
        draw_mail();
    } else if (desk_mode == DESK_DOCS) {
        draw_docs();
    } else if (desk_mode == DESK_SLIDES) {
        draw_slides();
    } else if (desk_mode == DESK_CONTACTS) {
        draw_contacts();
    } else if (desk_mode == DESK_EDITOR) {
        draw_editor();
    } else if (desk_mode == DESK_PAINT) {
        draw_paint();
    } else if (desk_mode == DESK_STORE) {
        draw_store();
    } else if (desk_mode == DESK_NOTIF) {
        draw_notiflog();
    } else {
        draw_settings();
    }
    if (search_active) {
        draw_search();
    }
    /* IME の変換中文字列を、対応アプリの下端に重ねて
     * 表示する (Notes は自前フッターに表示するので除く)。 */
    if (ime_on() && ime_active() &&
        (search_active || desk_mode == DESK_EDITOR ||
         desk_mode == DESK_DOCS || desk_mode == DESK_MAIL)) {
        char bar[128];
        size_t bp = 0;
        const char *tag = "[IME] ";
        while (*tag != '\0' && bp + 1 < sizeof(bar)) {
            bar[bp++] = *tag++;
        }
        const char *d = ime_display();
        while (*d != '\0' && bp + 1 < sizeof(bar)) {
            bar[bp++] = *d++;
        }
        if (ime_candidate_count() > 0) {
            bar[bp++] = ' ';
            bar[bp++] = '(';
            bar[bp++] = (char)('0' + ime_candidate_index() + 1);
            bar[bp++] = '/';
            bar[bp++] = (char)('0' + ime_candidate_count());
            bar[bp++] = ')';
        }
        bar[bp] = '\0';
        draw_text_small(text_x(),
                        text_y() + (uint64_t)((text_rows() - 1) *
                                              CELL_H),
                        bar, rgb(0xFFFFFF), col_accent());
    }
}

/* ---------- app icons (16x16 pixel maps) ----------
 * One map per desktop mode, plus a generic window icon
 * at index DESK_APP_COUNT. Characters are palette keys
 * (see icon_color); '.' leaves the background. The maps
 * are drawn by draw_app_icon() in the Dock, the
 * launcher and the taskbar, so every app is recognisable
 * by its picture, not just its name. */
static const char *const APP_ICONS[DESK_APP_COUNT + 1][16] = {
    /* DESK_TERMINAL: dark screen, green prompt */
    {
        "................",
        ".KKKKKKKKKKKKKK.",
        ".KWWWWWWWWWWWWK.",
        ".KWKKKKKKKKKKWK.",
        ".KWKGKKKKKKKKWK.",
        ".KWKKGGKKKKKKWK.",
        ".KWKGKKKKKKKKWK.",
        ".KWKKKKKKKKKKWK.",
        ".KWKKKKKKKKKKWK.",
        ".KWKKKKKKGGGGWK.",
        ".KWKKKKKKKKKKWK.",
        ".KWKKKKKKKKKKWK.",
        ".KWWWWWWWWWWWWK.",
        ".KKKKKKKKKKKKKK.",
        "................",
        "................",
    },
    /* DESK_FILES: yellow folder */
    {
        "................",
        "................",
        "..YYYY..........",
        ".YYYYYYYY.......",
        ".Yyyyyyyyyyyyy..",
        ".YyYYYYYYYYYYy..",
        ".YyYyyyyyyyyYy..",
        ".YyYyyyyyyyyYy..",
        ".YyYyyyyyyyyYy..",
        ".YyYyyyyyyyyYy..",
        ".Yyyyyyyyyyyyy..",
        "..yyyyyyyyyyyy..",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_SETTINGS: gear */
    {
        ".......dd.......",
        "......dWWd......",
        "...d...dd...d...",
        "....d..dd..d....",
        ".....dWWWWd.....",
        "..d.WWWKKWWd.d..",
        "...dWWKKKKWWd...",
        "....WWKKKKWW....",
        "...dWWKKKKWWd...",
        "..d.WWWKKWWd.d..",
        ".....dWWWWd.....",
        "....d..dd..d....",
        "...d...dd...d...",
        "......dWWd......",
        ".......dd.......",
        "................",
    },
    /* DESK_BROWSER: globe */
    {
        ".....BBBBBB.....",
        "...BBbbbbbbBB...",
        "..BbbBbbBbbBbb..",
        ".BbBbbBbbBbbBbb.",
        ".bBbbBbbBbbBbbb.",
        "BbbBbbbbbbbbBBbB",
        "BbBbbBbbBbbBbbBb",
        "BBBBBBBBBBBBBBBB",
        "BbBbbBbbBbbBbbBb",
        "BbbBbbbbbbbbBBbB",
        ".bBbbBbbBbbBbbb.",
        ".BbBbbBbbBbbBbb.",
        "..BbbBbbBbbBbb..",
        "...BBbbbbbbBB...",
        ".....BBBBBB.....",
        "................",
    },
    /* DESK_NOTES: page with lines and a pencil */
    {
        "..eeeeeeee......",
        ".eeeeeeeeee..OO.",
        ".eeBBeeeeeee.OO.",
        ".eeeeeeeeee.OO..",
        ".eeBBBBBeeeOO...",
        ".eeeeeeeeeOO....",
        ".eeBBBBBBeO.....",
        ".eeeeeeeeOO.....",
        ".eeBBBBBeO......",
        ".eeeeeeeO.......",
        ".eeBBBBe........",
        ".eeeeee.........",
        ".eeeee..........",
        "..eeee..........",
        "................",
        "................",
    },
    /* DESK_CALENDAR: wall calendar */
    {
        "................",
        "..RRRRRRRRRRRR..",
        ".RWWWWWWWWWWWWR.",
        ".RWdddddddddddR.",
        ".RWWWWWWWWWWWWR.",
        ".RWddWddWddWddR.",
        ".RWddWddWddWddR.",
        ".RWddWddWddWddR.",
        ".RWddWddWRRWddR.",
        ".RWddWddWRRWddR.",
        ".RWddWddWddWddR.",
        ".RWWWWWWWWWWWWR.",
        "..RRRRRRRRRRRR..",
        "................",
        "................",
        "................",
    },
    /* DESK_PHOTOS: landscape picture */
    {
        ".KKKKKKKKKKKKKK.",
        ".KbbbbbbbbbbbbK.",
        ".KbbbYYYYbbbbK..",
        ".KbbYYYYYYbbbK..",
        ".KbbbYYYYbbbbK..",
        ".KbbbbbbbbbbbK..",
        ".KbbGGbbbbGGbK..",
        ".KbGGGGbbGGGGK..",
        ".KGGGGGGGGGGGK..",
        ".KGGGGGGGGGGGK..",
        ".KKKKKKKKKKKKKK.",
        "................",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_MUSIC: note */
    {
        ".......PP.......",
        "......PPPP......",
        "......PP.PP.....",
        "......PP..PP....",
        "......PP...PP...",
        "......PP....P...",
        "......PP........",
        "......PP........",
        "......PP........",
        ".....PPP........",
        "....PPPPP.......",
        "...PPPPPPP......",
        "...PPPPPPP......",
        "....PPPPP.......",
        ".....PPP........",
        "................",
    },
    /* DESK_CARDS: two cards, heart on top */
    {
        "....WWWWWW......",
        "...WRRRRRWWWWW..",
        "..WRWWRRWRWWWW..",
        "..WRRRRRRWKWKW..",
        "..WRRRRWRWKWKW..",
        "..WWRRWWWWKWKW..",
        "...WWWWWWKWKW...",
        "....WWWWKWKW....",
        "........KWKW....",
        "........WWWW....",
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_SHOOTER: crosshair */
    {
        ".......RR.......",
        ".......RR.......",
        ".......RR.......",
        "................",
        ".......RR.......",
        "...RRRRRRRRRR...",
        "..RRRRRRRRRRRR..",
        "RRRRR..RR..RRRRR",
        "RRRRR..RR..RRRRR",
        "..RRRRRRRRRRRR..",
        "...RRRRRRRRRR...",
        ".......RR.......",
        "................",
        ".......RR.......",
        ".......RR.......",
        ".......RR.......",
    },
    /* DESK_SHEET: green table */
    {
        ".GGGGGGGGGGGGGG.",
        ".GWWWGWWWGWWWG..",
        ".GGGGGGGGGGGGG..",
        ".GWWWGWWWGWWWG..",
        ".GGGGGGGGGGGGG..",
        ".GWWWGWWWGWWWG..",
        ".GGGGGGGGGGGGG..",
        ".GWWWGWWWGWWWG..",
        ".GGGGGGGGGGGGG..",
        ".GWWWGWWWGWWWG..",
        ".GGGGGGGGGGGGG..",
        "................",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_VIDEO: film with play button */
    {
        ".KKKKKKKKKKKKKK.",
        ".KWK KWK KWK KK.",
        ".KKKKKKKKKKKKKK.",
        ".KKKKKWWKKKKKKK.",
        ".KKKKKWWWKKKKKK.",
        ".KKKKKWWWWKKKKK.",
        ".KKKKKWWWKKKKKK.",
        ".KKKKKWWKKKKKKK.",
        ".KKKKKKKKKKKKKK.",
        ".KWK KWK KWK KK.",
        ".KKKKKKKKKKKKKK.",
        "................",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_INSTALLER: download arrow into a tray */
    {
        ".......BB.......",
        ".......BB.......",
        ".......BB.......",
        ".......BB.......",
        ".......BB.......",
        "....B.BBBB.B....",
        ".....BBBBBB.....",
        "......BBBB......",
        ".......BB.......",
        "................",
        "..KKKKKKKKKKKK..",
        "..K..........K..",
        "..KKKKKKKKKKKK..",
        "................",
        "................",
        "................",
    },
    /* DESK_MAIL: envelope */
    {
        "................",
        ".WWWWWWWWWWWWWW.",
        ".WKKKKKKKKKKKKW.",
        ".WWKKKKKKKKKKWW.",
        ".KWWKKKKKKKKWWK.",
        ".KKWWKKKKKKWWKK.",
        ".KKKWWKKKKWWKKK.",
        ".KKKKWWKKWWKKKK.",
        ".KKKKKWWWWKKKKK.",
        ".KKKKKKWWKKKKKK.",
        ".KKKKKKKKKKKKKK.",
        ".WWWWWWWWWWWWWW.",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_DOCS: blue document */
    {
        "...BBBBBBBB.....",
        "..BeeeeeeeeB....",
        "..BeBBBBBBeeB...",
        "..BeeeeeeeeeB...",
        "..BeBBBBBBBeeB..",
        "..BeeeeeeeeeB...",
        "..BeBBBBBBBeeB..",
        "..BeeeeeeeeeB...",
        "..BeBBBBBeeeB...",
        "..BeeeeeeeeeB...",
        "..BeeeeeeeeeB...",
        "...BBBBBBBBBB...",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_SLIDES: presentation chart */
    {
        ".OOOOOOOOOOOOOO.",
        ".OeeeeeeeeeeeeO.",
        ".OeOeeeeeeeeeeO.",
        ".OeOOeeeeeeeeeO.",
        ".OeOOOeeeeeeeeO.",
        ".OeOOOOeeeeeeeO.",
        ".OeOOOeeeeReeeO.",
        ".OeOOeeeeRReeeO.",
        ".OeOeeeeRRReeeO.",
        ".OeeeeeeRRReeeO.",
        ".OeeeeeeeeeeeeO.",
        ".OOOOOOOOOOOOOO.",
        ".......OO.......",
        "......OOOO......",
        "................",
        "................",
    },
    /* DESK_CONTACTS: address book */
    {
        "................",
        ".CCCCCCCCCCCCCC.",
        ".CCCCCWWWWCCCCC.",
        ".CCCCWWWWWWCCCC.",
        ".CCCCWWWWWWCCCC.",
        ".CCCCCWWWWCCCCC.",
        ".CCCCCCCCCCCCCC.",
        ".CCCWWWWWWWWCCC.",
        ".CCWWWWWWWWWWCC.",
        ".CWWWWWWWWWWWWC.",
        ".CWWWWWWWWWWWWC.",
        ".CCCCCCCCCCCCCC.",
        ".CCCCCCCCCCCCCC.",
        "................",
        "................",
        "................",
    },
    /* DESK_EDITOR: page and pencil */
    {
        "................",
        "..WWWWWWWK......",
        "..WeeeeeeWK.....",
        "..WeeeeeeeWK....",
        "..WeeeeeeeeK....",
        "..Weeeeeee......",
        "..Weeeee........",
        "..Weeeee..OO....",
        "..Weeee..OOOO...",
        "..Weee..OOOO....",
        "..Wee..OOOO.....",
        "..We..OOOO......",
        "..W..OOOO.......",
        ".....OO.........",
        "................",
        "................",
    },
    /* DESK_PAINT: paint bucket */
    {
        "................",
        "......KKKK......",
        ".....K....K.....",
        "....K......K....",
        "...KBBBBBBBBK...",
        "...KBBBBBBBBK...",
        "...KBBBBBBBBK...",
        "...KBBBBBBBBK...",
        "....KBBBBBBK....",
        ".....KKKKKK.....",
        "......BBB.......",
        ".....BBBBB......",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_STORE: shopping bag */
    {
        "................",
        "......KKKK......",
        ".....K....K.....",
        ".....K....K.....",
        "....KKKKKKKK....",
        "...PPPPPPPPPP...",
        "...PWWWWWWWWP...",
        "...PWWWWWWWWP...",
        "...PWWWWWWWWP...",
        "...PWWWWWWWWP...",
        "...PPPPPPPPPP...",
        "................",
        "................",
        "................",
        "................",
        "................",
    },
    /* DESK_NOTIF: bell */
    {
        "................",
        ".......YY.......",
        "......YWWY......",
        ".....YWWWWY.....",
        ".....YWWWWY.....",
        "....YWWWWWWY....",
        "....YWWWWWWY....",
        "...YWWWWWWWWY...",
        "...YWWWWWWWWY...",
        "..YWWWWWWWWWWY..",
        "..YKKKKKKKKKKY..",
        ".......YY.......",
        "......Y..Y......",
        "................",
        "................",
        "................",
    },
    /* generic window */
    {
        ".KKKKKKKKKKKKKK.",
        ".KRRRRRKKKKKKKK.",
        ".KKKKKKKKKKKKKK.",
        ".KeeeeeeeeeeeeK.",
        ".KeeeeeeeeeeeeK.",
        ".KeeeeeeeeeeeeK.",
        ".KeeeeeeeeeeeeK.",
        ".KeeeeeeeeeeeeK.",
        ".KeeeeeeeeeeeeK.",
        ".KeeeeeeeeeeeeK.",
        ".KKKKKKKKKKKKKK.",
        "................",
        "................",
        "................",
        "................",
        "................",
    },
};

static uint32_t icon_color(char c, uint32_t bg) {
    switch (c) {
    case 'K': return rgb(0x202226);
    case 'W': return rgb(0xFFFFFF);
    case 'R': return rgb(0xE5484D);
    case 'G': return rgb(0x46A758);
    case 'B': return rgb(0x3E7BFA);
    case 'Y': return rgb(0xFFD60A);
    case 'y': return rgb(0xF5C518);
    case 'O': return rgb(0xF76B15);
    case 'P': return rgb(0x8E4EC6);
    case 'C': return rgb(0x12A594);
    case 'g': return rgb(0x8B8D98);
    case 'd': return rgb(0x44474E);
    case 'b': return rgb(0x8EC8F8);
    case 'e': return rgb(0xF1F5F9);
    default: return bg;
    }
}

/* Draw mode's icon at (x, y), scaled by `scale` (1 or 2).
 * `bg` fills the transparent ('.') cells. */
static void draw_app_icon(desktop_mode m, uint64_t x, uint64_t y,
                          int scale, uint32_t bg) {
    int idx = (int)m;
    if (idx < 0 || idx > DESK_APP_COUNT) {
        idx = DESK_APP_COUNT;
    }
    for (int row = 0; row < 16; row++) {
        const char *line = APP_ICONS[idx][row];
        for (int col = 0; col < 16; col++) {
            uint32_t c = icon_color(line[col], bg);
            fb_fill_rect(x + (uint64_t)(col * scale),
                         y + (uint64_t)(row * scale),
                         (uint64_t)scale, (uint64_t)scale, c);
        }
    }
}

static void draw_dock(void) {
    uint64_t w = fb_width();
    uint32_t bg = col_dock();
    uint32_t fg = col_text();
    uint32_t acc = col_accent();
    const int bw = dock_btn_w();
    const int bh = dock_btn_h();
    const int gap = 4;
    int64_t start = 0;
    int64_t cross = 0;
    int count = dock_layout(&start, &cross);
    if (desk_settings.dock_pos == 1) {
        fb_fill_rect(0, 0, (uint64_t)(bw + 12), fb_height(), bg);
    } else {
        fb_fill_rect(0, dock_y(), w, (uint64_t)dock_strip_h(), bg);
    }
    for (int i = 0; i < count; i++) {
        int64_t bx = desk_settings.dock_pos == 1 ? cross
                                                 : start + i * (bw + gap);
        int64_t by = desk_settings.dock_pos == 1
                         ? start + i * (bh + gap)
                         : cross;
        desktop_mode app = desk_visible_mode(i);
        int active = (desk_mode == app);
        uint32_t fill = active ? acc : bg;
        uint32_t label = active ? rgb(0xFFFFFF) : fg;
        fb_fill_rect((uint64_t)bx, (uint64_t)by, (uint64_t)bw,
                     (uint64_t)bh, fill);
        if (!active) {
            for (int64_t px = bx; px < bx + bw; px++) {
                fb_put_pixel((uint64_t)px, (uint64_t)by, col_dim());
                fb_put_pixel((uint64_t)px, (uint64_t)(by + bh - 1),
                             col_dim());
            }
            for (int64_t py = by; py < by + bh; py++) {
                fb_put_pixel((uint64_t)bx, (uint64_t)py, col_dim());
                fb_put_pixel((uint64_t)(bx + bw - 1), (uint64_t)py,
                             col_dim());
            }
        }
        draw_app_icon(app, (uint64_t)(bx + 4),
                      (uint64_t)(by + (bh - 16) / 2), 1, fill);
        const char *name = mode_name(app);
        size_t len = strlen(name);
        int64_t tx = bx + 22 + (bw - 22 - (int64_t)(len * 6)) / 2;
        if (tx < bx + 22) {
            tx = bx + 22;
        }
        draw_text_small((uint64_t)tx, (uint64_t)(by + (bh - 8) / 2),
                        name, label, fill);
    }
}
static void draw_taskbar(void) {
    if (taskbar_vertical()) {
        uint64_t h = fb_height();
        uint64_t x = desk_settings.taskbar_side == 1
                         ? 0
                         : fb_width() - TASKBAR_W;
        uint32_t bgv = col_taskbar();
        uint32_t fgv = col_text();
        uint32_t accv = col_accent();
        fb_fill_rect(x, 0, TASKBAR_W, h, bgv);
        fb_fill_rect(desk_settings.taskbar_side == 1
                         ? TASKBAR_W - 2
                         : x,
                     0, 2, h, accv);
        draw_text_small(x + 10, 16, "PrismOS", accv, bgv);
        fb_fill_rect(x + 10, 40, TASKBAR_W - 20, 24,
                     launcher_active ? accv : bgv);
        draw_text_small(x + 22, 48, L("Apps", "アプリ"),
                        launcher_active ? rgb(0xFFFFFF) : fgv,
                        launcher_active ? accv : bgv);
        const char *mode = mode_name(desk_mode);
        draw_app_icon(desk_mode, x + 10, 84, 1, bgv);
        draw_text_small(x + 32, 88, mode, fgv, bgv);
        draw_text_small(x + 10, h - 40, date_text, fgv, bgv);
        draw_text_small(x + 10, h - 24, clock_text, fgv, bgv);
        return;
    }
    uint64_t w = fb_width();
    uint64_t y = taskbar_y();
    uint32_t bg = col_taskbar();
    uint32_t fg = col_text();
    uint32_t acc = col_accent();
    fb_fill_rect(0, y, w, TASKBAR_H, bg);
    fb_fill_rect(0, y, w, 2, acc);
    draw_text_small(16, y + 18, "PrismOS", acc, bg);
    /* Apps button: opens the launcher panel. */
    fb_fill_rect(104, y + 8, 60, 24, launcher_active ? acc : bg);
    draw_text_small(116, y + 16, L("Apps", "アプリ"),
                    launcher_active ? rgb(0xFFFFFF) : fg,
                    launcher_active ? acc : bg);
    const char *mode = mode_name(desk_mode);
    size_t len = strlen(mode);
    uint64_t mx = (w - (uint64_t)(len * 6)) / 2;
    draw_app_icon(desk_mode, mx - 22, y + 16, 1, bg);
    draw_text_small(mx, y + 18, mode, fg, bg);
    /* Clock, right aligned: 日付と時刻の両方を出す。 */
    uint64_t cx = w - 16 - 16 * 6;
    draw_text_small(cx, y + 18, date_text, fg, bg);
    draw_text_small(cx + 11 * 6, y + 18, clock_text, fg, bg);
}

/* The mouse cursor: a small arrow, white with a black shadow
 * so it stays visible on light and dark backgrounds. */
static const char *CURSOR_SHAPE[16] = {
    "X           ",
    "XX          ",
    "XXX         ",
    "XXXX        ",
    "XXXXX       ",
    "XXXXXX      ",
    "XXXXXXX     ",
    "XXXXXXXX    ",
    "XXXXXXXXX   ",
    "XXXXXX      ",
    "XXXX XX     ",
    "XXX   XX    ",
    "XX     XX   ",
    "X       XX  ",
    "         XX ",
    "            ",
};

static void draw_cursor(void) {
    uint32_t white = rgb(0xFFFFFF);
    uint32_t black = rgb(0x000000);
    /* Shadow first (offset by one pixel), then the arrow. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t colour = pass == 0 ? black : white;
        int ox = pass == 0 ? 1 : 0;
        int oy = pass == 0 ? 1 : 0;
        for (int row = 0; row < 16; row++) {
            for (int col = 0; col < 12; col++) {
                if (CURSOR_SHAPE[row][col] == 'X') {
                    fb_put_pixel((uint64_t)(mouse_x + col + ox),
                                 (uint64_t)(mouse_y + row + oy), colour);
                }
            }
        }
    }
}

static void switch_mode(desktop_mode m);

/* ================= Notifications ================= */

/* ---- アクセシビリティ: ナレーターと画面キーボード ---- */

/* ナレーター (読み上げの基礎): 有効な間、通知を
 * キャプションとして画面下部に表示し、シリアルにも
 * "narrator: ..." で記録する。音声合成エンジンは無い
 * ため、読み上げの基礎は文字キャプションとログで
 * 実装する (合成そのものは未実装)。 */
static char narrator_caption[160];

static void narrator_say(const char *text) {
    if (!desk_settings.narrator || text == NULL) {
        return;
    }
    size_t i = 0;
    while (text[i] != '\0' && i + 1 < sizeof(narrator_caption)) {
        narrator_caption[i] = text[i];
        i++;
    }
    narrator_caption[i] = '\0';
    serial_write("narrator: ");
    serial_write_line(narrator_caption);
}

/* 画面キーボード (OSK): 有効な間、画面下部にキーの
 * パネルを出す。クリックしたキーは物理キーボードと
 * 同じ desktop_handle_key へ入る。 */
#define OSK_KEY_H 36
#define OSK_GAP 4
#define OSK_ROWS 5
static const char *const OSK_ROW_KEYS[4] = {
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm,."
};
typedef struct {
    const char *label;
    int code;
    int units;
} osk_ctl_key;
static const osk_ctl_key OSK_CTL[6] = {
    {"Esc", 0x1B, 2},   {"Bksp", '\b', 3}, {"Space", ' ', 6},
    {"Enter", '\n', 3}, {"Up", DESK_KEY_UP, 2},
    {"Down", DESK_KEY_DOWN, 2},
};
#define OSK_CTL_UNITS 18

static int osk_panel_h(void) {
    return OSK_ROWS * (OSK_KEY_H + OSK_GAP) + OSK_GAP;
}

static uint64_t osk_panel_y(void) {
    uint64_t ty = taskbar_y();
    int h = osk_panel_h();
    return ty > (uint64_t)h ? ty - (uint64_t)h : 0;
}

static int osk_row_key_w(int n) {
    return (int)((fb_width() - 16 - (uint64_t)(OSK_GAP * (n + 1))) /
                 (uint64_t)n);
}

static void osk_draw_key(uint64_t kx, uint64_t ky, uint64_t kw,
                         const char *label) {
    fb_fill_rect(kx, ky, kw, OSK_KEY_H, rgb(0x3A3A44));
    draw_text_small(kx + 6, ky + 12, label, rgb(0xFFFFFF),
                    rgb(0x3A3A44));
}

static void draw_osk(void) {
    if (!desk_settings.osk) {
        return;
    }
    uint64_t py = osk_panel_y();
    fb_fill_rect(0, py, fb_width(), (uint64_t)osk_panel_h(),
                 rgb(0x202028));
    for (int r = 0; r < 4; r++) {
        const char *keys = OSK_ROW_KEYS[r];
        int n = (int)strlen(keys);
        int kw = osk_row_key_w(n);
        for (int i = 0; i < n; i++) {
            uint64_t kx = 8 + OSK_GAP +
                          (uint64_t)i * (uint64_t)(kw + OSK_GAP);
            uint64_t ky = py + OSK_GAP +
                          (uint64_t)r * (OSK_KEY_H + OSK_GAP);
            char label[2] = {keys[i], '\0'};
            osk_draw_key(kx, ky, (uint64_t)kw, label);
        }
    }
    {
        uint64_t ky = py + OSK_GAP +
                      (uint64_t)4 * (OSK_KEY_H + OSK_GAP);
        int unit = (int)((fb_width() - 16 - OSK_GAP * 7) /
                         OSK_CTL_UNITS);
        uint64_t kx = 8 + OSK_GAP;
        for (int i = 0; i < 6; i++) {
            uint64_t kw = (uint64_t)(OSK_CTL[i].units * unit);
            osk_draw_key(kx, ky, kw, OSK_CTL[i].label);
            kx += kw + OSK_GAP;
        }
    }
}

/* パネル上のクリック位置からキーコードを返す。
 * パネル外は -1、パネルの隙間は -2。 */
static int osk_key_at(int x, int y) {
    if (!desk_settings.osk) {
        return -1;
    }
    uint64_t py = osk_panel_y();
    if (y < (int)py) {
        return -1;
    }
    int rel = y - (int)py - OSK_GAP;
    if (rel < 0) {
        return -2;
    }
    int r = rel / (OSK_KEY_H + OSK_GAP);
    if (r >= OSK_ROWS || (rel % (OSK_KEY_H + OSK_GAP)) >= OSK_KEY_H) {
        return -2;
    }
    int relx = x - 8 - OSK_GAP;
    if (relx < 0) {
        return -2;
    }
    if (r < 4) {
        const char *keys = OSK_ROW_KEYS[r];
        int n = (int)strlen(keys);
        int kw = osk_row_key_w(n);
        int i = relx / (kw + OSK_GAP);
        if (i >= n || (relx % (kw + OSK_GAP)) >= kw) {
            return -2;
        }
        return (unsigned char)keys[i];
    }
    {
        int unit = (int)((fb_width() - 16 - OSK_GAP * 7) /
                         OSK_CTL_UNITS);
        int acc = 0;
        for (int i = 0; i < 6; i++) {
            int w = OSK_CTL[i].units * unit;
            if (relx >= acc && relx < acc + w) {
                return OSK_CTL[i].code;
            }
            acc += w + OSK_GAP;
        }
    }
    return -2;
}

/* OSK の注入キュー: クリックされたキーはここに入り、
 * シェルの入力ループが物理キーと同じ経路で取り出す
 * (desktop_handle_key で消費されなければ行入力になる)。 */
#define OSK_QUEUE_MAX 16
static int osk_queue[OSK_QUEUE_MAX];
static int osk_queue_len;

int desktop_osk_poll_key(void) {
    if (osk_queue_len <= 0) {
        return -1;
    }
    int c = osk_queue[0];
    for (int i = 1; i < osk_queue_len; i++) {
        osk_queue[i - 1] = osk_queue[i];
    }
    osk_queue_len--;
    return c;
}

static void draw_narrator_caption(void) {
    if (!desk_settings.narrator || narrator_caption[0] == '\0') {
        return;
    }
    uint64_t base = desk_settings.osk ? osk_panel_y() : taskbar_y();
    if (base < 40) {
        return;
    }
    fb_fill_rect(0, base - 40, fb_width(), 40, rgb(0x000000));
    draw_text_small(12, base - 32, narrator_caption, rgb(0xFFFF00),
                    rgb(0x000000));
}

void desktop_notify(const char *msg) {
    if (msg == NULL) {
        return;
    }
    int slot = -1;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (notify_ttl[i] == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0; /* overwrite the oldest slot */
    }
    size_t n = 0;
    while (msg[n] != '\0' && n + 1 < NOTIFY_TEXT_MAX) {
        notify_text[slot][n] = msg[n];
        n++;
    }
    notify_text[slot][n] = '\0';
    {
        int hs = notify_hist_count % NOTIFY_HIST_MAX;
        size_t hn = 0;
        while (notify_text[slot][hn] != '\0' &&
               hn + 1 < NOTIFY_TEXT_MAX) {
            notify_hist[hs][hn] = notify_text[slot][hn];
            hn++;
        }
        notify_hist[hs][hn] = '\0';
        notify_hist_count++;
        notify_hist_save();
    }
    notify_ttl[slot] = 6; /* about six seconds */
    if (slot >= notify_count) {
        notify_count = slot + 1;
    }
    serial_write("notify: ");
    serial_write_line(notify_text[slot]);
    narrator_say(notify_text[slot]);
    if (desk_ok) {
        desktop_redraw();
    }
}

/* 通知履歴アプリ: 過去の通知を新しい順に一覧する。 */
static void draw_notiflog(void) {
    uint32_t fg = col_text();
    uint32_t bg = col_window_bg();
    uint32_t acc = col_accent();
    uint64_t x0 = text_x();
    uint64_t y0 = text_y();
    draw_text_small(x0, y0, L("Notifications", "通知履歴"), acc,
                    bg);
    int total = notify_hist_count < NOTIFY_HIST_MAX
                    ? notify_hist_count
                    : NOTIFY_HIST_MAX;
    if (total == 0) {
        draw_text_small(x0, y0 + (uint64_t)(2 * CELL_H),
                        L("(no notifications yet)",
                          "(まだ通知はありません)"),
                        col_dim(), bg);
    }
    int rows = text_rows() - 4;
    if (notif_scroll > total - 1) {
        notif_scroll = total > 0 ? total - 1 : 0;
    }
    if (notif_scroll < 0) {
        notif_scroll = 0;
    }
    for (int i = 0; i < rows && notif_scroll + i < total; i++) {
        /* 新しい順: 履歴の末尾から数える。 */
        int idx = (notify_hist_count - 1 - (notif_scroll + i)) %
                  NOTIFY_HIST_MAX;
        if (idx < 0) {
            idx += NOTIFY_HIST_MAX;
        }
        fb_draw_text(x0 + 4,
                     y0 + (uint64_t)((2 + i) * CELL_H),
                     notify_hist[idx], fg, bg, 1);
    }
    draw_text_small(x0,
                    y0 + (uint64_t)((text_rows() - 1) * CELL_H),
                    L("Up/Down = scroll  Esc = back",
                      "上下 = スクロール  Esc = 戻る"),
                    col_dim(), bg);
}

static void draw_notifications(void) {
    uint64_t w = fb_width();
    uint64_t y = desk_settings.taskbar_top ? TASKBAR_H + 8 : 8;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (notify_ttl[i] == 0) {
            continue;
        }
        size_t len = strlen(notify_text[i]);
        uint64_t bw = (uint64_t)(len * 6) + 24;
        uint64_t bx = w - bw - 10;
        fb_fill_rect(bx, y, bw, 28, col_taskbar());
        for (uint64_t px = bx; px < bx + bw; px++) {
            fb_put_pixel(px, y, col_accent());
            fb_put_pixel(px, y + 27, col_accent());
        }
        for (uint64_t py = y; py < y + 28; py++) {
            fb_put_pixel(bx, py, col_accent());
            fb_put_pixel(bx + bw - 1, py, col_accent());
        }
        draw_text_small(bx + 12, y + 10, notify_text[i], col_text(),
                        col_taskbar());
        y += 34;
    }
}

/* ================= Clipboard ================= */

void desktop_clipboard_set(const char *s, size_t n) {
    if (s == NULL) {
        n = 0;
    }
    if (n > CLIP_MAX - 1) {
        n = CLIP_MAX - 1;
    }
    for (size_t i = 0; i < n; i++) {
        desk_clipboard[i] = s[i];
    }
    desk_clipboard[n] = '\0';
    desk_clipboard_len = n;
}

const char *desktop_clipboard_get(size_t *n) {
    if (n != NULL) {
        *n = desk_clipboard_len;
    }
    return desk_clipboard;
}

/* ================= Floating windows ================= */

static int win_w_of(int kind) {
    return kind == WIN_NOTES ? 460 : kind == WIN_CALC ? 240 : 300;
}
static int win_h_of(int kind) {
    return kind == WIN_NOTES ? 300 : kind == WIN_CALC ? 300 : 160;
}

static void win_open_kind(int kind) {
    if (!win_open[kind]) {
        win_open[kind] = 1;
        win_px[kind] = 120 + kind * 60;
        win_py[kind] = 110 + kind * 40;
        if (kind == WIN_NOTES) {
            notes_load();
        }
        if (kind == WIN_CALC && calc_len == 0) {
            calc_display[0] = '0';
            calc_display[1] = '\0';
            calc_len = 1;
            calc_fresh = 1;
        }
    }
    win_z[kind] = ++win_z_top;
    win_focus = kind;
    serial_write("window: opened ");
    serial_write_line(WIN_NAMES[kind]);
    desktop_redraw();
}

static void win_close(int kind) {
    if (kind == WIN_NOTES && notes_dirty) {
        notes_save();
    }
    win_open[kind] = 0;
    if (win_focus == kind) {
        win_focus = -1;
        int best = -1;
        for (int i = 0; i < WIN_COUNT; i++) {
            if (win_open[i] && (best < 0 || win_z[i] > win_z[best])) {
                best = i;
            }
        }
        win_focus = best;
    }
    serial_write("window: closed ");
    serial_write_line(WIN_NAMES[kind]);
    desktop_redraw();
}

/* The topmost open window at (x, y), or -1. */
static int win_at(int x, int y) {
    int best = -1;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (!win_open[i]) {
            continue;
        }
        if (x >= win_px[i] && x < win_px[i] + win_w_of(i) &&
            y >= win_py[i] && y < win_py[i] + win_h_of(i)) {
            if (best < 0 || win_z[i] > win_z[best]) {
                best = i;
            }
        }
    }
    return best;
}

static int64_t calc_parse(void) {
    int64_t v = 0;
    int neg = 0;
    int i = 0;
    if (calc_display[0] == '-') {
        neg = 1;
        i = 1;
    }
    for (; calc_display[i] != '\0'; i++) {
        if (calc_display[i] >= '0' && calc_display[i] <= '9') {
            v = v * 10 + (calc_display[i] - '0');
        }
    }
    return neg ? -v : v;
}

static void calc_show(int64_t v) {
    char tmp[24];
    int n = 0;
    int neg = 0;
    uint64_t u;
    if (v < 0) {
        neg = 1;
        u = (uint64_t)(-(v + 1)) + 1;
    } else {
        u = (uint64_t)v;
    }
    if (u == 0) {
        tmp[n++] = '0';
    }
    while (u > 0) {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    int p = 0;
    if (neg) {
        calc_display[p++] = '-';
    }
    while (n > 0) {
        calc_display[p++] = tmp[--n];
    }
    calc_display[p] = '\0';
    calc_len = p;
}

static void calc_apply(void) {
    int64_t v = calc_parse();
    if (calc_op == 0) {
        calc_acc = v;
        return;
    }
    switch (calc_op) {
    case '+': calc_acc += v; break;
    case '-': calc_acc -= v; break;
    case '*': calc_acc *= v; break;
    case '/':
        if (v == 0) {
            calc_show(0);
            calc_len = 3;
            calc_display[0] = 'E';
            calc_display[1] = 'r';
            calc_display[2] = 'r';
            calc_display[3] = '\0';
            calc_op = 0;
            calc_fresh = 1;
            return;
        }
        calc_acc /= v;
        break;
    }
}

static void calc_input(int c) {
    if (c >= '0' && c <= '9') {
        if (calc_fresh || calc_len == 0 ||
            (calc_len == 1 && calc_display[0] == '0')) {
            calc_len = 0;
            calc_fresh = 0;
        }
        if (calc_len + 1 < (int)sizeof(calc_display)) {
            calc_display[calc_len++] = (char)c;
            calc_display[calc_len] = '\0';
        }
        return;
    }
    if (c == '+' || c == '-' || c == '*' || c == '/') {
        if (!calc_fresh) {
            calc_apply();
        }
        calc_op = c;
        calc_fresh = 1;
        calc_show(calc_acc);
        return;
    }
    if (c == '=' || c == '\n' || c == '\r') {
        if (calc_op != 0) {
            calc_apply();
            calc_op = 0;
        } else {
            calc_acc = calc_parse();
        }
        calc_fresh = 1;
        calc_show(calc_acc);
        serial_write("calc: result ");
        serial_write_line(calc_display);
        return;
    }
    if (c == 'c' || c == 'C') {
        calc_acc = 0;
        calc_op = 0;
        calc_fresh = 1;
        calc_show(0);
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (!calc_fresh && calc_len > 0) {
            calc_display[--calc_len] = '\0';
            if (calc_len == 0) {
                calc_show(0);
                calc_fresh = 1;
            }
        }
        return;
    }
}

/* One key for the focused window. Returns 1 if consumed. */
static int win_key(int kind, int c) {
    if (kind == WIN_CALC) {
        calc_input(c);
        desktop_redraw();
        return 1;
    }
    if (kind == WIN_NOTES) {
        if (c == 0xA2) { /* Ctrl+V: paste */
            size_t n = 0;
            const char *clip = desktop_clipboard_get(&n);
            for (size_t i = 0; i < n; i++) {
                if (notes_len + 1 < PRISM_VFS_MAX_APP_FILE_SIZE) {
                    /* insert at cursor */
                    for (size_t j = notes_len; j > notes_cursor; j--) {
                        notes_buf[j] = notes_buf[j - 1];
                    }
                    notes_buf[notes_cursor++] = clip[i];
                    notes_len++;
                    notes_buf[notes_len] = '\0';
                    notes_dirty = 1;
                }
            }
            desktop_redraw();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (notes_cursor > 0) {
                for (size_t j = notes_cursor - 1; j + 1 < notes_len; j++) {
                    notes_buf[j] = notes_buf[j + 1];
                }
                notes_cursor--;
                notes_len--;
                notes_buf[notes_len] = '\0';
                notes_dirty = 1;
            }
            desktop_redraw();
            return 1;
        }
        if (c >= 32 && c < 127) {
            if (notes_len + 1 < PRISM_VFS_MAX_APP_FILE_SIZE) {
                for (size_t j = notes_len; j > notes_cursor; j--) {
                    notes_buf[j] = notes_buf[j - 1];
                }
                notes_buf[notes_cursor++] = (char)c;
                notes_len++;
                notes_buf[notes_len] = '\0';
                notes_dirty = 1;
            }
            desktop_redraw();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            if (notes_len + 1 < PRISM_VFS_MAX_APP_FILE_SIZE) {
                for (size_t j = notes_len; j > notes_cursor; j--) {
                    notes_buf[j] = notes_buf[j - 1];
                }
                notes_buf[notes_cursor++] = '\n';
                notes_len++;
                notes_buf[notes_len] = '\0';
                notes_dirty = 1;
            }
            desktop_redraw();
            return 1;
        }
        return 1; /* other keys in a focused window: consumed */
    }
    return 1; /* clock window: any key is consumed except Esc
                 (handled by the caller) */
}

/* ウィンドウの表示名 (言語切替つき)。WIN_NAMES は
 * ログ用に英語のまま残す。 */
static const char *win_name(int kind) {
    switch (kind) {
        case WIN_NOTES:
            return L("Notes", "メモ");
        case WIN_CALC:
            return L("Calculator", "電卓");
        default:
            return L("Clock", "時計");
    }
}

static void draw_window_frame(int kind) {
    int x = win_px[kind];
    int y = win_py[kind];
    int w = win_w_of(kind);
    int h = win_h_of(kind);
    uint32_t bg = col_window_bg();
    uint32_t title = win_focus == kind ? col_accent() : col_taskbar();
    fb_fill_rect((uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, bg);
    fb_fill_rect((uint64_t)x, (uint64_t)y, (uint64_t)w, 24, title);
    draw_text_small((uint64_t)(x + 8), (uint64_t)(y + 8), win_name(kind),
                    rgb(0xFFFFFF), title);
    /* Close box. */
    fb_fill_rect((uint64_t)(x + w - 24), (uint64_t)y, 24, 24,
                 rgb(0xC0392B));
    draw_text_small((uint64_t)(x + w - 15), (uint64_t)(y + 8), "x",
                    rgb(0xFFFFFF), rgb(0xC0392B));
    /* Border. */
    for (int px = x; px < x + w; px++) {
        fb_put_pixel((uint64_t)px, (uint64_t)y, col_dim());
        fb_put_pixel((uint64_t)px, (uint64_t)(y + h - 1), col_dim());
    }
    for (int py = y; py < y + h; py++) {
        fb_put_pixel((uint64_t)x, (uint64_t)py, col_dim());
        fb_put_pixel((uint64_t)(x + w - 1), (uint64_t)py, col_dim());
    }
    if (kind == WIN_NOTES) {
        /* Show the last lines of the notes buffer. */
        int lines = (h - 40) / CELL_H;
        int cols = (w - 16) / CELL_W;
        /* Find the start of the visible tail. */
        size_t pos = notes_len;
        int up = lines;
        while (pos > 0 && up > 0) {
            pos--;
            if (notes_buf[pos] == '\n') {
                up--;
            }
        }
        if (pos > 0 && notes_buf[pos - 1] != '\n' && pos < notes_len) {
            /* mid-line start: fine for a preview window */
        }
        int row = 0;
        int col = 0;
        for (size_t i = pos; i < notes_len && row < lines; i++) {
            char ch = notes_buf[i];
            if (ch == '\n') {
                row++;
                col = 0;
                continue;
            }
            if (col >= cols) {
                row++;
                col = 0;
                if (row >= lines) {
                    break;
                }
            }
            fb_draw_char((uint64_t)(x + 8 + col * CELL_W),
                         (uint64_t)(y + 32 + row * CELL_H), ch,
                         col_text(), bg, TEXT_SCALE);
            col++;
        }
    } else if (kind == WIN_CALC) {
        fb_fill_rect((uint64_t)(x + 12), (uint64_t)(y + 36),
                     (uint64_t)(w - 24), 28, col_taskbar());
        size_t dl = strlen(calc_display);
        draw_text_small((uint64_t)(x + w - 20 - (int)dl * 6),
                        (uint64_t)(y + 46), calc_display, col_text(),
                        col_taskbar());
        draw_text_small((uint64_t)(x + 12), (uint64_t)(y + 76),
                        L("Keys: 0-9 + - * / = C", "キー: 0-9 + - * / = C"), col_dim(), bg);
        draw_text_small((uint64_t)(x + 12), (uint64_t)(y + 92),
                        L("Enter = result", "Enter = 結果"), col_dim(), bg);
    } else {
        draw_text_small((uint64_t)(x + 16), (uint64_t)(y + 48),
                        clock_text, col_text(), bg);
        prism_rtc_time t;
        if (rtc_read(&t) == 0) {
            char dbuf[24];
            int p = 0;
            int yr = t.year;
            int mo = t.month;
            int dy = t.day;
            dbuf[p++] = (char)('0' + (yr / 1000) % 10);
            dbuf[p++] = (char)('0' + (yr / 100) % 10);
            dbuf[p++] = (char)('0' + (yr / 10) % 10);
            dbuf[p++] = (char)('0' + yr % 10);
            dbuf[p++] = '-';
            dbuf[p++] = (char)('0' + (mo / 10) % 10);
            dbuf[p++] = (char)('0' + mo % 10);
            dbuf[p++] = '-';
            dbuf[p++] = (char)('0' + (dy / 10) % 10);
            dbuf[p++] = (char)('0' + dy % 10);
            dbuf[p] = '\0';
            draw_text_small((uint64_t)(x + 16), (uint64_t)(y + 68),
                            dbuf, col_dim(), bg);
        }
    }
}

static void draw_windows(void) {
    if (desk_mode != DESK_TERMINAL) {
        return;
    }
    /* Draw from the back to the front. */
    for (int z = 0; z <= win_z_top; z++) {
        for (int i = 0; i < WIN_COUNT; i++) {
            if (win_open[i] && win_z[i] == z) {
                draw_window_frame(i);
            }
        }
    }
}

/* ================= Launcher ================= */

/* ================= 電源メニューとロック画面 =================
 * ランチャーの末尾に電源項目 (ロック/ユーザー切替/スリープ/
 * 再起動/シャットダウン) を出す。処理はシェルの power /
 * sleep / poweroff / lock と同じ部品 (acpi_poweroff、
 * acpi_enter_s3、shell_reboot_now、account_verify +
 * bio_verify) に接続する。ユーザー切替はシェルのログイン
 * フローへ戻る (shell_request_user_switch)。 */

static int desk_locked;
static char lock_buf[17];
static int lock_len;
static int lock_failed;
/* ロック画面の試行制限 (監査 M3)。ログインと同じ政策
 * (lockout_attempts 回の連続失敗で lockout_seconds 秒の
 * 待ち。秒数が 0 なら停止) を適用し、失敗は監査に残す。
 * 以前は無制限・無記録に試せた。 */
static int lock_wrong;
static uint32_t lock_wait_until; /* 待ちの終わる epoch 秒。0 = 待ちなし */

static uint32_t lock_now_epoch(void) {
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        return 0;
    }
    return rtc_epoch_seconds(&t);
}

static int lock_waiting(void) {
    return lock_wait_until != 0 &&
           lock_now_epoch() < lock_wait_until;
}

static void draw_lock_screen(void) {
    uint64_t w = fb_width();
    uint64_t h = fb_height();
    fb_fill_rect(0, 0, w, h, rgb(0x0B1026));
    uint64_t cx = w / 2 - 120;
    uint64_t cy = h / 2 - 90;
    draw_text_small(cx, cy, "PrismOS", col_accent(), rgb(0x0B1026));
    draw_text_small(cx, cy + (uint64_t)(2 * CELL_H),
                    L("Locked", "ロック中"), rgb(0xFFFFFF),
                    rgb(0x0B1026));
    {
        char line[64];
        size_t p = 0;
        const char *u = shell_current_user_name();
        while (*u != '\0' && p + 1 < sizeof(line)) {
            line[p++] = *u++;
        }
        line[p] = '\0';
        draw_text_small(cx, cy + (uint64_t)(3 * CELL_H), line,
                        rgb(0xC8D2E0), rgb(0x0B1026));
    }
    {
        char dots[20];
        int n = lock_len;
        if (n > 16) {
            n = 16;
        }
        for (int i = 0; i < n; i++) {
            dots[i] = '*';
        }
        dots[n] = '\0';
        draw_text_small(cx, cy + (uint64_t)(5 * CELL_H),
                        n > 0 ? dots : " ", rgb(0xFFFFFF),
                        rgb(0x0B1026));
    }
    if (lock_waiting()) {
        draw_text_small(cx, cy + (uint64_t)(6 * CELL_H),
                        L("Too many attempts. Locked for a while.",
                          "試行回数が多すぎます。しばらくロックされます"),
                        rgb(0xFF8080), rgb(0x0B1026));
    } else if (lock_failed) {
        draw_text_small(cx, cy + (uint64_t)(6 * CELL_H),
                        L("Wrong PIN.", "PINが違います"),
                        rgb(0xFF8080), rgb(0x0B1026));
    }
    draw_text_small(cx, cy + (uint64_t)(7 * CELL_H),
                    L("Type your PIN and press Enter",
                      "PINを入力してEnter"),
                    rgb(0x8A94A6), rgb(0x0B1026));
    if (bio_enrolled(BIO_TYPE_FINGER) || bio_enrolled(BIO_TYPE_FACE)) {
        draw_text_small(cx, cy + (uint64_t)(8 * CELL_H),
                        L("Tab = use biometrics",
                          "Tab = 生体認証を使う"),
                        rgb(0x8A94A6), rgb(0x0B1026));
    }
}

/* ロック中のキー処理。シェルの lock と同じ照合
 * (主アカウントの PIN、または生体認証) で解除する。 */
static int lock_key(int c) {
    if (c == '\t' &&
        (bio_enrolled(BIO_TYPE_FINGER) || bio_enrolled(BIO_TYPE_FACE))) {
        int rc = -3;
        if (bio_enrolled(BIO_TYPE_FINGER)) {
            rc = bio_verify(BIO_TYPE_FINGER);
        }
        if (rc != 0 && bio_enrolled(BIO_TYPE_FACE)) {
            rc = bio_verify(BIO_TYPE_FACE);
        }
        if (rc == 0) {
            desk_locked = 0;
            lock_len = 0;
            lock_buf[0] = '\0';
            lock_failed = 0;
            lock_wrong = 0;
            lock_wait_until = 0;
            audit_log_event("unlock", "biometrics accepted");
            serial_write_line("desktop: unlocked (biometrics)");
        } else {
            lock_failed = 1;
            serial_write_line("desktop: biometric unlock failed");
        }
        desktop_redraw();
        return 1;
    }
    if (c == '\n' || c == '\r') {
        lock_buf[lock_len] = '\0';
        if (lock_wait_until != 0 && !lock_waiting()) {
            /* 待ち時間が過ぎた。失敗回数を数え直す。 */
            lock_wait_until = 0;
            lock_wrong = 0;
        }
        if (lock_waiting()) {
            /* ロックアウト中は照合自体をしない。 */
            lock_failed = 1;
            serial_write_line("desktop: unlock refused (lockout)");
            lock_len = 0;
            lock_buf[0] = '\0';
            desktop_redraw();
            return 1;
        }
        size_t len = 0;
        const char *text = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
        if (text != NULL && account_verify(lock_buf, text, len) == 1) {
            desk_locked = 0;
            lock_wrong = 0;
            lock_wait_until = 0;
            bio_unlock_reset();
            audit_log_event("unlock", "PIN accepted");
            serial_write_line("desktop: unlocked");
        } else {
            lock_failed = 1;
            /* シェルの lock と同じく失敗を監査に残す (監査 M3)。 */
            audit_log_event("unlock-fail",
                            "wrong PIN at lock screen");
            serial_write_line("desktop: unlock failed");
            lock_wrong++;
            int attempts = desk_settings.lockout_attempts;
            if (attempts < 1) {
                attempts = 1;
            }
            if (attempts > 10) {
                attempts = 10;
            }
            if (lock_wrong >= attempts) {
                audit_log_event(
                    "lockout", "too many wrong PINs at lock screen");
                if (desk_settings.lockout_seconds > 0) {
                    lock_wait_until =
                        lock_now_epoch() +
                        (uint32_t)desk_settings.lockout_seconds;
                    lock_wrong = 0;
                    serial_write_line("desktop: lock screen locked out");
                } else {
                    /* 秒数 0 = 回数を超えたら止まる (設定の
                     * 取り決め。ログインとシェルの lock に同じ)。 */
                    serial_write_line(
                        "desktop: too many unlock attempts, halting");
                    prism_halt();
                }
            }
        }
        lock_len = 0;
        lock_buf[0] = '\0';
        desktop_redraw();
        return 1;
    }
    if (c == '\b' || c == 0x7F) {
        if (lock_len > 0) {
            lock_len--;
            lock_buf[lock_len] = '\0';
        }
        desktop_redraw();
        return 1;
    }
    if (c >= 32 && c < 127 && lock_len < 16) {
        lock_buf[lock_len++] = (char)c;
        lock_buf[lock_len] = '\0';
        lock_failed = 0;
        desktop_redraw();
        return 1;
    }
    return 1; /* ロック中は他のキーをすべて消費する */
}

static void power_action(int which) {
    switch (which) {
        case 0: { /* ロック */
            size_t len = 0;
            const char *text = vfs_read_ptr(PRISM_ACCOUNT_FILE, &len);
            if (text == NULL && !bio_enrolled(BIO_TYPE_FINGER) &&
                !bio_enrolled(BIO_TYPE_FACE)) {
                desktop_notify(L("No PIN set: lock unavailable",
                                 "PINが未設定のためロックできません"));
                serial_write_line("power: lock unavailable (no PIN)");
                return;
            }
            desk_locked = 1;
            lock_len = 0;
            lock_buf[0] = '\0';
            lock_failed = 0;
            lock_wrong = 0;
            lock_wait_until = 0;
            audit_log_event("lock", "screen locked from desktop");
            serial_write_line("desktop: locked");
            break;
        }
        case 1: /* ユーザー切替 */
            serial_write_line("desktop: switch user");
            persist_maybe_sync();
            shell_request_user_switch();
            break;
        case 2: /* スリープ */
            if (acpi_s3_available()) {
                serial_write_line("desktop: entering S3 sleep");
                if (acpi_enter_s3() == 0) {
                    serial_write_line("desktop: resumed from S3");
                }
            } else {
                desktop_notify(L("Sleep is not available here",
                                 "このマシンではスリープできません"));
                serial_write_line("desktop: sleep unavailable");
            }
            break;
        case 3: /* 再起動 */
            serial_write_line("desktop: restart");
            persist_maybe_sync();
            shell_reboot_now();
            break;
        default: /* シャットダウン */
            serial_write_line("desktop: shutdown");
            persist_maybe_sync();
            (void)acpi_poweroff();
            /* ここに来るのは電源を切れなかった場合だけ。 */
            desktop_notify(L("Could not shut down",
                             "シャットダウンできませんでした"));
            serial_write_line("desktop: poweroff failed");
            break;
    }
    desktop_redraw();
}

#define LAUNCH_COLS 4
#define LAUNCH_CELL_W 150
#define LAUNCH_CELL_H 46
#define LAUNCH_GAP 10
#define LAUNCH_PAD 16

#define POWER_COUNT 5
#define WIDGETS_COUNT 1

static int launcher_entries(void) {
    return desk_visible_count() + WIN_COUNT + WIDGETS_COUNT +
           POWER_COUNT;
}

static void launcher_geometry(int *pw, int *ph, int *px, int *py) {
    int rows = (launcher_entries() + LAUNCH_COLS - 1) / LAUNCH_COLS;
    *pw = LAUNCH_PAD * 2 + LAUNCH_COLS * LAUNCH_CELL_W +
          (LAUNCH_COLS - 1) * LAUNCH_GAP;
    *ph = LAUNCH_PAD * 2 + 28 + rows * LAUNCH_CELL_H +
          (rows - 1) * LAUNCH_GAP;
    *px = ((int)fb_width() - *pw) / 2;
    *py = ((int)fb_height() - *ph) / 2;
}

static const char *launcher_label(int idx, desktop_mode *app_out) {
    int apps = desk_visible_count();
    if (idx < apps) {
        *app_out = desk_visible_mode(idx);
        return mode_name(*app_out);
    }
    *app_out = DESK_TERMINAL;
    if (idx < apps + WIN_COUNT) {
        return win_name(idx - apps);
    }
    if (idx == apps + WIN_COUNT) {
        return L("Widgets", "ウィジェット");
    }
    /* 末尾は電源項目。 */
    switch (idx - apps - WIN_COUNT - WIDGETS_COUNT) {
        case 0:
            return L("Lock", "ロック");
        case 1:
            return L("Switch user", "ユーザー切替");
        case 2:
            return L("Sleep", "スリープ");
        case 3:
            return L("Restart", "再起動");
        default:
            return L("Shut down", "シャットダウン");
    }
}

static void widgets_open_panel(void);

static void launcher_activate(int idx) {
    int apps = desk_visible_count();
    launcher_active = 0;
    if (idx < apps) {
        desktop_mode app = desk_visible_mode(idx);
        serial_write("launcher: ");
        serial_write_line(mode_name(app));
        switch_mode(app);
    } else if (idx < apps + WIN_COUNT) {
        win_open_kind(idx - apps);
    } else if (idx == apps + WIN_COUNT) {
        widgets_open_panel();
    } else {
        desktop_mode dummy;
        serial_write("launcher: ");
        serial_write_line(launcher_label(idx, &dummy));
        power_action(idx - apps - WIN_COUNT - WIDGETS_COUNT);
    }
    desktop_redraw();
}

/* ---- デスクトップウィジェット (バッチ4 項目21) ----
 * ランチャーから開くパネル。カレンダーは RTC の
 * ローカルデータ、天気・ニュースは設定で指定した
 * URL のプレーンテキストフィードから取る。 */
static int widgets_open;
static char widgets_weather[3][56];
static int widgets_weather_count;
static int widgets_weather_state; /* 0=未設定 1=ok -1=失敗 */
static char widgets_news[5][56];
static int widgets_news_count;
static int widgets_news_state;
static char widgets_buf[8192];

/* フィード本文を行に分解する (タグは読み飛ばす)。 */
static void widgets_set_lines(const char *body, size_t len,
                              char lines[][56], int max_lines,
                              int *out_count) {
    int count = 0;
    size_t i = 0;
    while (i < len && count < max_lines) {
        /* 行頭の空白と空行を飛ばす。 */
        while (i < len &&
               (body[i] == ' ' || body[i] == '\t' ||
                body[i] == '\r' || body[i] == '\n')) {
            i++;
        }
        if (i >= len) {
            break;
        }
        if (body[i] == '<') {
            /* タグは次の '>' まで読み飛ばす。 */
            while (i < len && body[i] != '>') {
                i++;
            }
            if (i < len) {
                i++;
            }
            continue;
        }
        size_t p = 0;
        while (i < len && body[i] != '\n' && body[i] != '\r' &&
               body[i] != '<') {
            if (p + 1 < 56) {
                lines[count][p++] = body[i];
            }
            i++;
        }
        /* 行末の空白を落とす。 */
        while (p > 0 && lines[count][p - 1] == ' ') {
            p--;
        }
        lines[count][p] = '\0';
        if (p > 0) {
            count++;
        }
    }
    *out_count = count;
}

static int widgets_fetch_one(const char *url, char lines[][56],
                             int max_lines, int *out_count) {
    if (url[0] == '\0') {
        *out_count = 0;
        return 0;
    }
    size_t len = 0;
    int status = 0;
    char final_url[256];
    if (br_fetch_buf(url, widgets_buf, sizeof(widgets_buf), &len,
                     &status, final_url, sizeof(final_url)) != 0) {
        *out_count = 0;
        return -1;
    }
    widgets_set_lines(widgets_buf, len, lines, max_lines, out_count);
    return 1;
}

static void widgets_fetch(void) {
    widgets_weather_state = widgets_fetch_one(
        desk_settings.feed_weather, widgets_weather, 3,
        &widgets_weather_count);
    serial_write_line(widgets_weather_state == 1
                          ? "widgets: weather ok"
                          : (widgets_weather_state == 0
                                 ? "widgets: weather not set"
                                 : "widgets: weather failed"));
    widgets_news_state = widgets_fetch_one(
        desk_settings.feed_news, widgets_news, 5,
        &widgets_news_count);
    serial_write_line(widgets_news_state == 1
                          ? "widgets: news ok"
                          : (widgets_news_state == 0
                                 ? "widgets: news not set"
                                 : "widgets: news failed"));
}

static void widgets_open_panel(void) {
    widgets_open = 1;
    widgets_fetch();
    serial_write_line("widgets: opened");
}

static void draw_widgets(void) {
    if (!widgets_open) {
        return;
    }
    uint64_t px = 8;
    uint64_t py = 56;
    uint64_t pw = 440;
    uint64_t ph = fb_height() - DOCK_H - 64;
    fb_fill_rect(px, py, pw, ph, col_taskbar());
    uint32_t fg = col_text();
    uint32_t bg = col_taskbar();
    uint32_t acc = col_accent();
    uint64_t x = px + 14;
    uint64_t y = py + 10;
    draw_text_small(x, y, L("Widgets", "ウィジェット"), acc, bg);
    y += 28;
    /* カレンダー (RTC のローカルデータ)。 */
    prism_rtc_time t;
    if (rtc_read(&t) == 0) {
        int year = 2000 + t.year;
        int month = t.month;
        char title[32];
        size_t tp = 0;
        title[tp++] = (char)('0' + (year / 1000) % 10);
        title[tp++] = (char)('0' + (year / 100) % 10);
        title[tp++] = (char)('0' + (year / 10) % 10);
        title[tp++] = (char)('0' + year % 10);
        title[tp++] = '/';
        title[tp++] = (char)('0' + (month / 10) % 10);
        title[tp++] = (char)('0' + month % 10);
        title[tp] = '\0';
        draw_text_small(x, y, title, fg, bg);
        y += 18;
        draw_text_small(x, y,
                        L("Su Mo Tu We Th Fr Sa",
                          "日 月 火 水 木 金 土"),
                        col_dim(), bg);
        y += 16;
        int first = rtc_weekday(year, month, 1);
        int days = rtc_days_in_month(year, month);
        int cell = 0;
        for (int d = 1; d <= days; d++) {
            int pos = first + d - 1;
            int cx = (int)x + (pos % 7) * 36;
            int cy = (int)y + (pos / 7) * 16;
            char db[4];
            db[0] = d >= 10 ? (char)('0' + d / 10) : ' ';
            db[1] = (char)('0' + d % 10);
            db[2] = '\0';
            if (d == t.day) {
                fb_fill_rect((uint64_t)cx - 2, (uint64_t)cy, 24,
                             15, acc);
                fb_draw_text((uint64_t)cx, (uint64_t)cy, db,
                             rgb(0xFFFFFF), acc, 1);
            } else {
                fb_draw_text((uint64_t)cx, (uint64_t)cy, db, fg,
                             bg, 1);
            }
            cell = pos;
        }
        y += (uint64_t)((cell / 7 + 1) * 16) + 10;
    }
    /* 天気。 */
    draw_text_small(x, y, L("Weather", "天気"), acc, bg);
    y += 18;
    if (widgets_weather_state == 0) {
        draw_text_small(x, y,
                        L("(feed URL not set in Settings)",
                          "(設定でフィードURLが未設定です)"),
                        col_dim(), bg);
        y += 16;
    } else if (widgets_weather_state < 0) {
        draw_text_small(x, y,
                        L("(could not fetch the feed)",
                          "(フィードを取得できませんでした)"),
                        col_dim(), bg);
        y += 16;
    } else {
        for (int i = 0; i < widgets_weather_count; i++) {
            draw_text_small(x, y, widgets_weather[i], fg, bg);
            y += 16;
        }
    }
    y += 8;
    /* ニュース。 */
    draw_text_small(x, y, L("News", "ニュース"), acc, bg);
    y += 18;
    if (widgets_news_state == 0) {
        draw_text_small(x, y,
                        L("(feed URL not set in Settings)",
                          "(設定でフィードURLが未設定です)"),
                        col_dim(), bg);
        y += 16;
    } else if (widgets_news_state < 0) {
        draw_text_small(x, y,
                        L("(could not fetch the feed)",
                          "(フィードを取得できませんでした)"),
                        col_dim(), bg);
        y += 16;
    } else {
        for (int i = 0; i < widgets_news_count; i++) {
            draw_text_small(x, y, widgets_news[i], fg, bg);
            y += 16;
        }
    }
    draw_text_small(x, py + ph - 20,
                    L("R = refresh  Esc = close",
                      "R = 更新  Esc = 閉じる"),
                    col_dim(), bg);
}

static void draw_launcher(void) {
    if (!launcher_active) {
        return;
    }
    int pw, ph, px, py;
    launcher_geometry(&pw, &ph, &px, &py);
    fb_fill_rect((uint64_t)px, (uint64_t)py, (uint64_t)pw, (uint64_t)ph,
                 col_taskbar());
    draw_text_small((uint64_t)(px + LAUNCH_PAD), (uint64_t)(py + 10),
                    L("Apps", "アプリ"), col_accent(),
                    col_taskbar());
    int n = launcher_entries();
    for (int i = 0; i < n; i++) {
        int col = i % LAUNCH_COLS;
        int row = i / LAUNCH_COLS;
        int64_t cx = px + LAUNCH_PAD + col * (LAUNCH_CELL_W + LAUNCH_GAP);
        int64_t cy = py + LAUNCH_PAD + 28 +
                     row * (LAUNCH_CELL_H + LAUNCH_GAP);
        uint32_t fill = i == launcher_selected ? col_accent()
                                               : col_window_bg();
        uint32_t label = i == launcher_selected ? rgb(0xFFFFFF)
                                                : col_text();
        fb_fill_rect((uint64_t)cx, (uint64_t)cy, LAUNCH_CELL_W,
                     LAUNCH_CELL_H, fill);
        desktop_mode app;
        const char *name = launcher_label(i, &app);
        int apps = desk_visible_count();
        draw_app_icon(i < apps ? app : (desktop_mode)DESK_APP_COUNT,
                      (uint64_t)(cx + 7), (uint64_t)(cy + 7), 2,
                      fill);
        int64_t tx = cx + 46;
        draw_text_small((uint64_t)tx, (uint64_t)(cy + 19), name, label,
                        fill);
    }
}

static int launcher_cell_at(int x, int y) {
    int pw, ph, px, py;
    launcher_geometry(&pw, &ph, &px, &py);
    int n = launcher_entries();
    for (int i = 0; i < n; i++) {
        int col = i % LAUNCH_COLS;
        int row = i / LAUNCH_COLS;
        int64_t cx = px + LAUNCH_PAD + col * (LAUNCH_CELL_W + LAUNCH_GAP);
        int64_t cy = py + LAUNCH_PAD + 28 +
                     row * (LAUNCH_CELL_H + LAUNCH_GAP);
        if (x >= cx && x < cx + LAUNCH_CELL_W && y >= cy &&
            y < cy + LAUNCH_CELL_H) {
            return i;
        }
    }
    return -1;
}

/* ================= Context menu ================= */

#define MENU_W 176
#define MENU_ROW_H 24
#define MENU_PAD 6

/* 右クリックメニューの文脈 (kind 1 = Files)。
 * menu_ctx: 0=ファイル, 1=フォルダ行, 2=ゴミ箱内。 */
static int menu_ctx;

static int menu_item_count(void) {
    if (menu_kind == 3) {
        return 2; /* ランチャー: 開く / アンインストール */
    }
    if (menu_kind == 1) {
        if (menu_ctx == 2) {
            return 3;
        }
        if (menu_ctx == 1) {
            return 3;
        }
        return 7;
    }
    return 3;
}

static const char *menu_item_label(int kind, int idx) {
    if (kind == 3) {
        if (desk_settings.language) {
            static const char *const items[2] = {
                "開く", "アンインストール"};
            return items[idx];
        }
        static const char *const items[2] = {"Open", "Uninstall"};
        return items[idx];
    }
    if (kind == 1) {
        if (menu_ctx == 2) {
            if (desk_settings.language) {
                static const char *const items[3] = {
                    "元に戻す", "完全に削除", "ゴミ箱を空にする"};
                return items[idx];
            }
            static const char *const items[3] = {
                "Restore", "Delete permanently", "Empty trash"};
            return items[idx];
        }
        if (menu_ctx == 1) {
            if (desk_settings.language) {
                static const char *const items[3] = {
                    "開く", "新規ファイル", "新規フォルダ"};
                return items[idx];
            }
            static const char *const items[3] = {
                "Open", "New file", "New folder"};
            return items[idx];
        }
        if (desk_settings.language) {
            static const char *const items[7] = {
                "開く", "名前をコピー", "コピー", "名前を変更",
                "新規ファイル", "新規フォルダ", "削除"};
            return items[idx];
        }
        static const char *const items[7] = {
            "Open", "Copy name", "Copy file", "Rename",
            "New file", "New folder", "Delete"};
        return items[idx];
    }
    if (desk_settings.language) {
        static const char *const items[3] = {
            "設定", "テーマ切替", "ターミナル"};
        return items[idx];
    }
    static const char *const items[3] = {"Settings", "Toggle theme",
                                         "Terminal"};
    return items[idx];
}

static void menu_open(int kind, int x, int y, int file_row) {
    menu_active = 1;
    menu_kind = kind;
    menu_file_row = file_row;
    menu_ctx = 0;
    if (kind == 1) {
        files_build_rows();
        if (files_in_trash()) {
            menu_ctx = 2;
        } else if (file_row >= 0 && file_row < files_row_count &&
                   files_rows[file_row] <= -2) {
            menu_ctx = 1;
        }
    }
    int mh = MENU_PAD * 2 + menu_item_count() * MENU_ROW_H;
    if (x + MENU_W > (int)fb_width()) {
        x = (int)fb_width() - MENU_W - 4;
    }
    if (y + mh > (int)fb_height()) {
        y = (int)fb_height() - mh - 4;
    }
    menu_x = x;
    menu_y = y;
    serial_write_line(kind == 1 ? "menu: files"
                                : (kind == 3 ? "menu: launcher"
                                             : "menu: desktop"));
    desktop_redraw();
}

static void menu_activate(int idx) {
    serial_write("menu: ");
    serial_write_line(menu_item_label(menu_kind, idx));
    int kind = menu_kind;
    int row = menu_file_row;
    menu_active = 0;
    if (kind == 3) {
        /* ランチャーのアプリセル (row = セル番号)。 */
        if (row < 0 || row >= desk_visible_count()) {
            desktop_redraw();
            return;
        }
        desktop_mode m = desk_visible_mode(row);
        if (idx == 0) {
            launcher_activate(row);
            return;
        }
        if (app_set_hidden(m, 1) == 0) {
            desktop_notify(L("App uninstalled",
                             "アプリをアンインストールしました"));
        } else {
            desktop_notify(L("This app cannot be removed",
                             "このアプリは外せません"));
        }
        launcher_active = 0;
        if (desk_mode == m) {
            switch_mode(DESK_TERMINAL);
        }
        desktop_redraw();
        return;
    }
    if (kind == 1) {
        files_build_rows();
        if (row < 0 || row >= files_row_count) {
            desktop_redraw();
            return;
        }
        int vrow = files_rows[row];
        if (menu_ctx == 2) {
            /* ゴミ箱内: 復元 / 完全削除 / 空にする。 */
            if (idx == 2) {
                files_confirm = 1;
                desktop_redraw();
                return;
            }
            if (vrow < 0) {
                desktop_redraw();
                return;
            }
            const char *full = vfs_file_name(vrow);
            const char *base = full;
            if (full[0] == 't' && full[1] == 'r' && full[2] == 'a' &&
                full[3] == 's' && full[4] == 'h' && full[5] == '/') {
                base = full + 6;
            }
            if (idx == 0) {
                char btmp[28];
                size_t bp = 0;
                while (base[bp] != '\0' && bp + 1 < sizeof(btmp)) {
                    btmp[bp] = base[bp];
                    bp++;
                }
                btmp[bp] = '\0';
                if (files_trash_restore(btmp) == 0) {
                    desktop_notify(L("Restored", "元に戻しました"));
                } else {
                    desktop_notify(L("Could not restore",
                         "戻せませんでした (同名あり?)"));
                }
            } else if (idx == 1) {
                files_confirm = 2;
                size_t cp = 0;
                while (full[cp] != '\0' &&
                       cp + 1 < sizeof(files_confirm_name)) {
                    files_confirm_name[cp] = full[cp];
                    cp++;
                }
                files_confirm_name[cp] = '\0';
            }
            files_build_rows();
            desktop_redraw();
            return;
        }
        if (vrow < 0) {
            /* ディレクトリ行: 開く / 新規ファイル / 新規フォルダ。 */
            files_selected = row;
            if (idx == 0) {
                files_activate_row();
            } else if (idx == 1) {
                files_prompt_start(2, NULL);
            } else if (idx == 2) {
                files_prompt_start(3, NULL);
            }
            desktop_redraw();
            return;
        }
        const char *name = vfs_file_name(vrow);
        const char *base = name;
        for (const char *bp2 = name; *bp2 != '\0'; bp2++) {
            if (*bp2 == '/') {
                base = bp2 + 1;
            }
        }
        files_selected = row;
        if (idx == 0) {
            files_viewing = vrow;
        } else if (idx == 1) {
            desktop_clipboard_set(name, strlen(name));
            desktop_notify(L("Name copied", "名前をコピーしました"));
        } else if (idx == 2) {
            files_prompt_start(4, base);
        } else if (idx == 3) {
            files_prompt_start(1, base);
        } else if (idx == 4) {
            files_prompt_start(2, NULL);
        } else if (idx == 5) {
            files_prompt_start(3, NULL);
        } else if (idx == 6) {
            char tmp[32];
            size_t p = 0;
            while (name[p] != '\0' && p + 1 < sizeof(tmp)) {
                tmp[p] = name[p];
                p++;
            }
            tmp[p] = '\0';
            if (files_trash_file(tmp) == 0) {
                desktop_notify(L("Moved to trash", "ゴミ箱へ移動しました"));
            } else {
                desktop_notify(L("Could not trash it",
                     "ゴミ箱へ移動できませんでした"));
            }
            files_build_rows();
        }
        desktop_redraw();
        return;
    }
    if (idx == 0) {
        switch_mode(DESK_SETTINGS);
    } else if (idx == 1) {
        desk_settings.dark_mode = !desk_settings.dark_mode;
        settings_autosave();
        desktop_notify(desk_settings.dark_mode
                           ? L("Dark theme", "ダークテーマ")
                           : L("Light theme", "ライトテーマ"));
        desktop_redraw();
    } else {
        switch_mode(DESK_TERMINAL);
    }
}

static void draw_menu(void) {
    if (!menu_active) {
        return;
    }
    int mh = MENU_PAD * 2 + menu_item_count() * MENU_ROW_H;
    fb_fill_rect((uint64_t)menu_x, (uint64_t)menu_y, MENU_W,
                 (uint64_t)mh, col_taskbar());
    for (int px = menu_x; px < menu_x + MENU_W; px++) {
        fb_put_pixel((uint64_t)px, (uint64_t)menu_y, col_accent());
        fb_put_pixel((uint64_t)px, (uint64_t)(menu_y + mh - 1),
                     col_accent());
    }
    for (int i = 0; i < menu_item_count(); i++) {
        draw_text_small((uint64_t)(menu_x + 12),
                        (uint64_t)(menu_y + MENU_PAD + i * MENU_ROW_H + 8),
                        menu_item_label(menu_kind, i), col_text(),
                        col_taskbar());
    }
}

static int menu_item_at(int x, int y) {
    if (x < menu_x || x >= menu_x + MENU_W) {
        return -1;
    }
    int rel = y - menu_y - MENU_PAD;
    if (rel < 0) {
        return -1;
    }
    int idx = rel / MENU_ROW_H;
    if (idx >= menu_item_count()) {
        return -1;
    }
    return idx;
}

/* Everything drawn above the app window and the Dock:
 * floating windows, the launcher, the context menu and the
 * notification toasts. */
static void draw_drag_chip(void);

static void draw_overlays(void) {
    draw_windows();
    draw_launcher();
    draw_widgets();
    draw_menu();
    draw_notifications();
    draw_osk();
    draw_narrator_caption();
    draw_drag_chip();
}

/* アプリのフォルトを封じ込めた後の共通処理: アプリを
 * 閉じて (ターミナルに戻し) 通知を出し、監査に残す。 */
static void desk_contain_fault(const char *where) {
    serial_write("[appguard] contained a fault in a built-in app (");
    serial_write(where);
    serial_write_line("); the app was closed");
    audit_log_event("app-crash", where);
    desk_mode = DESK_TERMINAL;
    const char *msg =
        L("The app crashed and was closed.",
          "アプリが異常終了したため閉じました。");
    size_t p = 0;
    while (msg[p] != '\0' && p + 1 < sizeof(desk_notice)) {
        desk_notice[p] = msg[p];
        p++;
    }
    desk_notice[p] = '\0';
}

/* アプリ描画の武装つき呼び出し。 */
static void draw_window_guarded(void) {
    if (appguard_enter() != 0) {
        desk_contain_fault("draw");
        return;
    }
    draw_window();
    appguard_leave();
}

void desktop_redraw(void) {
    if (!desk_ok) {
        return;
    }
    if (desk_locked) {
        draw_lock_screen();
        fb_flush();
        return;
    }
    draw_wallpaper();
    draw_window_guarded();
    /* 許可リストのお知らせ (あれば、画面の下の行に出す)。 */
    if (desk_notice[0] != '\0') {
        draw_text_small(text_x(),
                        text_y() +
                            (uint64_t)((text_rows() - 1) * CELL_H),
                        desk_notice, col_accent(), col_window_bg());
    }
    if (dock_visible_now()) {
        draw_dock();
    }
    draw_taskbar();
    draw_overlays();
    draw_cursor();
}

void desktop_refresh_console(void) {
    if (!desk_ok) {
        return;
    }
    if (desk_locked) {
        draw_lock_screen();
        fb_flush();
        return;
    }
    draw_window_guarded();
    if (dock_visible_now()) {
        draw_dock();
    }
    draw_taskbar();
    draw_overlays();
    draw_cursor();
}

void desktop_tick(void) {
    if (!desk_ok) {
        return;
    }
    tick_counter++;
    /* Shooter enemy AI: once per RTC second, while playing.
     * Checked every 1024 idle-loop iterations (far more often
     * than once a second, so the second change is never missed). */
    if ((tick_counter & 0x3FF) == 0 && desk_mode == DESK_SHOOTER &&
        shooter_game_state.phase == 1) {
        prism_rtc_time st;
        if (rtc_read(&st) == 0 && st.second != shooter_last_ai_second) {
            shooter_last_ai_second = st.second;
            int hp_before = shooter_game_state.health;
            shooter_enemy_tick(&shooter_game_state, st.second);
            if (shooter_game_state.health < hp_before) {
                serial_write("shooter: hit by enemy, health=");
                shooter_serial_num(shooter_game_state.health);
                serial_write_line("");
            }
            if (shooter_game_state.phase == 3) {
                serial_write_line("shooter: game over");
            }
            desktop_redraw();
        }
    }
    /* Photos のスライドショー: 3 秒ごとに次の写真へ。 */
    if ((tick_counter & 0x3FF) == 0 && desk_mode == DESK_PHOTOS &&
        photos_slideshow && photos_viewing >= 0) {
        prism_rtc_time pt;
        if (rtc_read(&pt) == 0 && pt.second != photos_slide_second) {
            photos_slide_second = pt.second;
            photos_slide_count++;
            if (photos_slide_count >= 3) {
                photos_slide_count = 0;
                int list[PRISM_VFS_MAX_FILES];
                int count = photos_list(list, PRISM_VFS_MAX_FILES);
                if (count > 0) {
                    photos_viewing =
                        (photos_viewing + 1) % count;
                    photos_selected = photos_viewing;
                    photos_zoom = 0;
                    photos_rot = 0;
                    serial_write_line("photos: slideshow next");
                    desktop_redraw();
                }
            }
        }
    }
    /* ダブルクリック判定用に、RTC の秒変化から
     * tick_counter の 1 秒あたりの進みを較正する
     * (ビデオ再生の較正と同じ方法)。 */
    if ((tick_counter & 0x3FF) == 0) {
        prism_rtc_time ct;
        if (rtc_read(&ct) == 0 && ct.second != desk_cal_second) {
            if (desk_cal_second >= 0) {
                desk_ticks_per_sec =
                    tick_counter - desk_tick_at_last_second;
            }
            desk_cal_second = ct.second;
            desk_tick_at_last_second = tick_counter;
        }
    }
    /* Video playback: calibrate the loop rate from the RTC's
     * second changes, then advance the frame at 4 fps. */
    if ((tick_counter & 0x3FF) == 0 && desk_mode == DESK_VIDEO) {
        prism_rtc_time vt;
        if (rtc_read(&vt) == 0 && vt.second != video_last_second) {
            if (video_last_second >= 0) {
                video_ticks_per_sec =
                    (int64_t)(tick_counter - video_tick_at_last_second);
                video_calibrated = 1;
                if (video_player_state.playing) {
                    video_arm_next_tick();
                }
            }
            video_last_second = vt.second;
            video_tick_at_last_second = (int64_t)tick_counter;
        }
        if (video_calibrated &&
            video_tick(&video_player_state, (int64_t)tick_counter,
                       &video_next_tick,
                       video_ticks_per_frame(video_ticks_per_sec))) {
            desktop_redraw();
            if (video_play_frame_audio()) {
                /* 音声がフレームの表示時間そのものなので、
                 * 次のフレームは音声が終わり次第進める。 */
                video_next_tick = (int64_t)tick_counter;
            }
        }
    }
    if ((tick_counter & 0xFFF) != 0) {
        return;
    }
    prism_rtc_time t;
    if (rtc_read(&t) != 0) {
        return;
    }
    if (t.second == last_clock_second) {
        return;
    }
    last_clock_second = t.second;
    rtc_format_hhmm(&t, clock_text);
    clock_update_date(&t);
    int notify_changed = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (notify_ttl[i] > 0) {
            notify_ttl[i]--;
            if (notify_ttl[i] == 0) {
                notify_changed = 1;
            }
        }
    }
    int any_win = 0;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (win_open[i]) {
            any_win = 1;
        }
    }
    if (notify_changed || (any_win && desk_mode == DESK_TERMINAL)) {
        desktop_redraw();
    } else {
        draw_taskbar();
        draw_cursor();
    }
}

void desktop_init(void) {
    if (!fb_ready()) {
        return;
    }
    settings_defaults(&desk_settings);
    /* Load saved settings, if a previous run in this boot saved
     * them to the file system. The file system lives in RAM,
     * so this only survives within one boot. */
    size_t len = 0;
    const char *saved = vfs_read_ptr("settings.txt", &len);
    if (saved != NULL && len > 0) {
        settings_parse(&desk_settings, saved, len);
    }
    sb16_set_volume(desk_settings.volume);
    mouse_set_speed(desk_settings.mouse_speed);
    keyboard_set_repeat(desk_settings.key_repeat);
    /* 通知履歴を前回起動から引き継ぐ。 */
    notify_hist_load();
    /* 保存された解像度があれば適用する (res コマンド /
     * 設定画面の解像度切替の永続化)。失敗したら既定の
     * モードのままにする。 */
    if (desk_settings.res_w > 0 && desk_settings.res_h > 0) {
        if (vbe_set_mode((uint32_t)desk_settings.res_w,
                         (uint32_t)desk_settings.res_h) == 0) {
            mouse_set_bounds(desk_settings.res_w,
                             desk_settings.res_h);
            mouse_set_position(desk_settings.res_w / 2,
                               desk_settings.res_h / 2);
            serial_write("[boot] desktop: resolution restored ");
            serial_write_num((uint64_t)desk_settings.res_w);
            serial_write("x");
            serial_write_num((uint64_t)desk_settings.res_h);
            serial_write_line("");
        }
    }
    /* 許可リストを読む。止められたアプリがあれば報告する。 */
    desk_load_allowlist();
    {
        int denied = 0;
        for (int i = 0; i < SEC_APP_COUNT; i++) {
            if (!desk_allowed[i]) {
                denied++;
            }
        }
        if (denied > 0) {
            serial_write("[boot] desktop: allowlist denies ");
            {
                char num[4];
                int v = denied;
                int d = 0;
                char tmp[4];
                if (v == 0) {
                    tmp[d++] = '0';
                } else {
                    while (v > 0) {
                        tmp[d++] = (char)('0' + (v % 10));
                        v /= 10;
                    }
                }
                int q = 0;
                while (d > 0) {
                    num[q++] = tmp[--d];
                }
                num[q] = '\0';
                serial_write(num);
            }
            serial_write_line(" app(s).");
        }
    }
    if (desk_settings.install_profile) {
        serial_write_line(
            "[boot] desktop: minimal profile (4 apps).");
    }
    /* グリッドは標準セル (12x16) の寸法で確保する。
     * 起動時の文字サイズが大きくても、後から標準へ
     * 戻したときにグリッドが不足しないようにするため。 */
    {
        uint64_t w = win_w();
        uint64_t h = win_h();
        int max_cols = w >= 16 ? (int)((w - 16) / 12) : 0;
        int max_rows =
            h >= TITLE_H + 16 ? (int)((h - TITLE_H - 16) / 16) : 0;
        console_cells = max_cols * max_rows;
    }
    console_cols = text_cols();
    console_rows = text_rows();
    console_col = 0;
    console_row = 0;
    console_grid = NULL;
    console_ok = 0;
    if (console_cols > 0 && console_rows > 0 && console_cells > 0) {
        console_grid = (uint32_t *)kmalloc(
            (size_t)console_cells * sizeof(uint32_t));
        if (console_grid != NULL) {
            for (int i = 0; i < console_cells; i++) {
                console_grid[i] = ' ';
            }
            console_ok = 1;
            console_hist = (uint32_t *)kmalloc(
                (size_t)CONSOLE_HIST_LINES *
                (size_t)console_cols * sizeof(uint32_t));
            if (console_hist != NULL) {
                console_hist_cols = console_cols;
                for (int i = 0; i < CONSOLE_HIST_LINES * console_cols;
                     i++) {
                    console_hist[i] = ' ';
                }
            }
        }
    }
    desk_ok = 1;
    /* Parse the embedded wallpaper image once. */
    wallpaper_ok =
        wallpaper_parse(prism_wallpaper_data,
                        (size_t)(prism_wallpaper_end - prism_wallpaper_data),
                        &wallpaper_image) == 0;
    /* 自分の壁紙が設定されていれば読み込む (設定は復号済みの
     * ファイルから読んだ後なので、この時点で中身が揃う)。 */
    wp_custom_load();
    search_active = 0;
    search_count = 0;
    search_selected = 0;
    search_len = 0;
    search_query[0] = '\0';
    /* The mouse cursor starts in the middle of the screen. */
    mouse_set_bounds((int)fb_width(), (int)fb_height());
    mouse_x = (int)(fb_width() / 2);
    mouse_y = (int)(fb_height() / 2);
    mouse_prev_buttons = 0;
    mouse_set_position(mouse_x, mouse_y);
    files_selected = 0;
    files_viewing = -1;
    photos_selected = 0;
    photos_viewing = -1;
    music_selected = 0;
    music_checked = 0;
    music_has_device = 0;
    music_state = 0;
    cards_phase = 0;
    cards_game = 0;
    cards_wins = 0;
    cards_losses = 0;
    cards_pushes = 0;
    cards_player_n = 0;
    cards_dealer_n = 0;
    shooter_new_game(&shooter_game_state);
    shooter_hitmark = 0;
    shooter_last_ai_second = -1;
    sheet_clear(&sheet_grid);
    sheet_sel_col = 0;
    sheet_sel_row = 0;
    sheet_edit_len = 0;
    sheet_editing = 0;
    sheet_loaded = 0;
    sheet_dirty = 0;
    video_init(&video_player_state, 0);
    video_frame_count = 0;
    video_next_tick = 0;
    video_ticks_per_sec = 0;
    video_tick_at_last_second = 0;
    video_last_second = -1;
    video_calibrated = 0;
    video_is_pv1 = 0;
    video_decoded_frame = -1;
    video_pv1_w = 0;
    video_pv1_h = 0;
    settings_saved = 0;
    browser_state = 0;
    browser_status = 0;
    browser_scroll = 0;
    notes_buf = NULL;
    notes_len = 0;
    notes_cursor = 0;
    notes_loaded = 0;
    notes_dirty = 0;
    notes_saved = 0;
    cal_offset = 0;
    prism_rtc_time t;
    if (rtc_read(&t) == 0) {
        last_clock_second = t.second;
        rtc_format_hhmm(&t, clock_text);
        clock_update_date(&t);
    }
    desktop_redraw();
    serial_write_line("[boot] desktop: ready (screen apps on framebuffer).");
}

int desktop_ready(void) {
    return desk_ok;
}

static void console_scroll(void) {
    if (!console_ok) {
        return;
    }
    /* 流れ出る最上行を履歴へ残す。 */
    if (console_hist != NULL && console_hist_cols == console_cols) {
        int slot = console_hist_count % CONSOLE_HIST_LINES;
        for (int i = 0; i < console_cols; i++) {
            console_hist[slot * console_cols + i] =
                console_grid[i];
        }
        console_hist_count++;
    }
    size_t row_cells = (size_t)console_cols;
    memmove(console_grid, console_grid + row_cells,
            row_cells * (size_t)(console_rows - 1) * sizeof(uint32_t));
    for (int i = 0; i < console_cols; i++) {
        console_grid[(console_rows - 1) * console_cols + i] = ' ';
    }
    console_row = console_rows - 1;
}

static void console_next_row(void) {
    console_col = 0;
    console_row++;
    if (console_row >= console_rows) {
        console_scroll();
    }
}

static void console_place(uint32_t cp) {
    int wide = jfont_is_wide(cp);
    if (console_col + (wide ? 2 : 1) > console_cols) {
        console_next_row();
    }
    console_grid[console_row * console_cols + console_col] = cp;
    console_col++;
    if (wide) {
        console_grid[console_row * console_cols + console_col] =
            CONSOLE_WIDE_TAIL;
        console_col++;
    }
    if (console_col >= console_cols) {
        console_next_row();
    }
}

void desktop_console_putc(int c) {
    if (!console_ok) {
        return;
    }
    /* 新しい出力が来たら表示は最下部へ戻る。 */
    console_view_offset = 0;
    unsigned char b = (unsigned char)c;
    if (b == '\r') {
        console_pending_len = 0;
        console_col = 0;
        return;
    }
    if (b == '\n') {
        console_pending_len = 0;
        console_next_row();
        return;
    }
    if (b == '\b') {
        console_pending_len = 0;
        if (console_col > 0) {
            console_col--;
            if (console_grid[console_row * console_cols + console_col] ==
                CONSOLE_WIDE_TAIL) {
                console_grid[console_row * console_cols + console_col] =
                    ' ';
                if (console_col > 0) {
                    console_col--;
                }
            }
            console_grid[console_row * console_cols + console_col] = ' ';
        }
        return;
    }
    if (console_pending_len == 0) {
        if (b < 32 || b == 127) {
            return;
        }
        if (b < 0x80) {
            console_place(b);
            return;
        }
        if (b >= 0xC2 && b <= 0xDF) {
            console_pending_need = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            console_pending_need = 3;
        } else if (b >= 0xF0 && b <= 0xF4) {
            console_pending_need = 4;
        } else {
            console_place(0xFFFD);
            return;
        }
        console_pending[0] = b;
        console_pending_len = 1;
        return;
    }
    /* Continuation byte of a pending sequence. */
    if ((b & 0xC0) != 0x80) {
        console_pending_len = 0;
        console_place(0xFFFD);
        return;
    }
    console_pending[console_pending_len++] = b;
    if (console_pending_len >= console_pending_need) {
        uint32_t cp = 0xFFFD;
        jfont_utf8_decode((const char *)console_pending,
                          (size_t)console_pending_len, &cp);
        console_pending_len = 0;
        console_place(cp);
    }
}

void desktop_console_write(const char *s) {
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        desktop_console_putc((unsigned char)*s);
        s++;
    }
}

void desktop_console_clear(void) {
    if (!console_ok) {
        return;
    }
    for (int i = 0; i < console_cols * console_rows; i++) {
        console_grid[i] = ' ';
    }
    console_col = 0;
    console_row = 0;
    desktop_redraw();
}

static void switch_mode(desktop_mode m) {
    /* 許可リストで止められたアプリは開かない。 */
    if (!sec_app_allowed(desk_allowed, (int)m)) {
        serial_write("desktop: app '");
        serial_write(mode_name(m));
        serial_write_line("' is not allowed by the administrator.");
        size_t p = 0;
        const char *a = L("App '", "アプリ「");
        while (*a != '\0' && p + 1 < sizeof(desk_notice)) {
            desk_notice[p++] = *a++;
        }
        const char *nm = mode_name(m);
        while (*nm != '\0' && p + 1 < sizeof(desk_notice)) {
            desk_notice[p++] = *nm++;
        }
        const char *b = L("' is not allowed.", "」は許可されていません");
        while (*b != '\0' && p + 1 < sizeof(desk_notice)) {
            desk_notice[p++] = *b++;
        }
        desk_notice[p] = '\0';
        desktop_redraw();
        return;
    }
    desk_notice[0] = '\0';
    desk_mode = m;
    narrator_say(mode_name(m));
    files_viewing = -1;
    settings_saved = 0;
    if (m == DESK_INSTALLER) {
        installer_refresh();
    }
    if (m == DESK_MAIL) {
        mail_rebuild();
        if (!mail_configured()) {
            /* 未設定なら初回設定フォームに誘導する。 */
            mail_setup_open();
        }
    }
    if (m == DESK_SETTINGS) {
        settings_gui_enter();
    }
    if (m == DESK_NOTIF) {
        notif_scroll = 0;
        serial_write("notiflog: opened, ");
        serial_write_num(notify_hist_count);
        serial_write_line(" entries");
    }
    desktop_redraw();
}

/* Which Dock button (0-15) is at (x, y), or -1. The geometry
 * mirrors draw_dock(). */
static int dock_button_at(int x, int y) {
    if (!dock_visible_now()) {
        return -1;
    }
    const int bw = dock_btn_w();
    const int bh = dock_btn_h();
    const int gap = 4;
    int64_t start = 0;
    int64_t cross = 0;
    int count = dock_layout(&start, &cross);
    for (int i = 0; i < count; i++) {
        int64_t bx = desk_settings.dock_pos == 1 ? cross
                                                 : start + i * (bw + gap);
        int64_t by = desk_settings.dock_pos == 1
                         ? start + i * (bh + gap)
                         : cross;
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            return i;
        }
    }
    return -1;
}
/* A left click at screen position (x, y). */
/* 設定アプリのマウス操作 (項目10)。行クリックで
 * 選択し、選択中の行の再クリックは Enter と同じ
 * (決定/切り替え)。見た目ページは項目キーそのものを
 * 実行する。他アプリと同じく、実際の処理はキー処理に
 * 集約されているので、ここは行→キーの対応だけを持つ。 */
static void settings_mouse_click(int x, int y) {
    if (pin_dialog_open) {
        return;
    }
    if (y < (int)text_y()) {
        return;
    }
    int row = (int)((y - (int)text_y()) / CELL_H);
    if (settings_page == 0) {
        int key = 0;
        if (row >= 2 && row <= 5) {
            int idx = (row - 2) * 2 +
                      (x >= (int)(text_x() + win_w() / 2) ? 1 : 0);
            if (idx < PRISM_ACCENT_COUNT) {
                key = '1' + idx;
            }
        } else {
            switch (row) {
                case 6: key = 'k'; break;
                case 7: key = 't'; break;
                case 8: key = 'm'; break;
                case 9: key = 'w'; break;
                case 14: key = 'p'; break;
                case 15: key = 'z'; break;
                case 16: key = 'h'; break;
                case 17: key = 'g'; break;
                case 18: key = 'j'; break;
                case 19: key = 'x'; break;
                case 20: key = 'b'; break;
                case 21: key = 'o'; break;
                case 22: key = 'n'; break;
                case 23: key = 'r'; break;
                case 24: key = 'e'; break;
                case 25: key = 'i'; break;
                case 26: key = 'u'; break;
                default: break;
            }
        }
        if (key != 0) {
            desktop_handle_key(key);
        }
        return;
    }
    if (settings_page == 1) {
        if (row == PRISM_BIND_COUNT + 3) {
            desktop_handle_key('0');
            return;
        }
        int idx = row - 2;
        if (idx >= 0 && idx < PRISM_BIND_COUNT) {
            if (idx == bind_selected) {
                desktop_handle_key('\n');
            } else {
                bind_selected = idx;
                desktop_redraw();
            }
        }
        return;
    }
    if (settings_page == 2) {
        if (acct_mode != ACCT_MODE_LIST) {
            return;
        }
        int visible = text_rows() - 3 - 4;
        if (visible < 1) {
            visible = 1;
        }
        int start = 0;
        if (acct_selected >= visible) {
            start = acct_selected - visible + 1;
        }
        int idx = start + (row - 3);
        if (idx >= 0 && idx < acct_user_count + 2 && row >= 3) {
            if (idx == acct_selected) {
                desktop_handle_key('\n');
            } else {
                acct_selected = idx;
                desktop_redraw();
            }
        }
        return;
    }
    if (settings_page == 3) {
        if (row >= 2 && row <= 7) {
            int idx = row - 2;
            if (idx == sec_selected) {
                desktop_handle_key('\n');
            } else {
                sec_selected = idx;
                desktop_redraw();
            }
            return;
        }
        if (row >= 10) {
            int visible = text_rows() - 10 - 4;
            if (visible < 1) {
                visible = 1;
            }
            int app_sel = sec_selected - 6;
            int start = 0;
            if (app_sel >= visible) {
                start = app_sel - visible + 1;
            }
            int idx = 6 + start + (row - 10);
            if (idx >= 6 && idx < 6 + SEC_APP_COUNT) {
                if (idx == sec_selected) {
                    desktop_handle_key('\n');
                } else {
                    sec_selected = idx;
                    desktop_redraw();
                }
            }
        }
        return;
    }
    if (settings_page == 4) {
        int idx = row - 5;
        if (idx >= 0 && idx <= 4) {
            if (idx == upd_selected) {
                desktop_handle_key('\n');
            } else {
                upd_selected = idx;
                desktop_redraw();
            }
        }
        return;
    }
    if (settings_page == 5) {
        int idx = -1;
        if (row == 3 || row == 4) {
            idx = 0;
        } else if (row == 6 || row == 7) {
            idx = 1;
        } else if (row == 9) {
            idx = 2;
        }
        if (idx >= 0) {
            if (idx == clk_row) {
                desktop_handle_key('\n');
            } else {
                clk_row = idx;
                desktop_redraw();
            }
        }
        return;
    }
    if (settings_page == 6) {
        int idx = row - 4;
        if (row == 11 || row == 12) {
            idx = row - 5; /* フィード行: 6 / 7 */
        }
        if (idx >= 0 && idx <= 7) {
            if (idx == net_row) {
                desktop_handle_key('\n');
            } else {
                net_row = idx;
                desktop_redraw();
            }
        }
        return;
    }
    if (settings_page == 7) {
        if (adm_form) {
            return;
        }
        if (row == 2) {
            /* 分類タブの行: クリックで次の分類へ。 */
            adm_section = (adm_section + 1) % 5;
            adm_selected = 0;
            adm_refresh();
            desktop_redraw();
            return;
        }
        /* 内容行は分類ごとに開始行が違うので、代表的な
         * 行だけ対応する (FDE のアクション行など)。
         * それ以外はキーボード操作に委ねる。 */
        if (adm_section == 0 && row >= 4) {
            desktop_handle_key('\n');
        }
        return;
    }
}

static void desktop_mouse_click(int x, int y) {
    /* The on-screen keyboard takes clicks on its panel first:
     * the key goes through the same path as a physical key. */
    if (desk_settings.osk && y >= (int)osk_panel_y()) {
        int code = osk_key_at(x, y);
        if (code >= 0) {
            if (osk_queue_len < OSK_QUEUE_MAX) {
                osk_queue[osk_queue_len++] = code;
            }
            serial_write_line("osk: key pressed");
        }
        return;
    }
    /* Open panels and windows take clicks first. */
    if (menu_active) {
        int idx = menu_item_at(x, y);
        if (idx >= 0) {
            menu_activate(idx);
        } else {
            menu_active = 0;
            desktop_redraw();
        }
        return;
    }
    if (widgets_open) {
        /* パネルの外をクリックしたら閉じる。 */
        if (!(x >= 8 && x < 448 && y >= 56)) {
            widgets_open = 0;
            serial_write_line("widgets: closed");
            desktop_redraw();
        }
        return;
    }
    if (launcher_active) {
        int cell = launcher_cell_at(x, y);
        if (cell >= 0) {
            launcher_activate(cell);
        } else {
            launcher_active = 0;
            desktop_redraw();
        }
        return;
    }
    /* Taskbar Apps button toggles the launcher. */
    if (taskbar_vertical()) {
        uint64_t bx = desk_settings.taskbar_side == 1
                          ? 0
                          : fb_width() - TASKBAR_W;
        if (x >= (int)bx + 10 && x < (int)bx + TASKBAR_W - 10 &&
            y >= 40 && y < 64) {
            launcher_active = !launcher_active;
            launcher_selected = 0;
            serial_write_line(launcher_active ? "launcher: opened"
                                              : "launcher: closed");
            desktop_redraw();
            return;
        }
    }
    {
        uint64_t ty = taskbar_y();
        if (y >= (int)ty + 8 && y < (int)ty + 32 && x >= 104 &&
            x < 164) {
            launcher_active = !launcher_active;
            launcher_selected = 0;
            serial_write_line(launcher_active ? "launcher: opened"
                                              : "launcher: closed");
            desktop_redraw();
            return;
        }
    }
    if (desk_mode == DESK_SETTINGS) {
        settings_mouse_click(x, y);
        return;
    }
    if (desk_mode == DESK_TERMINAL) {
        int w = win_at(x, y);
        if (w >= 0) {
            if (x >= win_px[w] + win_w_of(w) - 24 &&
                y < win_py[w] + 24) {
                win_close(w);
                return;
            }
            win_z[w] = ++win_z_top;
            win_focus = w;
            if (y < win_py[w] + 24) {
                win_drag = w;
                win_drag_dx = x - win_px[w];
                win_drag_dy = y - win_py[w];
            }
            desktop_redraw();
            return;
        }
        if (win_focus >= 0) {
            win_focus = -1; /* clicked the terminal itself */
            desktop_redraw();
        }
    }
    int dock = dock_button_at(x, y);
    if (dock >= 0) {
        desktop_mode app = desk_visible_mode(dock);
        serial_write("desktop: mouse click -> ");
        serial_write_line(mode_name(app));
        switch_mode(app);
        return;
    }
    if (desk_mode == DESK_BROWSER && browser_state == 1) {
        /* Clicking a link line follows it. */
        int64_t row_y = (int64_t)text_y() + 2 * CELL_H;
        if (y >= row_y) {
            int row = (int)((y - row_y) / CELL_H);
            int li = browser_scroll + row;
            if (li >= 0 && li < br_line_count &&
                br_lines[li].link >= 0) {
                br_navigate(br_links[br_lines[li].link]);
            }
        }
        return;
    }
    if (desk_mode == DESK_FILES && files_viewing < 0) {
        files_build_rows();
        int64_t row_y = (int64_t)text_y() + 2 * CELL_H;
        int row = (int)((y - row_y) / CELL_H);
        if (row >= 0 && row < files_row_count &&
            x >= (int)text_x() &&
            x < (int)(text_x() + win_w() - 16)) {
            /* 選択済みの行の再クリックは、ダブルクリック
             * 間隔 (設定値) の中だけ「開く」にする。
             * 較正前は従来どおり常時開く。 */
            unsigned long win_ticks =
                desk_ticks_per_sec *
                (unsigned long)desk_settings.dblclick_ms / 1000;
            if (row == files_selected &&
                row == files_last_click_row &&
                (win_ticks == 0 ||
                 tick_counter - files_last_click_tick <=
                     win_ticks)) {
                files_activate_row();
                files_last_click_row = -1;
            } else {
                files_selected = row;
                files_last_click_row = row;
                files_last_click_tick = tick_counter;
            }
            desktop_refresh_console();
        }
    }
    if (desk_mode == DESK_MAIL && mail_viewing < 0 &&
        !mail_composing) {
        int64_t row_y = (int64_t)text_y() + 2 * CELL_H;
        int row = (int)((y - row_y) / CELL_H);
        if (row >= 0 && row < mail_list_count &&
            x >= (int)text_x() &&
            x < (int)(text_x() + win_w() - 16)) {
            if (row == mail_selected) {
                mail_viewing = row;
                mail_scroll = 0;
            } else {
                mail_selected = row;
            }
            desktop_refresh_console();
        }
    }
    if (desk_mode == DESK_PHOTOS && photos_viewing < 0) {
        int list[PRISM_VFS_MAX_FILES];
        int count = photos_list(list, PRISM_VFS_MAX_FILES);
        int64_t row_y = (int64_t)text_y() + 2 * CELL_H;
        int row = (int)((y - row_y) / CELL_H);
        if (row >= 0 && row < count &&
            x >= (int)text_x() &&
            x < (int)(text_x() + win_w() - 16)) {
            if (row == photos_selected) {
                photos_viewing = row;
            } else {
                photos_selected = row;
            }
            desktop_refresh_console();
        }
    }
}

/* ドラッグの落下処理 (項目18)。 */
static void drag_drop_at(int x, int y) {
    const char *base = drag_name;
    for (const char *p = drag_name; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    if (desk_mode == DESK_FILES && files_viewing < 0) {
        files_build_rows();
        int64_t row_y = (int64_t)text_y() + 2 * CELL_H;
        int row = (int)((y - row_y) / CELL_H);
        if (row >= 0 && row < files_row_count &&
            x >= (int)text_x() &&
            x < (int)(text_x() + win_w() - 16)) {
            int v = files_rows[row];
            if (v <= -2 || v == -1) {
                /* フォルダ行 (または親) へ移動する。
                 * バッファは最長パス (親 31 + '/' + フォルダ名
                 * 27、宛先はさらに '/' + 名前 31) が収まる大きさ
                 * にし、組み立て前に長さを確認して、途中で
                 * 切れた先へは動かさない (監査 L7)。 */
                char dirpath[64];
                size_t dp = 0;
                if (v == -1) {
                    /* 親フォルダ = files_dir の末尾を落とす。 */
                    size_t n = 0;
                    while (files_dir[n] != '\0') {
                        n++;
                    }
                    while (n > 0 && files_dir[n - 1] != '/') {
                        n--;
                    }
                    if (n > 0) {
                        n--;
                    }
                    while (dp < n && dp + 1 < sizeof(dirpath)) {
                        dirpath[dp] = files_dir[dp];
                        dp++;
                    }
                } else {
                    const char *dn = files_dirnames[-2 - v];
                    if (strcmp(dn, "trash") == 0) {
                        if (files_trash_file(drag_name) == 0) {
                            serial_write("drag: trashed ");
                            serial_write_line(drag_name);
                            desktop_notify(L("Moved to trash",
                                             "ゴミ箱へ移動しました"));
                            persist_maybe_sync();
                        } else {
                            serial_write_line("drag: cancelled");
                        }
                        files_build_rows();
                        desktop_redraw();
                        return;
                    }
                    size_t need = strlen(files_dir) +
                                  (files_dir[0] != '\0' ? 1 : 0) +
                                  strlen(dn);
                    if (need + 1 > sizeof(dirpath)) {
                        serial_write_line("drag: cancelled");
                        desktop_redraw();
                        return;
                    }
                    size_t q = 0;
                    while (files_dir[q] != '\0' &&
                           dp + 1 < sizeof(dirpath)) {
                        dirpath[dp++] = files_dir[q++];
                    }
                    if (dp > 0 && dp + 1 < sizeof(dirpath)) {
                        dirpath[dp++] = '/';
                    }
                    q = 0;
                    while (dn[q] != '\0' && dp + 1 < sizeof(dirpath)) {
                        dirpath[dp++] = dn[q++];
                    }
                }
                dirpath[dp] = '\0';
                char dst[96];
                if (strlen(dirpath) + 1 + strlen(base) + 1 >
                    sizeof(dst)) {
                    serial_write_line("drag: cancelled");
                    desktop_redraw();
                    return;
                }
                size_t p = 0;
                while (dirpath[p] != '\0' && p + 1 < sizeof(dst)) {
                    dst[p] = dirpath[p];
                    p++;
                }
                if (p > 0 && p + 1 < sizeof(dst)) {
                    dst[p++] = '/';
                }
                size_t q = 0;
                while (base[q] != '\0' && p + 1 < sizeof(dst)) {
                    dst[p++] = base[q++];
                }
                dst[p] = '\0';
                if (strcmp(dst, drag_name) == 0 ||
                    vfs_exists(dst)) {
                    serial_write_line("drag: cancelled");
                    desktop_redraw();
                    return;
                }
                size_t len = 0;
                const char *content = vfs_read_ptr(drag_name, &len);
                if (content != NULL &&
                    vfs_write(dst, content, len) == 0) {
                    (void)vfs_delete(drag_name);
                    serial_write("drag: moved ");
                    serial_write(drag_name);
                    serial_write(" -> ");
                    serial_write_line(dst);
                    desktop_notify(L("Moved", "移動しました"));
                    persist_maybe_sync();
                } else {
                    serial_write_line("drag: cancelled");
                }
                files_build_rows();
                desktop_redraw();
                return;
            }
        }
    }
    /* Dock の対応アプリで開く。 */
    int di = dock_button_at(x, y);
    if (di >= 0) {
        desktop_mode m = desk_visible_mode(di);
        if (m == DESK_PHOTOS &&
            name_ends_with(drag_name, ".bmp")) {
            int list[PRISM_VFS_MAX_FILES];
            int count = photos_list(list, PRISM_VFS_MAX_FILES);
            for (int i = 0; i < count; i++) {
                if (strcmp(vfs_file_name(list[i]), drag_name) ==
                    0) {
                    photos_viewing = i;
                    photos_selected = i;
                    photos_zoom = 0;
                    photos_rot = 0;
                    switch_mode(DESK_PHOTOS);
                    serial_write("drag: opened ");
                    serial_write_line(drag_name);
                    desktop_redraw();
                    return;
                }
            }
        }
        if (m == DESK_EDITOR) {
            size_t q = 0;
            while (drag_name[q] != '\0' &&
                   q + 1 < sizeof(editor_name)) {
                editor_name[q] = drag_name[q];
                q++;
            }
            editor_name[q] = '\0';
            editor_loaded = 0;
            switch_mode(DESK_EDITOR);
            serial_write("drag: opened ");
            serial_write_line(drag_name);
            desktop_redraw();
            return;
        }
    }
    serial_write_line("drag: cancelled");
    desktop_redraw();
}

/* ドラッグ中のチップ表示。 */
static void draw_drag_chip(void) {
    if (!drag_active) {
        return;
    }
    uint64_t w = 16 + (uint64_t)strlen(drag_name) * 8;
    uint64_t cx = (uint64_t)(mouse_x + 12);
    uint64_t cy = (uint64_t)(mouse_y + 14);
    if (cx + w > fb_width()) {
        cx = fb_width() - w;
    }
    if (cy + 20 > fb_height()) {
        cy = fb_height() - 20;
    }
    fb_fill_rect(cx, cy, w, 20, col_accent());
    fb_draw_text(cx + 6, cy + 2, drag_name, rgb(0xFFFFFF),
                 col_accent(), 1);
}

/* ---- 内蔵アプリのフォルト封じ込め (appguard) ----
 * 入力ディスパッチの武装つきラッパー。封じ込めの共通
 * 処理は desk_contain_fault (描画側と共用)。 */
static void desktop_mouse_poll_inner(void);

void desktop_mouse_poll(void) {
    if (appguard_enter() != 0) {
        desk_contain_fault("mouse");
        return;
    }
    desktop_mouse_poll_inner();
    appguard_leave();
}

static void desktop_mouse_poll_inner(void) {
    if (!desk_ok || desk_locked) {
        return;
    }
    int dx = 0;
    int dy = 0;
    int buttons = 0;
    int have = mouse_poll(&dx, &dy, &buttons);
    if (!have) {
        /* virtio タブレット (絶対座標) が動いていれば、
         * カーソルをその位置へ置いてボタン状態を使う。 */
        int ax = 0;
        int ay = 0;
        int ab = 0;
        if (vinput_poll(&ax, &ay, &ab, (int)fb_width(),
                        (int)fb_height())) {
            mouse_set_position(ax, ay);
            buttons = ab;
            have = 1;
        }
    }
    if (!have) {
        return;
    }
    if (desk_mode == DESK_SHOOTER) {
        /* In the shooter the mouse aims: horizontal movement
         * turns the player, a left click shoots. The cursor
         * itself stays where it is. */
        int steps = dx / 2;
        if (steps > 32) {
            steps = 32;
        }
        if (steps < -32) {
            steps = -32;
        }
        if (steps != 0 && shooter_game_state.phase == 1) {
            shooter_turn(&shooter_game_state, steps);
        }
        int left_pressed = (buttons & 1) && !(mouse_prev_buttons & 1);
        mouse_prev_buttons = buttons;
        if (left_pressed) {
            shooter_fire();
        }
        desktop_redraw();
        return;
    }
    mouse_position(&mouse_x, &mouse_y);
    int left_pressed = (buttons & 1) && !(mouse_prev_buttons & 1);
    int right_pressed = (buttons & 2) && !(mouse_prev_buttons & 2);
    mouse_prev_buttons = buttons;
    if (win_drag >= 0) {
        if (buttons & 1) {
            int nx = mouse_x - win_drag_dx;
            int ny = mouse_y - win_drag_dy;
            if (nx < 0) {
                nx = 0;
            }
            if (ny < 0) {
                ny = 0;
            }
            if (nx > (int)fb_width() - 60) {
                nx = (int)fb_width() - 60;
            }
            if (ny > (int)fb_height() - 40) {
                ny = (int)fb_height() - 40;
            }
            win_px[win_drag] = nx;
            win_py[win_drag] = ny;
        } else {
            win_drag = -1;
        }
    }
    /* ドラッグ&ドロップの開始・追跡・落下 (項目18)。 */
    if (left_pressed && desk_mode == DESK_FILES &&
        files_viewing < 0 && !menu_active && !launcher_active &&
        !widgets_open) {
        files_build_rows();
        int64_t row_y = (int64_t)text_y() + 2 * CELL_H;
        int row = (int)((mouse_y - row_y) / CELL_H);
        if (row >= 0 && row < files_row_count &&
            files_rows[row] >= 0 && mouse_x >= (int)text_x() &&
            mouse_x < (int)(text_x() + win_w() - 16)) {
            const char *nm = vfs_file_name(files_rows[row]);
            if (nm != NULL) {
                size_t q = 0;
                while (nm[q] != '\0' && q + 1 < sizeof(drag_name)) {
                    drag_name[q] = nm[q];
                    q++;
                }
                drag_name[q] = '\0';
                drag_pending = 1;
                drag_start_x = mouse_x;
                drag_start_y = mouse_y;
            }
        }
    }
    if (drag_pending && (buttons & 1)) {
        int dx = mouse_x - drag_start_x;
        int dy = mouse_y - drag_start_y;
        if (dx > 8 || dx < -8 || dy > 8 || dy < -8) {
            drag_pending = 0;
            drag_active = 1;
            serial_write("drag: started ");
            serial_write_line(drag_name);
        }
    }
    if (drag_active && !(buttons & 1)) {
        drag_active = 0;
        drag_drop_at(mouse_x, mouse_y);
    } else if (drag_pending && !(buttons & 1)) {
        drag_pending = 0;
    }
    if (right_pressed) {
        if (menu_active) {
            menu_active = 0;
            desktop_redraw();
        } else if (launcher_active) {
            int cell = launcher_cell_at(mouse_x, mouse_y);
            if (cell >= 0 && cell < desk_visible_count()) {
                menu_open(3, mouse_x, mouse_y, cell);
            } else {
                launcher_active = 0;
                desktop_redraw();
            }
        } else if (desk_mode == DESK_FILES && files_viewing < 0) {
            files_build_rows();
            int64_t row_y = (int64_t)text_y() + 2 * CELL_H;
            int row = (int)((mouse_y - row_y) / CELL_H);
            if (row >= 0 && row < files_row_count &&
                mouse_x >= (int)text_x() &&
                mouse_x < (int)(text_x() + win_w() - 16)) {
                files_selected = row;
                menu_open(1, mouse_x, mouse_y, row);
            } else {
                menu_open(0, mouse_x, mouse_y, -1);
            }
        } else {
            menu_open(0, mouse_x, mouse_y, -1);
        }
    }
    if (left_pressed) {
        desktop_mouse_click(mouse_x, mouse_y);
    }
    desktop_redraw();
}

/* アクション (アプリ起動) を実行する。割り当てキーの判定は
 * desktop_handle_key が行い、ここは起動そのものだけをする。
 * 各アプリの起動手順は従来のキー別処理と同一。 */
static void launch_action(int a) {
    switch (a) {
    case PRISM_ACT_TERMINAL:
        switch_mode(DESK_TERMINAL);
        break;
    case PRISM_ACT_FILES:
        switch_mode(DESK_FILES);
        break;
    case PRISM_ACT_SETTINGS:
        switch_mode(DESK_SETTINGS);
        break;
    case PRISM_ACT_BROWSER:
        switch_mode(DESK_BROWSER);
        break;
    case PRISM_ACT_NOTES:
        if (!desk_mode_visible(DESK_NOTES)) {
            break;
        }
        notes_load();
        switch_mode(DESK_NOTES);
        break;
    case PRISM_ACT_CALENDAR:
        if (!desk_mode_visible(DESK_CALENDAR)) {
            break;
        }
        cal_offset = 0;
        switch_mode(DESK_CALENDAR);
        break;
    case PRISM_ACT_PHOTOS:
        if (!desk_mode_visible(DESK_PHOTOS)) {
            break;
        }
        switch_mode(DESK_PHOTOS);
        break;
    case PRISM_ACT_MUSIC:
        if (!desk_mode_visible(DESK_MUSIC)) {
            break;
        }
        if (!music_checked) {
            music_checked = 1;
            music_has_device = sb16_present();
        }
        switch_mode(DESK_MUSIC);
        break;
    case PRISM_ACT_CARDS:
        if (!desk_mode_visible(DESK_CARDS)) {
            break;
        }
        switch_mode(DESK_CARDS);
        break;
    case PRISM_ACT_SHOOTER:
        if (!desk_mode_visible(DESK_SHOOTER)) {
            break;
        }
        switch_mode(DESK_SHOOTER);
        break;
    case PRISM_ACT_SHEET:
        if (!desk_mode_visible(DESK_SHEET)) {
            break;
        }
        if (!sheet_loaded) {
            sheet_load();
        }
        switch_mode(DESK_SHEET);
        break;
    case PRISM_ACT_VIDEO:
        if (!desk_mode_visible(DESK_VIDEO)) {
            break;
        }
        video_rebuild();
        switch_mode(DESK_VIDEO);
        break;
    case PRISM_ACT_INSTALLER:
        if (!desk_mode_visible(DESK_INSTALLER)) {
            break;
        }
        switch_mode(DESK_INSTALLER);
        break;
    case PRISM_ACT_MAIL:
        if (!desk_mode_visible(DESK_MAIL)) {
            break;
        }
        mail_rebuild();
        switch_mode(DESK_MAIL);
        serial_write_line("mail: opened");
        break;
    case PRISM_ACT_DOCS:
        if (!desk_mode_visible(DESK_DOCS)) {
            break;
        }
        docs_load();
        switch_mode(DESK_DOCS);
        break;
    case PRISM_ACT_SLIDES:
        if (!desk_mode_visible(DESK_SLIDES)) {
            break;
        }
        if (desk_mode == DESK_SLIDES) {
            /* スライド表示中はショーの切り替え (従来の Ctrl+P)。 */
            slides_commit_edit();
            slides_show = !slides_show;
            desktop_redraw();
            break;
        }
        slides_load();
        switch_mode(DESK_SLIDES);
        break;
    case PRISM_ACT_CONTACTS:
        if (!desk_mode_visible(DESK_CONTACTS)) {
            break;
        }
        contacts_status[0] = '\0';
        switch_mode(DESK_CONTACTS);
        break;
    case PRISM_ACT_EDITOR:
        if (!desk_mode_visible(DESK_EDITOR)) {
            break;
        }
        editor_loaded = 0;
        switch_mode(DESK_EDITOR);
        break;
    case PRISM_ACT_PAINT:
        if (!desk_mode_visible(DESK_PAINT)) {
            break;
        }
        switch_mode(DESK_PAINT);
        break;
    case PRISM_ACT_STORE:
        if (!desk_mode_visible(DESK_STORE)) {
            break;
        }
        switch_mode(DESK_STORE);
        serial_write_line("store-ui: opened");
        break;
    default:
        break;
    }
}

/* 音量を変更し、即適用して自動保存する
 * (Music / Video 共用)。 */
static void volume_adjust(int delta) {
    int v = desk_settings.volume + delta;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    desk_settings.volume = v;
    sb16_set_volume(v);
    settings_autosave();
    serial_write("settings: volume ");
    serial_write_num(v);
    serial_write_line("");
    char msg[48];
    size_t p = 0;
    const char *h = L("Volume: ", "音量: ");
    while (*h != '\0' && p + 1 < sizeof(msg)) {
        msg[p++] = *h++;
    }
    char num[8];
    int ni = 0;
    if (v == 0) {
        num[ni++] = '0';
    } else {
        char rev[4];
        int ri = 0;
        int x = v;
        while (x > 0 && ri < 3) {
            rev[ri++] = (char)('0' + x % 10);
            x /= 10;
        }
        while (ri > 0) {
            num[ni++] = rev[--ri];
        }
    }
    num[ni++] = '%';
    num[ni] = '\0';
    for (int k = 0; num[k] != '\0' && p + 1 < sizeof(msg); k++) {
        msg[p++] = num[k];
    }
    msg[p] = '\0';
    desktop_notify(msg);
}

/* メール作成への IME 確定文字列の挿入 (件名と本文
 * のみ。宛先と添付名は ASCII のまま)。 */
static void mail_insert_text(const char *text) {
    if (mail_field == 1) {
        size_t n = 0;
        while (mail_subject[n] != '\0') {
            n++;
        }
        for (size_t i = 0; text[i] != '\0'; i++) {
            if (n + 1 < sizeof(mail_subject)) {
                mail_subject[n++] = text[i];
            }
        }
        mail_subject[n] = '\0';
    } else if (mail_field == 2) {
        for (size_t i = 0; text[i] != '\0'; i++) {
            if (mail_body_len + 1 < sizeof(mail_body)) {
                mail_body[mail_body_len++] = text[i];
                mail_body[mail_body_len] = '\0';
            }
        }
    }
}

/* ---- スクリーンショット ----
 * Ctrl+U で現在の画面を 24 ビット BMP (原寸) に保存する。
 * 1280x800 なら 54 + 1280*800*3 = 3072054 バイト。
 * VFS と diskfs は版5 で 4MB まで扱える。 */
#define SHOT_MAX_W 1280
#define SHOT_MAX_H 800
#define SHOT_BUF_SIZE (54 + (size_t)SHOT_MAX_W * SHOT_MAX_H * 3)
static uint8_t shot_buf[SHOT_BUF_SIZE];

static void desktop_screenshot(void) {
    if (!fb_ready()) {
        return;
    }
    uint64_t fw = fb_width();
    uint64_t fh = fb_height();
    if (fw == 0 || fh == 0 || fw > SHOT_MAX_W || fh > SHOT_MAX_H) {
        desktop_notify(L("Screenshot too large",
                         "画面が大きすぎて保存できません"));
        return;
    }
    char name[16];
    const char *base = "shot.bmp";
    size_t bp = 0;
    while (base[bp] != '\0' && bp + 1 < sizeof(name)) {
        name[bp] = base[bp];
        bp++;
    }
    name[bp] = '\0';
    if (vfs_exists(name)) {
        int found = 0;
        for (int i = 1; i <= 9 && !found; i++) {
            /* "shotN.bmp" の形にする (監査 L4: 以前は index 4
             * の '.' を数字で上書きして "shot1bmp" になり、
             * 拡張子判定に載らなかった)。 */
            name[4] = (char)('0' + i);
            name[5] = '.';
            name[6] = 'b';
            name[7] = 'm';
            name[8] = 'p';
            name[9] = '\0';
            if (!vfs_exists(name)) {
                found = 1;
            }
        }
        if (!found) {
            desktop_notify(L("Too many screenshots",
                             "スクリーンショットが多すぎます"));
            return;
        }
    }
    /* BMP (24 ビット、パレット無し、行は 4 バイト境界)。 */
    uint32_t stride = (uint32_t)((fw * 3 + 3) & ~(uint64_t)3);
    uint32_t total = 54 + stride * (uint32_t)fh;
    uint8_t *b = shot_buf;
    for (size_t i = 0; i < total; i++) {
        b[i] = 0;
    }
    b[0] = 'B';
    b[1] = 'M';
    b[2] = (uint8_t)total;
    b[3] = (uint8_t)(total >> 8);
    b[4] = (uint8_t)(total >> 16);
    b[5] = (uint8_t)(total >> 24);
    b[10] = 54; /* データ開始 */
    b[14] = 40; /* BITMAPINFOHEADER */
    b[18] = (uint8_t)fw;
    b[19] = (uint8_t)(fw >> 8);
    b[20] = (uint8_t)(fw >> 16);
    b[21] = (uint8_t)(fw >> 24);
    b[22] = (uint8_t)fh;
    b[23] = (uint8_t)(fh >> 8);
    b[24] = (uint8_t)(fh >> 16);
    b[25] = (uint8_t)(fh >> 24);
    b[26] = 1;  /* プレーン数 */
    b[28] = 24; /* 24bpp */
    b[34] = (uint8_t)(stride * (uint32_t)fh);
    b[35] = (uint8_t)((stride * (uint32_t)fh) >> 8);
    b[36] = (uint8_t)((stride * (uint32_t)fh) >> 16);
    b[37] = (uint8_t)((stride * (uint32_t)fh) >> 24);
    for (uint64_t y = 0; y < fh; y++) {
        uint64_t sy = fh - 1 - y; /* BMP は下から上へ */
        for (uint64_t x = 0; x < fw; x++) {
            uint32_t rgb = fb_get_pixel_rgb(x, sy);
            size_t off = 54 + (size_t)y * stride + (size_t)x * 3;
            b[off + 0] = (uint8_t)(rgb & 0xFF);        /* B */
            b[off + 1] = (uint8_t)((rgb >> 8) & 0xFF);  /* G */
            b[off + 2] = (uint8_t)((rgb >> 16) & 0xFF); /* R */
        }
    }
    if (vfs_write(name, (const char *)b, total) == 0) {
        persist_maybe_sync();
        serial_write("screenshot: saved ");
        serial_write_line(name);
        desktop_notify(L("Screenshot saved",
                         "スクリーンショットを保存しました"));
    } else {
        desktop_notify(L("Screenshot failed",
                         "スクリーンショットに失敗しました"));
    }
    desktop_redraw();
}

static int desktop_handle_key_inner(int c);

int desktop_handle_key(int c) {
    if (appguard_enter() != 0) {
        desk_contain_fault("key");
        return 1;
    }
    int rc = desktop_handle_key_inner(c);
    appguard_leave();
    return rc;
}

/* 自己テスト: 武装した領域の中でわざとフォルトを起こし、
 * 封じ込めが働くか (例外 → appguard_bail → ここへ戻る)
 * を確かめる。evil.elf がサンドボックスに対して果たす
 * 役割と同じ、封じ込め機構そのものの検証用。 */
void desktop_appguard_selftest(void) {
    if (appguard_enter() != 0) {
        serial_write_line(
            "[appguard] self-test: fault contained, desktop alive");
        return;
    }
    *(volatile uint8_t *)0 = 0x41; /* ページフォルトさせる */
    appguard_leave();
    serial_write_line(
        "[appguard] self-test: NO fault occurred (unexpected)");
}

static int desktop_handle_key_inner(int c) {
    if (!desk_ok) {
        return 0;
    }
    /* ロック中は解除キーだけを受ける。 */
    if (desk_locked) {
        return lock_key(c);
    }
    /* Ctrl+U: スクリーンショット (どのアプリでも)。 */
    if (c == PRISM_KEY_CTRL_LETTER_BASE + ('u' - 'a')) {
        desktop_screenshot();
        return 1;
    }
    /* ショートカットのキー取り込み中は、押された特殊キーを
     * そのまま割り当てにする (Esc は取り消し)。アプリ起動の
     * 割り当て判定より先に処理しないと、取り込みたいキーで
     * アプリが起動してしまう。 */
    if (desk_mode == DESK_SETTINGS && bind_capture) {
        if (c == 0x1B) {
            bind_capture = 0;
            desktop_redraw();
            return 1;
        }
        if (c >= 0x90 && c <= 0x93) {
            /* 矢印キーは設定画面の移動に使う予約キー。
             * 名前で保存できない (parse が戻せない) ので
             * 割り当てには使わない。取り込みは続ける。 */
            return 1;
        }
        if (c >= 0x81) {
            int a = bind_selected;
            /* 既に他のアクションが使っているキーなら入れ替える
             * (重複した割り当てを作らない)。 */
            for (int i = 0; i < PRISM_BIND_COUNT; i++) {
                if (i != a && desk_settings.binds[i] == (uint8_t)c) {
                    desk_settings.binds[i] = desk_settings.binds[a];
                }
            }
            desk_settings.binds[a] = (uint8_t)c;
            bind_capture = 0;
            settings_autosave();
            desktop_redraw();
        }
        return 1;
    }
    /* 検索アクションの割り当てキーが検索 / ウィンドウ一覧を
     * 開閉する (既定は F8)。 */
    if (c == desk_settings.binds[PRISM_ACT_SEARCH]) {
        if (search_active) {
            search_active = 0;
            desktop_redraw();
        } else {
            search_active = 1;
            search_len = 0;
            search_query[0] = '\0';
            search_rebuild();
            desktop_redraw();
        }
        return 1;
    }
    if (search_active) {
        if (c == DESK_KEY_F9) {
            int on = ime_toggle();
            desktop_notify(on ? L("IME on", "IME オン")
                              : L("IME off", "IME オフ"));
            desktop_redraw();
            return 1;
        }
        if (c == 0x1B) {
            if (ime_active()) {
                char dummy[8];
                ime_feed(0x1B, dummy, sizeof(dummy));
                desktop_redraw();
                return 1;
            }
            search_active = 0;
            desktop_redraw();
            return 1;
        }
        if (ime_key_common(c, search_insert_text)) {
            return 1;
        }
        if (c == '\n' || c == '\r') {
            search_activate();
            return 1;
        }
        if (c == DESK_KEY_UP) {
            if (search_selected > 0) {
                search_selected--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            if (search_selected + 1 < search_count) {
                search_selected++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (search_len > 0) {
                search_len--;
                search_query[search_len] = '\0';
                search_rebuild();
                desktop_refresh_console();
            }
            return 1;
        }
        if (c >= 32 && c <= 126 &&
            search_len + 1 < (int)sizeof(search_query)) {
            search_query[search_len++] = (char)c;
            search_query[search_len] = '\0';
            search_rebuild();
            desktop_refresh_console();
            return 1;
        }
        return 1; /* Other keys while searching: consumed. */
    }
    /* ランチャーとメニューが開いている間はキーを占有する。 */
    if (launcher_active) {
        int n = launcher_entries();
        if (c == 0x1B) {
            launcher_active = 0;
            desktop_redraw();
        } else if (c == DESK_KEY_LEFT) {
            if (launcher_selected > 0) {
                launcher_selected--;
            }
            desktop_redraw();
        } else if (c == DESK_KEY_RIGHT) {
            if (launcher_selected + 1 < n) {
                launcher_selected++;
            }
            desktop_redraw();
        } else if (c == DESK_KEY_UP) {
            if (launcher_selected >= LAUNCH_COLS) {
                launcher_selected -= LAUNCH_COLS;
            }
            desktop_redraw();
        } else if (c == DESK_KEY_DOWN) {
            if (launcher_selected + LAUNCH_COLS < n) {
                launcher_selected += LAUNCH_COLS;
            }
            desktop_redraw();
        } else if (c == '\n' || c == '\r') {
            launcher_activate(launcher_selected);
        }
        return 1;
    }
    if (menu_active) {
        if (c == 0x1B) {
            menu_active = 0;
            desktop_redraw();
        }
        return 1;
    }
    if (widgets_open) {
        if (c == 0x1B) {
            widgets_open = 0;
            serial_write_line("widgets: closed");
            desktop_redraw();
        } else if (c == 'r' || c == 'R') {
            widgets_fetch();
            desktop_redraw();
        }
        return 1;
    }
    /* フォーカスのあるウィンドウが通常キーを先に受け取る
     * (F キー等の特殊コードは従来通りアプリ切替に回す)。 */
    if (desk_mode == DESK_TERMINAL && win_focus >= 0 &&
        win_open[win_focus]) {
        if (c == 0x1B) {
            win_close(win_focus);
            return 1;
        }
        if (c < 0x80 || c == 0xA2 || c == 0xB2 || c == 0xC7) {
            return win_key(win_focus, c);
        }
    }
    /* Notes では Ctrl+V は貼り付け (ビデオ起動より優先)。 */
    if (desk_mode == DESK_NOTES && c == 0xA2) {
        notes_load();
        if (notes_buf != NULL) {
            notes_undo_push(1);
            size_t n = 0;
            const char *clip = desktop_clipboard_get(&n);
            for (size_t i = 0; i < n; i++) {
                if (notes_len + 1 < PRISM_VFS_MAX_APP_FILE_SIZE) {
                    for (size_t j = notes_len; j > notes_cursor; j--) {
                        notes_buf[j] = notes_buf[j - 1];
                    }
                    notes_buf[notes_cursor++] = clip[i];
                    notes_len++;
                    notes_buf[notes_len] = '\0';
                    notes_dirty = 1;
                    notes_saved = 0;
                }
            }
            desktop_refresh_console();
        }
        return 1;
    }
    /* エディタ / Docs / メール作成でも Ctrl+V は貼り付け
     * (ビデオ起動より優先)。コピー側は従来どおり Notes の
     * 行コピーと Files の名前コピー。 */
    if (desk_mode == DESK_EDITOR && c == 0xA2 &&
        editor_stage == 0) {
        size_t n = 0;
        const char *clip = desktop_clipboard_get(&n);
        if (clip != NULL && n > 0) {
            char tmp[1024];
            size_t m = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
            for (size_t i = 0; i < m; i++) {
                tmp[i] = clip[i];
            }
            tmp[m] = '\0';
            editor_insert_text(tmp);
            serial_write_line("editor: pasted");
        }
        desktop_redraw();
        return 1;
    }
    if (desk_mode == DESK_DOCS && c == 0xA2) {
        docs_load();
        size_t n = 0;
        const char *clip = desktop_clipboard_get(&n);
        if (clip != NULL && n > 0) {
            char tmp[1024];
            size_t m = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
            for (size_t i = 0; i < m; i++) {
                tmp[i] = clip[i];
            }
            tmp[m] = '\0';
            docs_insert_text(tmp);
            serial_write_line("docs: pasted");
        }
        desktop_refresh_console();
        return 1;
    }
    if (desk_mode == DESK_MAIL && mail_composing && c == 0xA2) {
        size_t n = 0;
        const char *clip = desktop_clipboard_get(&n);
        if (clip != NULL && n > 0) {
            char *buf = NULL;
            size_t cap = 0;
            if (mail_field == 0) {
                buf = mail_to;
                cap = sizeof(mail_to);
            } else if (mail_field == 1) {
                buf = mail_subject;
                cap = sizeof(mail_subject);
            } else if (mail_field == 3) {
                buf = mail_attach_name;
                cap = sizeof(mail_attach_name);
            }
            if (buf != NULL) {
                size_t len = 0;
                while (buf[len] != '\0') {
                    len++;
                }
                for (size_t i = 0; i < n && len + 1 < cap; i++) {
                    if (clip[i] >= 32 && clip[i] < 127) {
                        buf[len++] = clip[i];
                    }
                }
                buf[len] = '\0';
            } else if (mail_field == 2) {
                for (size_t i = 0; i < n; i++) {
                    if (mail_body_len + 1 < sizeof(mail_body) &&
                        clip[i] >= 32 && clip[i] < 127) {
                        mail_body[mail_body_len++] = clip[i];
                        mail_body[mail_body_len] = '\0';
                    }
                }
            }
            serial_write_line("mail: pasted");
        }
        desktop_redraw();
        return 1;
    }
    /* テキスト入力アプリでは F9 は日本語入力 (IME) の
     * オン/オフ (アプリ起動キーより優先)。 */
    if ((desk_mode == DESK_NOTES || desk_mode == DESK_EDITOR ||
         desk_mode == DESK_DOCS || desk_mode == DESK_MAIL) &&
        c == DESK_KEY_F9) {
        int on = ime_toggle();
        desktop_notify(on ? L("IME on - romaji to kana (F9: off)", "IME オン - ローマ字かな変換 (F9: オフ)")
                          : L("IME off", "IME オフ"));
        desktop_refresh_console();
        return 1;
    }
    /* アプリ起動: 割り当てられたキーで launch_action へ。
     * 割り当ては設定画面のショートカットページで変更できる。 */
    for (int a = 0; a < PRISM_BIND_COUNT; a++) {
        if (a == PRISM_ACT_SEARCH) {
            continue;
        }
        if (c == desk_settings.binds[a]) {
            launch_action(a);
            return 1;
        }
    }
    if (c == DESK_KEY_CTRL_N) {
        if (desk_mode == DESK_SLIDES && !slides_show) {
            int was_editing = slides_editing;
            slides_commit_edit();
            if (!was_editing) {
                slides_history_push();
            }
            int n = slides_add(&slides_deck);
            if (n >= 0) {
                slides_current = n;
                slides_field = 0;
                slides_editing = 0;
                slides_dirty = 1;
                slides_saved = 0;
                desktop_redraw();
            }
        }
        return 1;
    }
    if (c == DESK_KEY_CTRL_E) {
        if (desk_mode == DESK_DOCS) {
            docs_export();
            desktop_refresh_console();
        } else if (desk_mode == DESK_SLIDES && !slides_show) {
            slides_export();
            desktop_refresh_console();
        }
        return 1;
    }
    if (c == DESK_KEY_CTRL_R) {
        if (desk_mode == DESK_DOCS) {
            docs_import();
            desktop_redraw();
        } else if (desk_mode == DESK_SLIDES && !slides_show) {
            slides_import();
            desktop_redraw();
        }
        return 1;
    }
    if (c == DESK_KEY_F6) {
        if (desk_mode == DESK_NOTES) {
            notes_save();
            desktop_refresh_console();
        }
        if (desk_mode == DESK_DOCS) {
            docs_save();
            desktop_refresh_console();
        }
        if (desk_mode == DESK_SLIDES) {
            slides_save();
            desktop_refresh_console();
        }
        if (desk_mode == DESK_EDITOR) {
            editor_save();
        }
        if (desk_mode == DESK_PAINT) {
            paint_save_bmp();
            desktop_redraw();
        }
        return 1;
    }
    if (c == 0x1B) { /* Esc */
        if ((desk_mode == DESK_NOTES || desk_mode == DESK_EDITOR ||
             desk_mode == DESK_DOCS || desk_mode == DESK_MAIL) &&
            ime_active()) {
            char dummy[8];
            ime_feed(0x1B, dummy, sizeof(dummy));
            desktop_refresh_console();
            return 1;
        }
        if (desk_mode == DESK_SLIDES && slides_show) {
            slides_show = 0;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_CARDS && cards_game != 0) {
            cards_game = 0;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_MAIL && mail_setup_mode) {
            mail_setup_mode = 0;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_STORE && store_srv_mode) {
            store_srv_mode = 0;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_MAIL && mail_composing) {
            mail_composing = 0;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_MAIL && mail_viewing >= 0) {
            mail_viewing = -1;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_SHEET && sheet_editing) {
            /* Cancel the edit, keep the old cell content. */
            sheet_editing = 0;
            sheet_edit_len = 0;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_SETTINGS &&
            (pin_dialog_open || acct_mode != ACCT_MODE_LIST)) {
            /* 設定のフォーム/ダイアログの Esc は取り消し。 */
            settings_cancel_modal();
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_FILES && (files_prompt || files_confirm)) {
            /* Files の入力/確認の Esc は取り消し。 */
            files_prompt = 0;
            files_confirm = 0;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_FILES && files_viewing >= 0) {
            files_viewing = -1;
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_BROWSER && br_find_mode) {
            br_find_mode = 0;
            desktop_refresh_console();
            return 1;
        }
        if (desk_mode == DESK_SETTINGS && settings_page == 7 &&
            adm_form) {
            adm_form = 0;
            adm_pass1_len = 0;
            adm_pass2_len = 0;
            adm_pass1[0] = '\0';
            adm_pass2[0] = '\0';
            desktop_redraw();
            return 1;
        }
        if (desk_mode == DESK_PHOTOS && photos_viewing >= 0) {
            photos_viewing = -1;
            photos_slideshow = 0;
            photos_zoom = 0;
            photos_rot = 0;
            desktop_redraw();
            return 1;
        }
        if (desk_mode != DESK_TERMINAL) {
            switch_mode(DESK_TERMINAL);
            return 1;
        }
        return 1; /* Esc in the terminal: ignore it. */
    }
    if (desk_mode == DESK_FILES) {
        files_build_rows();
        if (files_prompt) {
            if (c == '\n' || c == '\r') {
                files_prompt_commit();
                return 1;
            }
            if (c == '\b' || c == 0x7f) {
                if (files_prompt_len > 0) {
                    files_prompt_len--;
                    files_prompt_buf[files_prompt_len] = '\0';
                }
                desktop_redraw();
                return 1;
            }
            if (c >= 0x20 && c < 0x7f && c != '/' &&
                files_prompt_len + 1 < (int)sizeof(files_prompt_buf)) {
                files_prompt_buf[files_prompt_len++] = (char)c;
                files_prompt_buf[files_prompt_len] = '\0';
                desktop_redraw();
                return 1;
            }
            return 1;
        }
        if (files_confirm) {
            if (c == 'y' || c == 'Y' || c == '\n' || c == '\r') {
                int cf = files_confirm;
                files_confirm = 0;
                if (cf == 1) {
                    files_trash_empty();
                    desktop_notify(L("Trash emptied", "ゴミ箱を空にしました"));
                } else {
                    (void)vfs_delete(files_confirm_name);
                    serial_write("files: deleted permanently ");
                    serial_write_line(files_confirm_name);
                    persist_maybe_sync();
                    desktop_notify(L("Deleted permanently", "完全に削除しました"));
                }
                files_build_rows();
                desktop_redraw();
                return 1;
            }
            if (c == 'n' || c == 'N') {
                files_confirm = 0;
                desktop_redraw();
                return 1;
            }
            return 1;
        }
        if (c == DESK_KEY_UP) {
            if (files_selected > 0) {
                files_selected--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            if (files_selected + 1 < files_row_count) {
                files_selected++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            files_activate_row();
            return 1;
        }
        if (files_viewing < 0 && c == 'n') {
            files_prompt_start(2, NULL);
            return 1;
        }
        if (files_viewing < 0 && c == 'm') {
            files_prompt_start(3, NULL);
            return 1;
        }
        if (files_viewing < 0 && c == 'r') {
            if (files_selected >= 0 &&
                files_selected < files_row_count &&
                files_rows[files_selected] >= 0) {
                const char *nm =
                    vfs_file_name(files_rows[files_selected]);
                const char *bs = nm;
                for (const char *q2 = nm; *q2 != '\0'; q2++) {
                    if (*q2 == '/') {
                        bs = q2 + 1;
                    }
                }
                if (files_in_trash()) {
                    char btmp[28];
                    size_t bp = 0;
                    while (bs[bp] != '\0' && bp + 1 < sizeof(btmp)) {
                        btmp[bp] = bs[bp];
                        bp++;
                    }
                    btmp[bp] = '\0';
                    if (files_trash_restore(btmp) == 0) {
                        desktop_notify(L("Restored", "元に戻しました"));
                    } else {
                        desktop_notify(L("Could not restore",
                             "戻せませんでした (同名あり?)"));
                    }
                    files_build_rows();
                } else {
                    files_prompt_start(1, bs);
                }
            }
            desktop_redraw();
            return 1;
        }
        if (files_viewing < 0 && c == 'c' && !files_in_trash()) {
            if (files_selected >= 0 &&
                files_selected < files_row_count &&
                files_rows[files_selected] >= 0) {
                const char *nm =
                    vfs_file_name(files_rows[files_selected]);
                const char *bs = nm;
                for (const char *q2 = nm; *q2 != '\0'; q2++) {
                    if (*q2 == '/') {
                        bs = q2 + 1;
                    }
                }
                files_prompt_start(4, bs);
            }
            return 1;
        }
        if (files_viewing < 0 && c == 'o') {
            files_sort_mode = (files_sort_mode + 1) % 3;
            serial_write("files: sort ");
            serial_write_line(files_sort_mode == 1
                                  ? "date"
                                  : files_sort_mode == 2
                                  ? "size"
                                  : "name");
            desktop_redraw();
            return 1;
        }
        if (files_viewing < 0 && files_in_trash() && c == 'x') {
            if (files_selected >= 0 &&
                files_selected < files_row_count &&
                files_rows[files_selected] >= 0) {
                const char *nm =
                    vfs_file_name(files_rows[files_selected]);
                size_t cp = 0;
                while (nm[cp] != '\0' &&
                       cp + 1 < sizeof(files_confirm_name)) {
                    files_confirm_name[cp] = nm[cp];
                    cp++;
                }
                files_confirm_name[cp] = '\0';
                files_confirm = 2;
                desktop_redraw();
            }
            return 1;
        }
        if (files_viewing < 0 && files_in_trash() && c == 'e') {
            files_confirm = 1;
            desktop_redraw();
            return 1;
        }
        if (c == 'd' && !files_in_trash()) {
            if (files_selected >= 0 &&
                files_selected < files_row_count) {
                int rowv = files_rows[files_selected];
                if (rowv >= 0) {
                    const char *name = vfs_file_name(rowv);
                    char tmp[32];
                    size_t p = 0;
                    while (name[p] != '\0' && p + 1 < sizeof(tmp)) {
                        tmp[p] = name[p];
                        p++;
                    }
                    tmp[p] = '\0';
                    if (files_trash_file(tmp) == 0) {
                        serial_write("files: trashed ");
                        serial_write_line(tmp);
                        desktop_notify(L("Moved to trash", "ゴミ箱へ移動しました"));
                    } else {
                        desktop_notify(L("Could not trash it",
                     "ゴミ箱へ移動できませんでした"));
                    }
                } else if (rowv <= -2) {
                    /* 空のディレクトリだけ消せる。 */
                    char path[40];
                    size_t p = 0;
                    size_t q = 0;
                    while (files_dir[q] != '\0' &&
                           p + 1 < sizeof(path)) {
                        path[p++] = files_dir[q++];
                    }
                    if (p > 0 && p + 1 < sizeof(path)) {
                        path[p++] = '/';
                    }
                    const char *dn = files_dirnames[-2 - rowv];
                    q = 0;
                    while (dn[q] != '\0' && p + 1 < sizeof(path)) {
                        path[p++] = dn[q++];
                    }
                    /* 中身はマーカーだけか */
                    int count2 = vfs_file_count();
                    int only_marker = 1;
                    for (int i2 = 0; i2 < count2; i2++) {
                        const char *n2 = vfs_file_name(i2);
                        size_t k2 = 0;
                        while (k2 < p && n2[k2] == path[k2]) {
                            k2++;
                        }
                        if (k2 == p && n2[p] == '/' &&
                            strcmp(n2 + p + 1, ".dir") != 0) {
                            only_marker = 0;
                        }
                    }
                    if (only_marker) {
                        size_t mp = p;
                        const char *sfx = "/.dir";
                        size_t sq = 0;
                        while (sfx[sq] != '\0' &&
                               mp + 1 < sizeof(path)) {
                            /* path を一時的に使う */
                            sq++;
                            mp++;
                        }
                        char marker[48];
                        size_t m2 = 0;
                        while (m2 < p && m2 + 1 < sizeof(marker)) {
                            marker[m2] = path[m2];
                            m2++;
                        }
                        const char *sf2 = "/.dir";
                        size_t s2 = 0;
                        while (sf2[s2] != '\0' &&
                               m2 + 1 < sizeof(marker)) {
                            marker[m2++] = sf2[s2++];
                        }
                        marker[m2] = '\0';
                        (void)vfs_delete(marker);
                        desktop_notify(L("Directory removed", "フォルダを削除しました"));
                    } else {
                        desktop_notify(L("Directory not empty",
                     "フォルダが空ではありません"));
                    }
                }
                files_build_rows();
                desktop_refresh_console();
            }
            return 1;
        }
        return 1; /* Other keys in Files: consumed, not typed. */
    }
    if (desk_mode == DESK_SETTINGS) {
        /* 管理者 PIN ダイアログが開いている間はキーを占有する。 */
        if (pin_dialog_open) {
            return pin_dialog_key(c);
        }
        /* アカウントのフォーム表示中はキーを占有する
         * (Tab は入力欄の移動に使う)。 */
        if (settings_page == 2 && acct_mode != ACCT_MODE_LIST) {
            return acct_key(c);
        }
        /* 管理ページの FDE フォーム表示中も同様
         * (Tab は入力欄の移動)。 */
        if (settings_page == 7 && adm_form) {
            return adm_key(c);
        }
        /* Tab でページ切り替え (見た目→ショートカット→
         * アカウント→セキュリティ→更新→時計→見た目)。 */
        if (c == '\t') {
            settings_page = (settings_page + 1) % 8;
            if (settings_page == 3 || settings_page == 4 ||
                settings_page == 5 || settings_page == 6) {
                settings_refresh_shared();
            }
            if (settings_page == 4) {
                upd_refresh_snaps();
            }
            if (settings_page == 5) {
                clk_refresh();
            }
            if (settings_page == 6) {
                net_refresh();
            }
            if (settings_page == 7) {
                adm_refresh();
            }
            desktop_redraw();
            return 1;
        }
        if (settings_page == 1) {
            /* ショートカットページ。 */
            if (c == DESK_KEY_UP) {
                if (bind_selected > 0) {
                    bind_selected--;
                }
                desktop_refresh_console();
                return 1;
            }
            if (c == DESK_KEY_DOWN) {
                if (bind_selected + 1 < PRISM_BIND_COUNT) {
                    bind_selected++;
                }
                desktop_refresh_console();
                return 1;
            }
            if (c == '\n' || c == '\r') {
                bind_capture = 1;
                desktop_redraw();
                return 1;
            }
            if (c == '0') {
                /* 割り当てを全部既定値に戻す。 */
                settings_binds_defaults(desk_settings.binds);
                settings_autosave();
                desktop_redraw();
                return 1;
            }
            if (c == 's') {
                if (save_desk_settings() == 0) {
                    settings_saved = 1;
                    desktop_notify(L("Settings saved", "設定を保存しました"));
                }
                desktop_refresh_console();
                return 1;
            }
            return 1; /* Other keys on the shortcuts page: consumed. */
        }
        if (settings_page == 2) {
            return acct_key(c);
        }
        if (settings_page == 3) {
            return sec_key(c);
        }
        if (settings_page == 4) {
            return upd_key(c);
        }
        if (settings_page == 5) {
            return clock_key(c);
        }
        if (settings_page == 6) {
            return net_key(c);
        }
        if (settings_page == 7) {
            return adm_key(c);
        }
        if (c >= '1' && c <= '7') {
            desk_settings.accent_rgb = PRISM_ACCENTS[c - '1'];
            settings_autosave();
            desktop_redraw();
            return 1;
        }
        if (c == 't') {
            /* タスクバー位置: 下 → 上 → 左 → 右 → 下。 */
            if (desk_settings.taskbar_side == 1) {
                desk_settings.taskbar_side = 2;
            } else if (desk_settings.taskbar_side == 2) {
                desk_settings.taskbar_side = 0;
                desk_settings.taskbar_top = 0;
            } else if (desk_settings.taskbar_top) {
                desk_settings.taskbar_side = 1;
                desk_settings.taskbar_top = 0;
            } else {
                desk_settings.taskbar_top = 1;
            }
            settings_autosave();
            serial_write("settings: taskbar ");
            serial_write_line(desk_settings.taskbar_side == 1
                                  ? "left"
                                  : desk_settings.taskbar_side == 2
                                  ? "right"
                                  : desk_settings.taskbar_top
                                  ? "top"
                                  : "bottom");
            desktop_redraw();
            return 1;
        }
        if (c == 'm') {
            desk_settings.dark_mode = !desk_settings.dark_mode;
            settings_autosave();
            desktop_redraw();
            return 1;
        }
        if (c == 'w') {
            /* 描画 -> 標準画像 -> 自分の画像 (設定済みのとき)
             * -> 描画、の順に巡回する。 */
            int cur = desk_settings.wallpaper_image;
            int next;
            if (cur == 0) {
                next = 1;
            } else if (cur == 1) {
                next = desk_settings.wallpaper_file[0] != '\0' ? 2 : 0;
            } else {
                next = 0;
            }
            desk_settings.wallpaper_image = next;
            settings_autosave();
            desktop_redraw();
            return 1;
        }
        if (c == 'W') {
            /* 標準の壁紙画像に戻す。 */
            desk_settings.wallpaper_image = 1;
            settings_autosave();
            serial_write_line("settings: wallpaper reset to standard");
            desktop_redraw();
            return 1;
        }
        if (c == 'k') {
            desk_settings.dock_show = !desk_settings.dock_show;
            settings_autosave();
            desktop_redraw();
            return 1;
        }
        if (c == 'p') {
            desk_settings.dock_pos = !desk_settings.dock_pos;
            settings_autosave();
            desktop_redraw();
            return 1;
        }
        if (c == 'z') {
            desk_settings.dock_size = !desk_settings.dock_size;
            settings_autosave();
            desktop_redraw();
            return 1;
        }
        if (c == 'h') {
            desk_settings.dock_autohide = !desk_settings.dock_autohide;
            settings_autosave();
            desktop_redraw();
            return 1;
        }
        if (c == 'g') {
            desk_settings.language = !desk_settings.language;
            settings_autosave();
            desktop_notify(desk_settings.language
                               ? L("Language: Japanese", "言語: 日本語")
                               : L("Language: English", "言語: 英語"));
            desktop_redraw();
            return 1;
        }
        if (c == 'x') {
            desk_settings.high_contrast =
                !desk_settings.high_contrast;
            settings_autosave();
            desktop_notify(desk_settings.high_contrast
                               ? L("High contrast on",
                                   "ハイコントラスト オン")
                               : L("High contrast off",
                                   "ハイコントラスト オフ"));
            desktop_redraw();
            return 1;
        }
        if (c == 'b') {
            desk_settings.text_scale =
                desk_settings.text_scale >= 3
                    ? 1
                    : desk_settings.text_scale + 1;
            settings_autosave();
            console_refit();
            serial_write("settings: text size ");
            serial_write_num(desk_settings.text_scale);
            serial_write_line("");
            desktop_notify(desk_settings.text_scale >= 3
                               ? L("Text size: extra large",
                                   "文字サイズ: 特大")
                               : desk_settings.text_scale >= 2
                               ? L("Text size: large", "文字サイズ: 大")
                               : L("Text size: standard",
                                   "文字サイズ: 標準"));
            desktop_redraw();
            return 1;
        }
        if (c == 'o') {
            desk_settings.osk = !desk_settings.osk;
            settings_autosave();
            serial_write_line(desk_settings.osk
                                  ? "settings: on-screen keyboard on"
                                  : "settings: on-screen keyboard off");
            desktop_notify(desk_settings.osk
                               ? L("On-screen keyboard on",
                                   "画面キーボード オン")
                               : L("On-screen keyboard off",
                                   "画面キーボード オフ"));
            desktop_redraw();
            return 1;
        }
        if (c == 'n') {
            desk_settings.narrator = !desk_settings.narrator;
            settings_autosave();
            if (!desk_settings.narrator) {
                narrator_caption[0] = '\0';
            }
            serial_write_line(desk_settings.narrator
                                  ? "settings: narrator on"
                                  : "settings: narrator off");
            desktop_notify(desk_settings.narrator
                               ? L("Narrator on", "ナレーター オン")
                               : L("Narrator off", "ナレーター オフ"));
            desktop_redraw();
            return 1;
        }
        if (c == 'j') {
            desk_settings.kbd_layout = !desk_settings.kbd_layout;
            keyboard_set_layout(desk_settings.kbd_layout);
            settings_autosave();
            desktop_notify(desk_settings.kbd_layout
                               ? L("Keyboard: JIS", "キーボード: JIS")
                               : L("Keyboard: US", "キーボード: US"));
            desktop_redraw();
            return 1;
        }
        if (c == 'r') {
            /* 解像度をプリセット順に切り替える (シェルの
             * res と同じ処理。設定に保存して永続化する)。 */
            static const uint16_t modes[4][2] = {
                {1280, 800}, {1024, 768}, {800, 600}, {640, 480}};
            uint32_t cw = fb_width();
            uint32_t chh = fb_height();
            int next = 0;
            for (int i = 0; i < 4; i++) {
                if (modes[i][0] == cw && modes[i][1] == chh) {
                    next = (i + 1) % 4;
                }
            }
            if (vbe_set_mode(modes[next][0], modes[next][1]) == 0) {
                mouse_set_bounds(modes[next][0], modes[next][1]);
                mouse_set_position(modes[next][0] / 2,
                                   modes[next][1] / 2);
                /* 解像度を永続化する (シェルの res と同じ)。 */
                desk_settings.res_w = modes[next][0];
                desk_settings.res_h = modes[next][1];
                settings_autosave();
                serial_write("settings: resolution ");
                serial_write_num(modes[next][0]);
                serial_write("x");
                serial_write_num(modes[next][1]);
                serial_write_line("");
                desktop_notify(L("Resolution changed",
                                 "解像度を変更しました"));
            } else {
                serial_write_line("settings: resolution failed");
                desktop_notify(L("Resolution change failed",
                                 "解像度を変更できませんでした"));
            }
            desktop_redraw();
            return 1;
        }
        if (c == 'e') {
            /* マウス速度をプリセット順に切り替える。 */
            static const int speeds[5] = {50, 75, 100, 150, 200};
            int next = 100;
            for (int i = 0; i < 5; i++) {
                if (desk_settings.mouse_speed == speeds[i]) {
                    next = speeds[(i + 1) % 5];
                }
            }
            desk_settings.mouse_speed = next;
            mouse_set_speed(next);
            settings_autosave();
            serial_write("settings: mouse speed ");
            serial_write_num(next);
            serial_write_line("");
            desktop_redraw();
            return 1;
        }
        if (c == 'i') {
            desk_settings.key_repeat =
                (desk_settings.key_repeat + 1) % 3;
            keyboard_set_repeat(desk_settings.key_repeat);
            settings_autosave();
            serial_write("settings: key repeat ");
            serial_write_num(desk_settings.key_repeat);
            serial_write_line("");
            desktop_redraw();
            return 1;
        }
        if (c == 'u') {
            static const int intervals[5] = {200, 350, 500, 700,
                                             900};
            int next = 500;
            for (int i = 0; i < 5; i++) {
                if (desk_settings.dblclick_ms == intervals[i]) {
                    next = intervals[(i + 1) % 5];
                }
            }
            desk_settings.dblclick_ms = next;
            settings_autosave();
            serial_write("settings: dblclick ");
            serial_write_num(next);
            serial_write_line("");
            desktop_redraw();
            return 1;
        }
        if (c == 's') {
            if (save_desk_settings() == 0) {
                settings_saved = 1;
                desktop_notify(L("Settings saved", "設定を保存しました"));
            }
            desktop_refresh_console();
            return 1;
        }
        return 1; /* Other keys in Settings: consumed, not typed. */
    }
    if (desk_mode == DESK_INSTALLER) {
        if (inst_step == INSTALL_STEP_INSTALLING) {
            return 1; /* Installing: ignore keys. */
        }
        if (c == 0x1B) { /* Esc */
            int back = install_step_back(inst_step);
            if (back >= 0) {
                inst_step = (install_step)back;
                inst_error[0] = '\0';
                serial_write_line("installer: step back");
                desktop_redraw();
            } else {
                switch_mode(DESK_TERMINAL);
            }
            return 1;
        }
        if (inst_step == INSTALL_STEP_WELCOME) {
            if (c == '\n' || c == '\r') {
                int next =
                    install_step_next(inst_step, inst_has_disk, 0, 0);
                if (next >= 0) {
                    inst_step = (install_step)next;
                    serial_write_line("installer: step disk");
                    desktop_redraw();
                }
            }
            return 1;
        }
        if (inst_step == INSTALL_STEP_DISK) {
            if (c == 'a') {
                inst_target = 0;
                inst_error[0] = '\0';
                desktop_redraw();
            } else if (c == 'd') {
                if (inst_kind == INSTALL_DISK_BOOTABLE) {
                    inst_target = 1;
                    inst_error[0] = '\0';
                } else {
                    inst_copy(inst_error, sizeof(inst_error),
                              L("No data partition on this disk.", "このディスクにデータパーティションがありません"));
                }
                desktop_redraw();
            } else if (c == 'w') {
                if (inst_kind == INSTALL_DISK_BOOTABLE) {
                    inst_copy(inst_error, sizeof(inst_error),
                              L("Whole disk would erase the boot partition.", "ディスク全体だと起動パーティションも消えます"));
                } else {
                    /* 起動可能なディスクは作れない: 起動ファイルは
                     * インストール媒体にあり、OS からは読めない
                     * (CD の読み取り手段がなく、ディスクは 1 台
                     * しか扱えないため、写す元もない)。選んでも
                     * データ用パーティションへのインストールに
                     * すり替えることはしない。 */
                    inst_copy(inst_error, sizeof(inst_error),
                              L("Whole-disk install is not available yet.", "ディスク全体へのインストールはまだ使えません"));
                    serial_write_line(
                        "installer: whole-disk not available");
                }
                desktop_redraw();
            } else if (c == '\n' || c == '\r') {
                int next =
                    install_step_next(inst_step, inst_has_disk, 0, 0);
                if (next >= 0) {
                    inst_step = (install_step)next;
                    serial_write_line("installer: step profile");
                    desktop_redraw();
                }
            }
            return 1;
        }
        if (inst_step == INSTALL_STEP_PROFILE) {
            if (c == '1') {
                inst_profile = 0;
                desktop_redraw();
            } else if (c == '2') {
                inst_profile = 1;
                desktop_redraw();
            } else if (c == '\n' || c == '\r') {
                int next = install_step_next(inst_step, 1,
                                            inst_profile >= 0, 0);
                if (next >= 0) {
                    inst_step = (install_step)next;
                    serial_write_line("installer: step confirm");
                    desktop_redraw();
                }
            }
            return 1;
        }
        if (inst_step == INSTALL_STEP_CONFIRM) {
            if (c == 'y' || c == 'Y') {
                installer_run_install();
            }
            return 1;
        }
        if (inst_step == INSTALL_STEP_DONE) {
            if (c == 'r' || c == 'R') {
                shell_reboot_now();
            }
            return 1;
        }
        /* FAILED: only Esc (handled above) goes back. */
        return 1;
    }
    if (desk_mode == DESK_CONTACTS) {
        return contacts_handle_key(c);
    }
    if (desk_mode == DESK_EDITOR) {
        return editor_handle_key(c);
    }
    if (desk_mode == DESK_PAINT) {
        return paint_handle_key(c);
    }
    if (desk_mode == DESK_STORE) {
        return store_handle_key(c);
    }
    if (desk_mode == DESK_NOTIF) {
        if (c == DESK_KEY_UP) {
            if (notif_scroll > 0) {
                notif_scroll--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            notif_scroll++;
            desktop_refresh_console();
            return 1;
        }
        if (c == 0x1B) {
            switch_mode(DESK_TERMINAL);
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_MAIL) {
        if (mail_setup_mode) {
            return mail_setup_key(c);
        }
        if (mail_composing) {
            if (c == '\t') {
                mail_field = (mail_field + 1) % 5;
                desktop_redraw();
                return 1;
            }
            if ((mail_field == 1 || mail_field == 2) &&
                ime_key_common(c, mail_insert_text)) {
                return 1;
            }
            if (c == '\b' || c == 0x7F) {
                if (mail_field == 0) {
                    size_t n = 0;
                    while (mail_to[n] != '\0') {
                        n++;
                    }
                    if (n > 0) {
                        mail_to[n - 1] = '\0';
                    }
                } else if (mail_field == 1) {
                    size_t n = 0;
                    while (mail_subject[n] != '\0') {
                        n++;
                    }
                    if (n > 0) {
                        mail_subject[n - 1] = '\0';
                    }
                } else if (mail_field == 2 && mail_body_len > 0) {
                    mail_body[--mail_body_len] = '\0';
                } else if (mail_field == 3) {
                    size_t n = 0;
                    while (mail_attach_name[n] != '\0') {
                        n++;
                    }
                    if (n > 0) {
                        mail_attach_name[n - 1] = '\0';
                    }
                }
                desktop_redraw();
                return 1;
            }
            if (c == '\n' || c == '\r') {
                if (mail_field == 4) {
                    mail_send_from_app();
                } else if (mail_field == 2) {
                    if (mail_body_len + 1 < sizeof(mail_body)) {
                        mail_body[mail_body_len++] = '\n';
                        mail_body[mail_body_len] = '\0';
                    }
                    desktop_redraw();
                } else {
                    mail_field++;
                    desktop_redraw();
                }
                return 1;
            }
            if (c >= 32 && c < 127) {
                if (mail_field == 0) {
                    size_t n = 0;
                    while (mail_to[n] != '\0') {
                        n++;
                    }
                    if (n + 1 < sizeof(mail_to)) {
                        mail_to[n] = (char)c;
                        mail_to[n + 1] = '\0';
                    }
                } else if (mail_field == 1) {
                    size_t n = 0;
                    while (mail_subject[n] != '\0') {
                        n++;
                    }
                    if (n + 1 < sizeof(mail_subject)) {
                        mail_subject[n] = (char)c;
                        mail_subject[n + 1] = '\0';
                    }
                } else if (mail_field == 2) {
                    if (mail_body_len + 1 < sizeof(mail_body)) {
                        mail_body[mail_body_len++] = (char)c;
                        mail_body[mail_body_len] = '\0';
                    }
                } else if (mail_field == 3) {
                    size_t n = 0;
                    while (mail_attach_name[n] != '\0') {
                        n++;
                    }
                    if (n + 1 < sizeof(mail_attach_name)) {
                        mail_attach_name[n] = (char)c;
                        mail_attach_name[n + 1] = '\0';
                    }
                }
                desktop_redraw();
                return 1;
            }
            return 1;
        }
        if (mail_viewing >= 0) {
            if (c == 'r' || c == 'R') {
                mail_start_reply(0);
                return 1;
            }
            if (c == 'f' || c == 'F') {
                mail_start_reply(1);
                return 1;
            }
            if (c == DESK_KEY_UP) {
                if (mail_scroll > 0) {
                    mail_scroll--;
                }
                desktop_refresh_console();
                return 1;
            }
            if (c == DESK_KEY_DOWN) {
                mail_scroll++;
                desktop_refresh_console();
                return 1;
            }
            return 1;
        }
        if (mail_search_mode) {
            if (c == '\n' || c == '\r') {
                mail_search_buf[mail_search_len] = '\0';
                inst_copy(mail_filter, sizeof(mail_filter),
                          mail_search_buf);
                mail_search_mode = 0;
                mail_selected = 0;
                mail_rebuild();
                serial_write("mail: filter ");
                serial_write_line(mail_filter);
                desktop_redraw();
            } else if (c == 0x1B) {
                mail_search_mode = 0;
                mail_filter[0] = '\0';
                mail_rebuild();
                desktop_redraw();
            } else if (c == '\b' || c == 0x7F) {
                if (mail_search_len > 0) {
                    mail_search_len--;
                }
                desktop_redraw();
            } else if (c >= 32 && c < 127) {
                if (mail_search_len + 1 < sizeof(mail_search_buf)) {
                    mail_search_buf[mail_search_len++] = (char)c;
                }
                desktop_redraw();
            }
            return 1;
        }
        if (mail_move_mode) {
            if (c == '\n' || c == '\r') {
                mail_move_buf[mail_move_len] = '\0';
                if (mail_list_count > 0 && mail_move_len > 0) {
                    mail_set_folder_of(
                        mail_num_of(mail_selected),
                        mail_move_buf);
                    desktop_notify(L("Mail moved", "メールを移動しました"));
                    serial_write("mail: moved to ");
                    serial_write_line(mail_move_buf);
                }
                mail_move_mode = 0;
                mail_rebuild();
                desktop_redraw();
            } else if (c == '\b' || c == 0x7F) {
                if (mail_move_len > 0) {
                    mail_move_len--;
                }
                desktop_redraw();
            } else if (c >= 32 && c < 127) {
                if (mail_move_len + 1 < sizeof(mail_move_buf)) {
                    mail_move_buf[mail_move_len++] = (char)c;
                }
                desktop_redraw();
            }
            return 1;
        }
        if (c == DESK_KEY_UP) {
            if (mail_selected > 0) {
                mail_selected--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            if (mail_selected + 1 < mail_list_count) {
                mail_selected++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            if (mail_list_count > 0) {
                mail_viewing = mail_selected;
                mail_scroll = 0;
                desktop_refresh_console();
            }
            return 1;
        }
        if (c == 'o' || c == 'O') {
            /* アカウント設定フォームを開く。 */
            mail_setup_open();
            desktop_redraw();
            return 1;
        }
        if (c == 'c' || c == 'C') {
            mail_composing = 1;
            mail_field = 0;
            mail_to[0] = '\0';
            mail_subject[0] = '\0';
            mail_attach_name[0] = '\0';
            mail_body_len = 0;
            mail_body[0] = '\0';
            desktop_redraw();
            return 1;
        }
        if (c == 'r' || c == 'R') {
            mail_receive_from_app();
            return 1;
        }
        if (c == '/') {
            mail_search_mode = 1;
            mail_search_len = 0;
            mail_search_buf[0] = '\0';
            serial_write_line("mail: search mode");
            desktop_redraw();
            return 1;
        }
        if (c == 'f' || c == 'F') {
            /* Cycle the folder view: Inbox, Sent, Trash,
             * then each custom folder in use. */
            char names[8][24];
            int nn = 0;
            inst_copy(names[nn++], 24, "Inbox");
            inst_copy(names[nn++], 24, "Sent");
            inst_copy(names[nn++], 24, "Trash");
            size_t flen = 0;
            const char *ft =
                vfs_read_ptr("mail-folders.txt", &flen);
            if (ft != NULL) {
                size_t i = 0;
                while (i < flen && nn < 8) {
                    size_t ls = i;
                    while (i < flen && ft[i] != '\n') {
                        i++;
                    }
                    for (size_t k = ls; k < i; k++) {
                        if (ft[k] == '=') {
                            char cand[24];
                            size_t cp = 0;
                            for (size_t m = k + 1;
                                 m < i && cp + 1 < sizeof(cand);
                                 m++) {
                                if (ft[m] != '\r') {
                                    cand[cp++] = ft[m];
                                }
                            }
                            cand[cp] = '\0';
                            int dup = 0;
                            for (int q = 0; q < nn; q++) {
                                if (strcmp(names[q], cand) ==
                                    0) {
                                    dup = 1;
                                }
                            }
                            if (!dup && cand[0] != '\0') {
                                inst_copy(names[nn++], 24,
                                          cand);
                            }
                            break;
                        }
                    }
                    i++;
                }
            }
            int cur = 0;
            for (int q = 0; q < nn; q++) {
                if (strcmp(names[q], mail_folder_cur()) == 0) {
                    cur = q;
                }
            }
            inst_copy(mail_folder, sizeof(mail_folder),
                      names[(cur + 1) % nn]);
            mail_selected = 0;
            mail_viewing = -1;
            mail_rebuild();
            serial_write("mail: folder ");
            serial_write_line(mail_folder_cur());
            desktop_redraw();
            return 1;
        }
        if (c == 'm' || c == 'M') {
            if (mail_list_count > 0 &&
                strcmp(mail_folder_cur(), "Sent") != 0) {
                mail_move_mode = 1;
                mail_move_len = 0;
                mail_move_buf[0] = '\0';
                desktop_redraw();
            }
            return 1;
        }
        if (c == 'd' || c == 'D') {
            if (mail_list_count > 0) {
                const char *name =
                    vfs_file_name(mail_list_idx[mail_selected]);
                char tmp[32];
                size_t p = 0;
                while (*name != '\0' && p + 1 < sizeof(tmp)) {
                    tmp[p++] = *name++;
                }
                tmp[p] = '\0';
                if (strcmp(mail_folder_cur(), "Trash") == 0 ||
                    strcmp(mail_folder_cur(), "Sent") == 0) {
                    (void)vfs_delete(tmp);
                } else {
                    mail_set_folder_of(
                        mail_num_of(mail_selected), "Trash");
                    desktop_notify(L("Mail moved to Trash", "メールをゴミ箱へ移動しました"));
                }
                mail_rebuild();
                desktop_refresh_console();
            }
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_BROWSER) {
        if (br_find_mode) {
            if (c == '\n' || c == '\r') {
                br_find_buf[br_find_len] = '\0';
                br_find_run();
            } else if (c == '\b' || c == 0x7F) {
                if (br_find_len > 0) {
                    br_find_len--;
                }
            } else if (c >= 32 && c <= 126 &&
                       br_find_len + 1 < (int)sizeof(br_find_buf)) {
                br_find_buf[br_find_len++] = (char)c;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == 0xB5) { /* Ctrl+F: ページ内検索 */
            if (browser_state == 1) {
                br_find_mode = 1;
                br_find_len = 0;
                br_find_buf[0] = '\0';
                br_find_last = browser_scroll - 1;
                serial_write_line("browser: find opened");
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == 0xC2) { /* Ctrl+S: ページを保存 */
            if (browser_state == 1) {
                br_save_page();
            }
            desktop_redraw();
            return 1;
        }
        if (c == DESK_KEY_ZOOM_IN || c == DESK_KEY_ZOOM_OUT) {
            if (browser_state == 1) {
                browser_zoom += (c == DESK_KEY_ZOOM_IN) ? 1 : -1;
                if (browser_zoom > 3) {
                    browser_zoom = 3;
                }
                if (browser_zoom < 1) {
                    browser_zoom = 1;
                }
                br_render_current();
                serial_write("browser: zoom ");
                serial_write_num(browser_zoom);
                serial_write_line("");
            }
            desktop_redraw();
            return 1;
        }
        if (c == '\t') {
            /* Tab cycles the link focus. */
            if (browser_state == 1 && br_link_count > 0) {
                br_focus = (br_focus + 1) % br_link_count;
                serial_write("browser: focus ");
                serial_write_line(br_links[br_focus]);
                for (int li = 0; li < br_line_count; li++) {
                    if (br_lines[li].link == br_focus) {
                        if (li < browser_scroll ||
                            li >= browser_scroll +
                                      (text_rows() - 3) /
                                          browser_zoom) {
                            browser_scroll = li;
                        }
                        break;
                    }
                }
                desktop_redraw();
            }
            return 1;
        }
        if (c == '\n' || c == '\r') {
            if (browser_state == 1 && br_focus >= 0 &&
                br_focus < br_link_count) {
                br_navigate(br_links[br_focus]);
            } else if (browser_url_len > 0) {
                browser_load();
                desktop_redraw();
            }
            return 1;
        }
        if (c == DESK_KEY_UP) {
            if (browser_scroll > 0) {
                browser_scroll--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            browser_scroll++;
            desktop_refresh_console();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (browser_url_len > 0) {
                browser_url_len--;
                browser_url[browser_url_len] = '\0';
                browser_state = 0;
                browser_scroll = 0;
                br_focus = -1;
                desktop_redraw();
            }
            return 1;
        }
        if (c >= 32 && c <= 126) {
            if (browser_url_len + 1 < (int)sizeof(browser_url)) {
                browser_url[browser_url_len++] = (char)c;
                browser_url[browser_url_len] = '\0';
                browser_state = 0;
                browser_scroll = 0;
                br_focus = -1;
                desktop_redraw();
            }
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_NOTES) {
        notes_load();
        if (notes_buf == NULL) {
            return 1;
        }
        if (c == 0xC9) { /* Ctrl+Z: 取り消し */
            notes_undo_apply();
            return 1;
        }
        if (c == 0xC8) { /* Ctrl+Y: やり直し */
            notes_redo_apply();
            return 1;
        }
        if (c == 0xB5) { /* Ctrl+F: 検索 */
            notes_find_stage = 1;
            notes_find_replace = 0;
            notes_find_len = 0;
            notes_find_buf[0] = '\0';
            desktop_refresh_console();
            return 1;
        }
        if (c == 0xB7) { /* Ctrl+H: 置換 */
            notes_find_stage = 1;
            notes_find_replace = 1;
            notes_find_len = 0;
            notes_find_buf[0] = '\0';
            desktop_refresh_console();
            return 1;
        }
        if (notes_find_stage > 0) {
            char *buf = notes_find_stage == 1 ? notes_find_buf
                                              : notes_repl_buf;
            size_t *blen = notes_find_stage == 1
                               ? &notes_find_len
                               : &notes_repl_len;
            if (c == '\n' || c == '\r') {
                buf[*blen] = '\0';
                if (notes_find_stage == 1) {
                    inst_copy(notes_find_term,
                              sizeof(notes_find_term),
                              notes_find_buf);
                    if (notes_find_replace) {
                        notes_find_stage = 2;
                        notes_repl_len = 0;
                        notes_repl_buf[0] = '\0';
                    } else {
                        notes_find_stage = 0;
                        if (notes_find_next() >= 0) {
                            serial_write_line("notes: found");
                        } else {
                            desktop_notify(L("Not found",
                                             "見つかりませんでした"));
                        }
                    }
                } else {
                    notes_find_stage = 0;
                    int n = notes_replace_all();
                    serial_write("notes: replaced ");
                    serial_write_num(n);
                    serial_write_line(" occurrence(s)");
                }
                desktop_refresh_console();
            } else if (c == 0x1B) {
                notes_find_stage = 0;
                desktop_refresh_console();
            } else if (c == '\b' || c == 0x7F) {
                if (*blen > 0) {
                    (*blen)--;
                }
                desktop_refresh_console();
            } else if (c >= 32 && c <= 126) {
                if (*blen + 1 < 64) {
                    buf[(*blen)++] = (char)c;
                }
                desktop_refresh_console();
            }
            return 1;
        }
        if (notes_ime_key(c)) {
            return 1;
        }
        if (c == DESK_KEY_LEFT) {
            if (notes_cursor > 0) {
                notes_cursor--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_RIGHT) {
            if (notes_cursor < notes_len) {
                notes_cursor++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_UP) {
            /* Move up one line: back over the current line,
             * then over the previous line to the same column. */
            size_t col = 0;
            size_t i = notes_cursor;
            while (i > 0 && notes_buf[i - 1] != '\n') {
                i--;
                col++;
            }
            if (i == 0) {
                return 1;
            }
            size_t line_start = i;
            i--; /* skip the newline */
            size_t prev_start = i;
            while (prev_start > 0 && notes_buf[prev_start - 1] != '\n') {
                prev_start--;
            }
            (void)line_start;
            size_t prev_len = i - prev_start;
            notes_cursor = prev_start + (col < prev_len ? col : prev_len);
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            size_t i = notes_cursor;
            size_t col = 0;
            size_t line_start = i;
            while (line_start > 0 && notes_buf[line_start - 1] != '\n') {
                line_start--;
            }
            col = i - line_start;
            while (i < notes_len && notes_buf[i] != '\n') {
                i++;
            }
            if (i >= notes_len) {
                return 1;
            }
            i++; /* first char of the next line */
            size_t next_end = i;
            while (next_end < notes_len && notes_buf[next_end] != '\n') {
                next_end++;
            }
            size_t next_len = next_end - i;
            notes_cursor = i + (col < next_len ? col : next_len);
            desktop_refresh_console();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (notes_cursor > 0) {
                notes_undo_push(2);
                for (size_t i = notes_cursor - 1; i < notes_len; i++) {
                    notes_buf[i] = notes_buf[i + 1];
                }
                notes_len--;
                notes_cursor--;
                notes_buf[notes_len] = '\0';
                notes_dirty = 1;
                notes_saved = 0;
                desktop_refresh_console();
            }
            return 1;
        }
        if (c == 0xB2 || c == 0xC7) { /* Ctrl+C / Ctrl+X */
            size_t ls = notes_cursor;
            while (ls > 0 && notes_buf[ls - 1] != '\n') {
                ls--;
            }
            size_t le = notes_cursor;
            while (le < notes_len && notes_buf[le] != '\n') {
                le++;
            }
            desktop_clipboard_set(notes_buf + ls, le - ls);
            if (c == 0xC7) {
                notes_undo_push(2);
                size_t end = le < notes_len ? le + 1 : le;
                for (size_t i = ls; i + (end - ls) <= notes_len; i++) {
                    notes_buf[i] = notes_buf[i + (end - ls)];
                }
                notes_len -= end - ls;
                notes_buf[notes_len] = '\0';
                notes_cursor = ls;
                notes_dirty = 1;
                notes_saved = 0;
            }
            desktop_notify(c == 0xB2 ? L("Line copied", "行をコピーしました")
                              : L("Line cut", "行を切り取りました"));
            desktop_refresh_console();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            if (notes_len + 1 < PRISM_VFS_MAX_APP_FILE_SIZE) {
                notes_undo_push(1);
                for (size_t i = notes_len; i > notes_cursor; i--) {
                    notes_buf[i] = notes_buf[i - 1];
                }
                notes_buf[notes_cursor] = '\n';
                notes_len++;
                notes_cursor++;
                notes_buf[notes_len] = '\0';
                notes_dirty = 1;
                notes_saved = 0;
                desktop_refresh_console();
            }
            return 1;
        }
        if (c >= 32 && c <= 126) {
            if (notes_len + 1 < PRISM_VFS_MAX_APP_FILE_SIZE) {
                notes_undo_push(1);
                for (size_t i = notes_len; i > notes_cursor; i--) {
                    notes_buf[i] = notes_buf[i - 1];
                }
                notes_buf[notes_cursor] = (char)c;
                notes_len++;
                notes_cursor++;
                notes_buf[notes_len] = '\0';
                notes_dirty = 1;
                notes_saved = 0;
                desktop_refresh_console();
            }
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_DOCS) {
        docs_load();
        if (docs_fr_stage > 0) {
            return docs_fr_key(c);
        }
        if (c == PRISM_KEY_CTRL_LETTER_BASE + ('f' - 'a')) {
            docs_fr_stage = 1;
            docs_fr_len = 0;
            while (docs_fr_find[docs_fr_len] != '\0') {
                docs_fr_len++;
            }
            docs_fr_show();
            desktop_refresh_console();
            return 1;
        }
        if (c == 0xC9) { /* Ctrl+Z */
            docs_undo_apply();
            return 1;
        }
        if (c == 0xC8) { /* Ctrl+Y */
            docs_redo_apply();
            return 1;
        }
        if (docs_fmt_pending) {
            if (c == '1' || c == '2' || c == '3' || c == '-' ||
                c == '0') {
                doc_line_type kind = DOC_LINE_PLAIN;
                if (c == '1') {
                    kind = DOC_LINE_H1;
                } else if (c == '2') {
                    kind = DOC_LINE_H2;
                } else if (c == '3') {
                    kind = DOC_LINE_H3;
                } else if (c == '-') {
                    kind = DOC_LINE_BULLET;
                }
                docs_history_push();
                doc_set_line_type(&docs_doc, kind);
                docs_dirty = 1;
                docs_saved = 0;
                docs_fmt_pending = 0;
                desktop_refresh_console();
                return 1;
            }
            if (c == '\\') {
                /* Two backslashes: type one literal backslash. */
                docs_fmt_pending = 0;
                docs_history_push();
                doc_insert_char(&docs_doc, '\\');
                docs_dirty = 1;
                docs_saved = 0;
                desktop_refresh_console();
                return 1;
            }
            /* Any other key: cancel the format, type the '\'
             * first, then fall through to the key below. */
            docs_fmt_pending = 0;
            docs_history_push();
            doc_insert_char(&docs_doc, '\\');
            docs_dirty = 1;
            docs_saved = 0;
        }
        if (c == '\\') {
            docs_fmt_pending = 1;
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_LEFT) {
            doc_cursor_left(&docs_doc);
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_RIGHT) {
            doc_cursor_right(&docs_doc);
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_UP) {
            size_t row = doc_cursor_row(&docs_doc);
            if (row == 0) {
                return 1;
            }
            size_t col = doc_cursor_col(&docs_doc);
            size_t s = 0;
            size_t e = 0;
            if (!doc_line_bounds(&docs_doc, row - 1, &s, &e)) {
                return 1;
            }
            size_t len = e - s;
            docs_doc.cursor = s + (col < len ? col : len);
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            size_t row = doc_cursor_row(&docs_doc);
            size_t col = doc_cursor_col(&docs_doc);
            size_t total = doc_line_count(&docs_doc);
            if (row + 1 >= total) {
                return 1;
            }
            size_t s = 0;
            size_t e = 0;
            if (!doc_line_bounds(&docs_doc, row + 1, &s, &e)) {
                return 1;
            }
            size_t len = e - s;
            docs_doc.cursor = s + (col < len ? col : len);
            desktop_refresh_console();
            return 1;
        }
        if (ime_key_common(c, docs_insert_text)) {
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            docs_history_push();
            doc_backspace(&docs_doc);
            docs_dirty = 1;
            docs_saved = 0;
            desktop_refresh_console();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            docs_history_push();
            doc_insert_newline(&docs_doc);
            docs_dirty = 1;
            docs_saved = 0;
            desktop_refresh_console();
            return 1;
        }
        if (c >= 32 && c <= 126) {
            docs_history_push();
            doc_insert_char(&docs_doc, (char)c);
            docs_dirty = 1;
            docs_saved = 0;
            desktop_refresh_console();
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_SLIDES) {
        slides_load();
        if (!slides_show && c == 0xC9) { /* Ctrl+Z */
            slides_undo_apply();
            return 1;
        }
        if (!slides_show && c == 0xC8) { /* Ctrl+Y */
            slides_redo_apply();
            return 1;
        }
        if (slides_show) {
            if (c == DESK_KEY_LEFT || c == DESK_KEY_UP) {
                if (slides_current > 0) {
                    slides_current--;
                }
                desktop_redraw();
                return 1;
            }
            if (c == DESK_KEY_RIGHT || c == DESK_KEY_DOWN || c == ' ' ||
                c == '\n' || c == '\r') {
                if (slides_current + 1 < slides_deck.count) {
                    slides_current++;
                }
                desktop_redraw();
                return 1;
            }
            return 1;
        }
        if (c == '\t') {
            slides_commit_edit();
            const slide *sl = &slides_deck.slides[slides_current];
            int max_field = sl->line_count + 1;
            if (max_field > SLIDES_LINES_MAX) {
                max_field = SLIDES_LINES_MAX;
            }
            slides_field++;
            if (slides_field > max_field) {
                slides_field = 0;
            }
            slides_edit_start();
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_UP) {
            slides_commit_edit();
            if (slides_field > 0) {
                slides_field--;
            }
            slides_edit_start();
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            slides_commit_edit();
            const slide *sl = &slides_deck.slides[slides_current];
            int max_field = sl->line_count + 1;
            if (max_field > SLIDES_LINES_MAX) {
                max_field = SLIDES_LINES_MAX;
            }
            if (slides_field < max_field) {
                slides_field++;
            }
            slides_edit_start();
            desktop_refresh_console();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            int was_field = slides_field;
            slides_commit_edit();
            const slide *sl = &slides_deck.slides[slides_current];
            if (was_field == 0) {
                slides_field = 1;
            } else {
                slides_field = was_field + 1;
                if (slides_field > sl->line_count + 1) {
                    slides_field = sl->line_count + 1;
                }
                if (slides_field > SLIDES_LINES_MAX) {
                    slides_field = SLIDES_LINES_MAX;
                }
            }
            slides_edit_start();
            desktop_refresh_console();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (!slides_editing) {
                slides_edit_start();
            }
            if (slides_edit_len > 0) {
                slides_edit[--slides_edit_len] = '\0';
                slides_dirty = 1;
                slides_saved = 0;
            } else if (slides_field > 0 &&
                       slides_field - 1 <
                           slides_deck.slides[slides_current]
                               .line_count) {
                /* Empty bullet field: remove the line. */
                slides_commit_edit();
                slides_remove_line(&slides_deck, slides_current,
                                   slides_field - 1);
                if (slides_field > 1) {
                    slides_field--;
                }
                slides_dirty = 1;
                slides_saved = 0;
                slides_edit_start();
            }
            desktop_refresh_console();
            return 1;
        }
        if (c >= 32 && c <= 126) {
            if (!slides_editing) {
                slides_edit_start();
            }
            if (slides_edit_len + 1 < (int)sizeof(slides_edit)) {
                slides_edit[slides_edit_len++] = (char)c;
                slides_edit[slides_edit_len] = '\0';
                slides_dirty = 1;
                slides_saved = 0;
            }
            desktop_refresh_console();
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_CALENDAR) {
        if (c == DESK_KEY_LEFT) {
            cal_offset--;
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_RIGHT) {
            cal_offset++;
            desktop_refresh_console();
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_PHOTOS) {
        int list[PRISM_VFS_MAX_FILES];
        int count = photos_list(list, PRISM_VFS_MAX_FILES);
        if (photos_viewing >= 0) {
            if (c == 'w' || c == 'W') {
                photos_set_wallpaper();
                desktop_redraw();
            } else if (c == DESK_KEY_LEFT || c == DESK_KEY_RIGHT) {
                /* 前 / 次の写真へ (端では反対側へ回る)。 */
                if (count > 0) {
                    photos_viewing =
                        (photos_viewing +
                         (c == DESK_KEY_RIGHT ? 1 : count - 1)) %
                        count;
                    photos_selected = photos_viewing;
                    photos_zoom = 0;
                    photos_rot = 0;
                    photos_status[0] = '\0';
                    serial_write("photos: viewing ");
                    serial_write_num(photos_viewing);
                    serial_write_line("");
                }
                desktop_redraw();
            } else if (c == '+' || c == '-') {
                photos_zoom += (c == '+') ? 1 : -1;
                if (photos_zoom > 2) {
                    photos_zoom = 2;
                }
                if (photos_zoom < 0) {
                    photos_zoom = 0;
                }
                serial_write("photos: zoom ");
                serial_write_num(photos_zoom);
                serial_write_line("");
                desktop_redraw();
            } else if (c == 'r' || c == 'R') {
                photos_rot = (photos_rot + 1) % 4;
                serial_write("photos: rotate ");
                serial_write_num(photos_rot * 90);
                serial_write_line("");
                desktop_redraw();
            } else if (c == 's' || c == 'S') {
                photos_slideshow = !photos_slideshow;
                serial_write_line(photos_slideshow
                                      ? "photos: slideshow on"
                                      : "photos: slideshow off");
                desktop_redraw();
            }
            return 1; /* Viewing: only Esc (handled above) leaves. */
        }
        if (c == DESK_KEY_UP) {
            if (photos_selected > 0) {
                photos_selected--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            if (photos_selected + 1 < count) {
                photos_selected++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            if (count > 0) {
                photos_viewing = photos_selected;
                desktop_refresh_console();
            }
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_MUSIC) {
        int list[PRISM_VFS_MAX_FILES];
        int count = music_list(list, PRISM_VFS_MAX_FILES);
        if (c == DESK_KEY_UP) {
            if (music_selected > 0) {
                music_selected--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            if (music_selected + 1 < count) {
                music_selected++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            if (count > 0) {
                music_play_selected();
                desktop_refresh_console();
            }
            return 1;
        }
        if (c == '+' || c == '-') {
            volume_adjust(c == '+' ? 10 : -10);
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_CARDS) {
        return cards_handle_key(c);
    }
    if (desk_mode == DESK_SHOOTER) {
        if (c == 'n' || c == 'N') {
            shooter_new_game(&shooter_game_state);
            shooter_hitmark = 0;
            serial_write_line("shooter: new game");
            desktop_redraw();
            return 1;
        }
        if (c == ' ') {
            shooter_fire();
            desktop_redraw();
            return 1;
        }
        if (c == 'w' || c == 'W' || c == DESK_KEY_UP) {
            if (shooter_game_state.phase == 1) {
                shooter_try_move(&shooter_game_state.px,
                                 &shooter_game_state.py,
                                 shooter_game_state.angle,
                                 SHOOTER_FX_ONE * 15 / 100);
                desktop_redraw();
            }
            return 1;
        }
        if (c == 's' || c == 'S' || c == DESK_KEY_DOWN) {
            if (shooter_game_state.phase == 1) {
                shooter_try_move(&shooter_game_state.px,
                                 &shooter_game_state.py,
                                 (shooter_game_state.angle + 512) & 1023,
                                 SHOOTER_FX_ONE * 15 / 100);
                desktop_redraw();
            }
            return 1;
        }
        if (c == 'a' || c == 'A' || c == DESK_KEY_LEFT) {
            if (shooter_game_state.phase == 1) {
                shooter_turn(&shooter_game_state, -8);
                desktop_redraw();
            }
            return 1;
        }
        if (c == 'd' || c == 'D' || c == DESK_KEY_RIGHT) {
            if (shooter_game_state.phase == 1) {
                shooter_turn(&shooter_game_state, 8);
                desktop_redraw();
            }
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_SHEET) {
        if (sheet_fr_stage > 0) {
            return sheet_fr_key(c);
        }
        if (c == PRISM_KEY_CTRL_LETTER_BASE + ('f' - 'a')) {
            sheet_commit_edit();
            sheet_fr_stage = 1;
            sheet_fr_len = 0;
            while (sheet_fr_find[sheet_fr_len] != '\0') {
                sheet_fr_len++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == 0xC9) { /* Ctrl+Z */
            sheet_commit_edit();
            sheet_undo_apply();
            return 1;
        }
        if (c == 0xC8) { /* Ctrl+Y */
            sheet_redo_apply();
            return 1;
        }
        if (c == DESK_KEY_UP) {
            sheet_commit_edit();
            if (sheet_sel_row > 0) {
                sheet_sel_row--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_DOWN) {
            sheet_commit_edit();
            if (sheet_sel_row + 1 < SHEET_ROWS) {
                sheet_sel_row++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_LEFT) {
            sheet_commit_edit();
            if (sheet_sel_col > 0) {
                sheet_sel_col--;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == DESK_KEY_RIGHT) {
            sheet_commit_edit();
            if (sheet_sel_col + 1 < SHEET_COLS) {
                sheet_sel_col++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == '\t') {
            sheet_commit_edit();
            if (sheet_sel_col + 1 < SHEET_COLS) {
                sheet_sel_col++;
            } else if (sheet_sel_row + 1 < SHEET_ROWS) {
                sheet_sel_col = 0;
                sheet_sel_row++;
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == '\n' || c == '\r') {
            if (sheet_editing) {
                sheet_commit_edit();
            } else {
                sheet_editing = 1;
                sheet_edit_len = 0;
                sheet_edit[0] = '\0';
            }
            desktop_refresh_console();
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (sheet_editing && sheet_edit_len > 0) {
                sheet_edit_len--;
                sheet_edit[sheet_edit_len] = '\0';
                desktop_refresh_console();
            }
            return 1;
        }
        if (!sheet_editing && (c == 'g' || c == 'G')) {
            sheet_chart = !sheet_chart;
            serial_write_line(sheet_chart ? "sheet: chart on"
                                          : "sheet: chart off");
            desktop_redraw();
            return 1;
        }
        if (sheet_chart) {
            return 1; /* グラフ表示中は他のキーを受けない */
        }
        if (!sheet_editing && (c == 'o' || c == 'O')) {
            sheet_commit_edit();
            sheet_sort(sheet_sel_col, c == 'O');
            desktop_refresh_console();
            return 1;
        }
        if (!sheet_editing && (c == 's' || c == 'S')) {
            sheet_save();
            desktop_refresh_console();
            return 1;
        }
        if (!sheet_editing && (c == 'l' || c == 'L')) {
            sheet_load();
            desktop_refresh_console();
            return 1;
        }
        if (c >= 32 && c <= 126) {
            if (!sheet_editing) {
                sheet_editing = 1;
                sheet_edit_len = 0;
                sheet_edit[0] = '\0';
            }
            if (sheet_edit_len < SHEET_CELL_MAX) {
                sheet_edit[sheet_edit_len++] = (char)c;
                sheet_edit[sheet_edit_len] = '\0';
            }
            desktop_refresh_console();
            return 1;
        }
        return 1;
    }
    if (desk_mode == DESK_VIDEO) {
        if (c == ' ') {
            if (video_frame_count <= 0) {
                return 1;
            }
            video_player_state.playing = !video_player_state.playing;
            if (video_player_state.playing) {
                video_arm_next_tick();
                serial_write_line("video: play");
            } else {
                serial_write_line("video: pause");
            }
            desktop_redraw();
            if (video_player_state.playing &&
                video_play_frame_audio()) {
                /* 最初のフレームの音声も鳴らし、次のフレームは
                 * 音声が終わり次第進める。 */
                video_next_tick = (int64_t)tick_counter;
            }
            return 1;
        }
        if (c == DESK_KEY_LEFT || c == 'p' || c == 'P') {
            video_seek(&video_player_state, -1);
            video_arm_next_tick();
            desktop_redraw();
            return 1;
        }
        if (c == DESK_KEY_RIGHT || c == 'n' || c == 'N') {
            video_seek(&video_player_state, 1);
            video_arm_next_tick();
            desktop_redraw();
            return 1;
        }
        if (c == 'r' || c == 'R') {
            video_player_state.frame = 0;
            video_arm_next_tick();
            desktop_redraw();
            return 1;
        }
        if (c == '+' || c == '-') {
            volume_adjust(c == '+' ? 10 : -10);
            desktop_redraw();
            return 1;
        }
        return 1;
    }
    /* Terminal のスクロールバック (項目16)。 */
    if (desk_mode == DESK_TERMINAL &&
        (c == DESK_KEY_PGUP || c == DESK_KEY_PGDN)) {
        int avail = console_hist_count < CONSOLE_HIST_LINES
                        ? console_hist_count
                        : CONSOLE_HIST_LINES;
        if (c == DESK_KEY_PGUP) {
            console_view_offset += console_rows - 2;
            if (console_view_offset > avail) {
                console_view_offset = avail;
            }
            serial_write("terminal: scrollback ");
            serial_write_num(console_view_offset);
            serial_write_line("");
        } else {
            console_view_offset -= console_rows - 2;
            if (console_view_offset < 0) {
                console_view_offset = 0;
            }
        }
        desktop_refresh_console();
        return 1;
    }
    /* Terminal mode: arrows and other special keys are ignored. */
    if (c >= 0x80) {
        return 1;
    }
    return 0;
}
