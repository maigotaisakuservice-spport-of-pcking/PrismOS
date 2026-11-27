/* PrismOS - desktop settings (pure logic). */
#include "settings.h"
#include "keyboard.h"
#include "string.h"

const char *const PRISM_ACTION_KEYS[PRISM_BIND_COUNT] = {
    "terminal", "files", "settings", "browser", "notes",
    "calendar", "search", "photos", "music", "cards",
    "shooter", "sheet", "video", "installer", "mail",
    "docs", "slides", "contacts", "editor", "paint", "store", "clock",
    "recorder", "camera",
};

const char *const PRISM_ACTION_NAMES[PRISM_BIND_COUNT] = {
    "Terminal", "Files", "Settings", "Browser", "Notes",
    "Calendar", "Search", "Photos", "Music", "Cards",
    "Shooter", "Sheet", "Video", "Installer", "Mail",
    "Docs", "Slides", "Contacts", "Editor", "Paint", "Store", "Clock",
    "Recorder", "Camera",
};

void settings_binds_defaults(uint8_t *binds) {
    if (binds == NULL) {
        return;
    }
    binds[PRISM_ACT_TERMINAL] = PRISM_KEY_F1;
    binds[PRISM_ACT_FILES] = PRISM_KEY_F2;
    binds[PRISM_ACT_SETTINGS] = PRISM_KEY_F3;
    binds[PRISM_ACT_BROWSER] = PRISM_KEY_F4;
    binds[PRISM_ACT_NOTES] = PRISM_KEY_F5;
    binds[PRISM_ACT_CALENDAR] = PRISM_KEY_F7;
    binds[PRISM_ACT_SEARCH] = PRISM_KEY_F8;
    binds[PRISM_ACT_PHOTOS] = PRISM_KEY_F9;
    binds[PRISM_ACT_MUSIC] = PRISM_KEY_F10;
    binds[PRISM_ACT_CARDS] = PRISM_KEY_F11;
    binds[PRISM_ACT_SHOOTER] = PRISM_KEY_F12;
    binds[PRISM_ACT_SHEET] = PRISM_KEY_CTRL_O;
    binds[PRISM_ACT_VIDEO] = PRISM_KEY_CTRL_V;
    binds[PRISM_ACT_INSTALLER] = PRISM_KEY_CTRL_I;
    binds[PRISM_ACT_MAIL] = PRISM_KEY_CTRL_M;
    binds[PRISM_ACT_DOCS] = PRISM_KEY_CTRL_D;
    binds[PRISM_ACT_SLIDES] = PRISM_KEY_CTRL_P;
    binds[PRISM_ACT_CONTACTS] = PRISM_KEY_CTRL_LETTER_BASE + ('b' - 'a');
    binds[PRISM_ACT_EDITOR] = PRISM_KEY_CTRL_LETTER_BASE + ('t' - 'a');
    binds[PRISM_ACT_PAINT] = PRISM_KEY_CTRL_LETTER_BASE + ('a' - 'a');
    binds[PRISM_ACT_STORE] = PRISM_KEY_CTRL_LETTER_BASE + ('k' - 'a');
    /* Ctrl+C はテキストのコピーと重なるため時計は
     * Ctrl+L。 */
    binds[PRISM_ACT_CLOCK] = PRISM_KEY_CTRL_LETTER_BASE + ('l' - 'a');
    /* Ctrl+R は文書/スライドの取り込みと重なり、
     * Ctrl+U は全体スクリーンショットが先に取るため
     * レコーダーは Ctrl+Q。 */
    binds[PRISM_ACT_RECORDER] = PRISM_KEY_CTRL_LETTER_BASE + ('q' - 'a');
    /* Ctrl+G はエディタの「ファイルを開く」と重なるため
     * カメラは Ctrl+J。 */
    binds[PRISM_ACT_CAMERA] = PRISM_KEY_CTRL_LETTER_BASE + ('j' - 'a');
}

/* レガシー Ctrl コードと英字の対応 (keyboard.c と同じ)。 */
static char legacy_ctrl_letter(int code) {
    switch (code) {
    case PRISM_KEY_CTRL_O: return 'o';
    case PRISM_KEY_CTRL_V: return 'v';
    case PRISM_KEY_CTRL_I: return 'i';
    case PRISM_KEY_CTRL_M: return 'm';
    case PRISM_KEY_CTRL_D: return 'd';
    case PRISM_KEY_CTRL_P: return 'p';
    case PRISM_KEY_CTRL_N: return 'n';
    case PRISM_KEY_CTRL_E: return 'e';
    case PRISM_KEY_CTRL_R: return 'r';
    default: return 0;
    }
}

static int legacy_ctrl_code(char letter) {
    switch (letter) {
    case 'o': return PRISM_KEY_CTRL_O;
    case 'v': return PRISM_KEY_CTRL_V;
    case 'i': return PRISM_KEY_CTRL_I;
    case 'm': return PRISM_KEY_CTRL_M;
    case 'd': return PRISM_KEY_CTRL_D;
    case 'p': return PRISM_KEY_CTRL_P;
    case 'n': return PRISM_KEY_CTRL_N;
    case 'e': return PRISM_KEY_CTRL_E;
    case 'r': return PRISM_KEY_CTRL_R;
    default: return -1;
    }
}

int settings_key_name(int code, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) {
        return -1;
    }
    if (code >= PRISM_KEY_F1 && code <= PRISM_KEY_F12) {
        int n = code - PRISM_KEY_F1 + 1;
        if (n < 10) {
            if (cap < 3) {
                return -1;
            }
            buf[0] = 'F';
            buf[1] = (char)('0' + n);
            buf[2] = '\0';
            return 2;
        }
        if (cap < 4) {
            return -1;
        }
        buf[0] = 'F';
        buf[1] = '1';
        buf[2] = (char)('0' + (n - 10));
        buf[3] = '\0';
        return 3;
    }
    char letter = legacy_ctrl_letter(code);
    if (letter == 0 && code >= PRISM_KEY_CTRL_LETTER_BASE &&
        code < PRISM_KEY_CTRL_LETTER_BASE + 26) {
        letter = (char)('a' + (code - PRISM_KEY_CTRL_LETTER_BASE));
    }
    if (letter != 0) {
        if (cap < 4) {
            return -1;
        }
        buf[0] = 'C';
        buf[1] = '-';
        buf[2] = letter;
        buf[3] = '\0';
        return 3;
    }
    return -1;
}

int settings_key_parse(const char *name) {
    if (name == NULL) {
        return -1;
    }
    if (name[0] == 'F' || name[0] == 'f') {
        int n = 0;
        int i = 1;
        if (name[i] < '0' || name[i] > '9') {
            return -1;
        }
        while (name[i] >= '0' && name[i] <= '9') {
            n = n * 10 + (name[i] - '0');
            if (n > 12) {
                return -1; /* F12 を超えたら即座に切る (溢れ防止) */
            }
            i++;
        }
        if (name[i] != '\0' || n < 1 || n > 12) {
            return -1;
        }
        return PRISM_KEY_F1 + (n - 1);
    }
    if ((name[0] == 'C' || name[0] == 'c') && name[1] == '-') {
        char c = name[2];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c < 'a' || c > 'z' || name[3] != '\0') {
            return -1;
        }
        int legacy = legacy_ctrl_code(c);
        if (legacy >= 0) {
            return legacy;
        }
        return PRISM_KEY_CTRL_LETTER_BASE + (c - 'a');
    }
    return -1;
}

void settings_overlay_security(prism_settings *dst,
                               const prism_settings *src) {
    if (dst == NULL || src == NULL) {
        return;
    }
    dst->usb_media = src->usb_media;
    dst->pin_min_len = src->pin_min_len;
    dst->pin_require_digit = src->pin_require_digit;
    dst->lockout_attempts = src->lockout_attempts;
    dst->lockout_seconds = src->lockout_seconds;
}

void settings_overlay_desktop(prism_settings *dst,
                              const prism_settings *src) {
    if (dst == NULL || src == NULL) {
        return;
    }
    dst->accent_rgb = src->accent_rgb;
    dst->taskbar_top = src->taskbar_top;
    dst->taskbar_side = src->taskbar_side;
    dst->taskbar_center = src->taskbar_center;
    dst->dark_mode = src->dark_mode;
    dst->high_contrast = src->high_contrast;
    dst->text_scale = src->text_scale;
    dst->osk = src->osk;
    dst->narrator = src->narrator;
    dst->volume = src->volume;
    dst->res_w = src->res_w;
    dst->res_h = src->res_h;
    dst->wallpaper_image = src->wallpaper_image;
    {
        size_t j = 0;
        while (src->wallpaper_file[j] != '\0' &&
               j + 1 < sizeof(dst->wallpaper_file)) {
            dst->wallpaper_file[j] = src->wallpaper_file[j];
            j++;
        }
        dst->wallpaper_file[j] = '\0';
    }
    dst->dock_show = src->dock_show;
    dst->ui_motion = src->ui_motion;
    dst->dock_pos = src->dock_pos;
    dst->dock_size = src->dock_size;
    dst->dock_autohide = src->dock_autohide;
    dst->language = src->language;
    dst->kbd_layout = src->kbd_layout;
    dst->tz_offset = src->tz_offset;
    dst->mouse_speed = src->mouse_speed;
    dst->key_repeat = src->key_repeat;
    dst->dblclick_ms = src->dblclick_ms;
    dst->net_static = src->net_static;
    {
        const char *srcs[4] = {src->net_ip, src->net_mask,
                               src->net_gw, src->net_dns};
        char *dsts[4] = {dst->net_ip, dst->net_mask, dst->net_gw,
                         dst->net_dns};
        for (int f = 0; f < 4; f++) {
            size_t j = 0;
            while (srcs[f][j] != '\0' && j + 1 < 16) {
                dsts[f][j] = srcs[f][j];
                j++;
            }
            dsts[f][j] = '\0';
        }
    }
    {
        const char *fsrcs[2] = {src->feed_weather, src->feed_news};
        char *fdsts[2] = {dst->feed_weather, dst->feed_news};
        for (int f = 0; f < 2; f++) {
            size_t j = 0;
            while (fsrcs[f][j] != '\0' && j + 1 < 128) {
                fdsts[f][j] = fsrcs[f][j];
                j++;
            }
            fdsts[f][j] = '\0';
        }
    }
    dst->hidden_apps = src->hidden_apps;
    dst->install_profile = src->install_profile;
    for (int i = 0; i < PRISM_BIND_COUNT; i++) {
        dst->binds[i] = src->binds[i];
    }
}

const uint32_t PRISM_ACCENTS[PRISM_ACCENT_COUNT] = {
    0x4CC2FF, /* cyan */
    0x7C5CFF, /* violet */
    0xFF6FB5, /* pink */
    0x4CC26B, /* green */
    0xFF9F43, /* orange */
    0xE74C3C, /* red */
    0x1ABC9C, /* teal */
};

void settings_defaults(prism_settings *s) {
    if (s == NULL) {
        return;
    }
    s->accent_rgb = PRISM_ACCENTS[0];
    s->taskbar_top = 0;
    s->taskbar_side = 0;
    s->taskbar_center = 1;
    s->dark_mode = 1;
    s->high_contrast = 0;
    s->text_scale = 1;
    s->osk = 0;
    s->narrator = 0;
    s->volume = 100;
    s->res_w = 0;
    s->res_h = 0;
    s->wallpaper_image = 1;
    s->wallpaper_file[0] = '\0';
    s->dock_show = 0;
    s->ui_motion = 1;
    s->dock_pos = 0;
    s->dock_size = 0;
    s->dock_autohide = 0;
    s->language = 0;
    s->kbd_layout = 0;
    s->tz_offset = 540;
    s->mouse_speed = 100;
    s->key_repeat = 1;
    s->dblclick_ms = 500;
    s->net_static = 0;
    s->net_ip[0] = '\0';
    s->net_mask[0] = '\0';
    s->net_gw[0] = '\0';
    s->net_dns[0] = '\0';
    s->feed_weather[0] = '\0';
    s->feed_news[0] = '\0';
    s->hidden_apps = 0;
    s->update_channel = 0;
    s->auto_security = 0;
    s->install_profile = 0;
    s->usb_media = 0;
    s->pin_min_len = 4;
    s->pin_require_digit = 0;
    s->lockout_attempts = 3;
    s->lockout_seconds = 300;
    settings_binds_defaults(s->binds);
    s->update_host[0] = '\0';
    s->update_path[0] = '\0';
    /* 既定のマニフェストの場所。ホストが空の間は使わない。 */
    const char *def_path = "/update-manifest.txt";
    size_t i = 0;
    while (def_path[i] != '\0') {
        s->update_path[i] = def_path[i];
        i++;
    }
    s->update_path[i] = '\0';
}

void settings_accent_hex(uint32_t rgb, char *buf) {
    static const char HEX[] = "0123456789ABCDEF";
    if (buf == NULL) {
        return;
    }
    buf[0] = '#';
    for (int i = 0; i < 6; i++) {
        buf[1 + i] = HEX[(rgb >> (20 - 4 * i)) & 0xF];
    }
    buf[7] = '\0';
}

/* s を pos へ追加する。バッファに収まる最長 prefix だけを
 * 書き、常に NUL 終端を保つが、戻り値は「全文に必要な長さ」
 * (pos + strlen(s)) とする。呼び出し側は戻り値と cap を
 * 比べることで切り捨てを検出できる (監査 M1: 以前は cap で
 * 静かに止まり、末尾のセキュリティ項目が保存されなかった)。 */
static size_t append_str(char *buf, size_t cap, size_t pos, const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        if (pos + n + 1 < cap) {
            buf[pos + n] = s[n];
        }
        n++;
    }
    if (cap > 0) {
        size_t end = pos + n;
        if (end >= cap) {
            end = cap - 1;
        }
        buf[end] = '\0';
    }
    return pos + n;
}

/* "\n" + key + 整数値 を足す (解像度など新しい項目用)。 */
static size_t append_int_field(char *buf, size_t cap, size_t pos,
                               const char *key, int v) {
    char rev[12];
    int r = 0;
    int neg = 0;
    if (v < 0) {
        neg = 1;
        v = -v;
    }
    if (v == 0) {
        rev[r++] = '0';
    }
    while (v > 0) {
        rev[r++] = (char)('0' + v % 10);
        v /= 10;
    }
    char nb[16];
    int n = 0;
    if (neg) {
        nb[n++] = '-';
    }
    while (r > 0) {
        nb[n++] = rev[--r];
    }
    nb[n] = '\0';
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, key);
    pos = append_str(buf, cap, pos, nb);
    return pos;
}

size_t settings_serialize(const prism_settings *s, char *buf, size_t cap) {
    char hex[8];
    size_t pos = 0;
    if (s == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    settings_accent_hex(s->accent_rgb, hex);
    pos = append_str(buf, cap, pos, "accent=");
    pos = append_str(buf, cap, pos, hex);
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "taskbar=");
    pos = append_str(buf, cap, pos, s->taskbar_top ? "top" : "bottom");
    pos = append_str(buf, cap, pos, "\n");
    if (s->taskbar_side != 0) {
        pos = append_str(buf, cap, pos, "taskbar_side=");
        pos = append_str(buf, cap, pos,
                         s->taskbar_side == 1 ? "left" : "right");
        pos = append_str(buf, cap, pos, "\n");
    }
    pos = append_str(buf, cap, pos, "taskbar_icons=");
    pos = append_str(buf, cap, pos,
                     s->taskbar_center ? "center" : "left");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "mode=");
    pos = append_str(buf, cap, pos, s->dark_mode ? "dark" : "light");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "contrast=");
    pos = append_str(buf, cap, pos, s->high_contrast ? "on" : "off");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "text_scale=");
    pos = append_str(buf, cap, pos,
                     s->text_scale >= 3 ? "3"
                     : s->text_scale >= 2 ? "2"
                                          : "1");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "osk=");
    pos = append_str(buf, cap, pos, s->osk ? "on" : "off");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "narrator=");
    pos = append_str(buf, cap, pos, s->narrator ? "on" : "off");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "wallpaper=");
    pos = append_str(buf, cap, pos,
                     s->wallpaper_image == 2
                         ? "custom"
                         : (s->wallpaper_image ? "image" : "drawn"));
    pos = append_str(buf, cap, pos, "\n");
    if (s->wallpaper_file[0] != '\0') {
        pos = append_str(buf, cap, pos, "wallpaper_file=");
        pos = append_str(buf, cap, pos, s->wallpaper_file);
        pos = append_str(buf, cap, pos, "\n");
    }
    pos = append_str(buf, cap, pos, "dock=");
    pos = append_str(buf, cap, pos, s->dock_show ? "show" : "hide");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "motion=");
    pos = append_str(buf, cap, pos, s->ui_motion ? "on" : "off");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "dock_pos=");
    pos = append_str(buf, cap, pos, s->dock_pos ? "left" : "bottom");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "dock_size=");
    pos = append_str(buf, cap, pos, s->dock_size ? "large" : "normal");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "dock_autohide=");
    pos = append_str(buf, cap, pos, s->dock_autohide ? "on" : "off");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "language=");
    pos = append_str(buf, cap, pos, s->language ? "ja" : "en");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "kbd_layout=");
    pos = append_str(buf, cap, pos, s->kbd_layout ? "jis" : "us");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "update_channel=");
    pos = append_str(buf, cap, pos, s->update_channel ? "beta" : "stable");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "net_mode=");
    pos = append_str(buf, cap, pos, s->net_static ? "static" : "dhcp");
    pos = append_str(buf, cap, pos, "\n");
    if (s->net_ip[0] != '\0') {
        pos = append_str(buf, cap, pos, "net_ip=");
        pos = append_str(buf, cap, pos, s->net_ip);
        pos = append_str(buf, cap, pos, "\n");
    }
    if (s->net_mask[0] != '\0') {
        pos = append_str(buf, cap, pos, "net_mask=");
        pos = append_str(buf, cap, pos, s->net_mask);
        pos = append_str(buf, cap, pos, "\n");
    }
    if (s->net_gw[0] != '\0') {
        pos = append_str(buf, cap, pos, "net_gw=");
        pos = append_str(buf, cap, pos, s->net_gw);
        pos = append_str(buf, cap, pos, "\n");
    }
    if (s->net_dns[0] != '\0') {
        pos = append_str(buf, cap, pos, "net_dns=");
        pos = append_str(buf, cap, pos, s->net_dns);
        pos = append_str(buf, cap, pos, "\n");
    }
    if (s->feed_weather[0] != '\0') {
        pos = append_str(buf, cap, pos, "feed_weather=");
        pos = append_str(buf, cap, pos, s->feed_weather);
        pos = append_str(buf, cap, pos, "\n");
    }
    if (s->feed_news[0] != '\0') {
        pos = append_str(buf, cap, pos, "feed_news=");
        pos = append_str(buf, cap, pos, s->feed_news);
        pos = append_str(buf, cap, pos, "\n");
    }
    if (s->hidden_apps != 0) {
        pos = append_str(buf, cap, pos, "hidden_apps=");
        char nb[12];
        int n = 0;
        int v = s->hidden_apps;
        char rev[12];
        int r = 0;
        if (v == 0) {
            nb[n++] = '0';
        }
        while (v > 0) {
            rev[r++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (r > 0) {
            nb[n++] = rev[--r];
        }
        nb[n] = '\0';
        pos = append_str(buf, cap, pos, nb);
        pos = append_str(buf, cap, pos, "\n");
    }
    pos = append_str(buf, cap, pos, "tz_offset=");
    {
        char nb[16];
        int n = 0;
        int v = s->tz_offset;
        if (v < 0) {
            nb[n++] = '-';
            v = -v;
        }
        char rev[8];
        int r = 0;
        if (v == 0) {
            rev[r++] = '0';
        }
        while (v > 0) {
            rev[r++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (r > 0) {
            nb[n++] = rev[--r];
        }
        nb[n] = '\0';
        pos = append_str(buf, cap, pos, nb);
    }
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "mouse_speed=");
    {
        char nb[8];
        int n = 0;
        int v = s->mouse_speed;
        char rev[4];
        int r = 0;
        if (v == 0) {
            rev[r++] = '0';
        }
        while (v > 0) {
            rev[r++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (r > 0) {
            nb[n++] = rev[--r];
        }
        nb[n] = '\0';
        pos = append_str(buf, cap, pos, nb);
    }
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "key_repeat=");
    pos = append_str(buf, cap, pos,
                     s->key_repeat == 0
                         ? "slow"
                         : s->key_repeat == 2 ? "fast" : "normal");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "dblclick_ms=");
    {
        char nb[8];
        int n = 0;
        int v = s->dblclick_ms;
        char rev[4];
        int r = 0;
        if (v == 0) {
            rev[r++] = '0';
        }
        while (v > 0) {
            rev[r++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (r > 0) {
            nb[n++] = rev[--r];
        }
        nb[n] = '\0';
        pos = append_str(buf, cap, pos, nb);
    }
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "volume=");
    {
        char nb[8];
        int n = 0;
        int v = s->volume;
        if (v < 0) {
            v = 0;
        }
        if (v > 100) {
            v = 100;
        }
        char rev[4];
        int r = 0;
        if (v == 0) {
            rev[r++] = '0';
        }
        while (v > 0) {
            rev[r++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (r > 0) {
            nb[n++] = rev[--r];
        }
        nb[n] = '\0';
        pos = append_str(buf, cap, pos, nb);
    }
    pos = append_int_field(buf, cap, pos, "res_w=", s->res_w);
    pos = append_int_field(buf, cap, pos, "res_h=", s->res_h);
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "auto_security=");
    pos = append_str(buf, cap, pos, s->auto_security ? "on" : "off");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "update_host=");
    pos = append_str(buf, cap, pos, s->update_host);
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "update_path=");
    pos = append_str(buf, cap, pos, s->update_path);
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "install_profile=");
    pos = append_str(buf, cap, pos, s->install_profile ? "minimal" : "standard");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "usb_media=");
    pos = append_str(buf, cap, pos, s->usb_media ? "block" : "allow");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "pin_min_len=");
    {
        char num[4];
        int v = s->pin_min_len;
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
        pos = append_str(buf, cap, pos, num);
    }
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "pin_require_digit=");
    pos = append_str(buf, cap, pos, s->pin_require_digit ? "on" : "off");
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "lockout_attempts=");
    {
        char num[4];
        int v = s->lockout_attempts;
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
        pos = append_str(buf, cap, pos, num);
    }
    pos = append_str(buf, cap, pos, "\n");
    pos = append_str(buf, cap, pos, "lockout_seconds=");
    {
        char num[8];
        int v = s->lockout_seconds;
        int d = 0;
        char tmp[8];
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
        pos = append_str(buf, cap, pos, num);
    }
    pos = append_str(buf, cap, pos, "\n");
    /* ショートカットの割り当て (bind_<アクション>=<キー名>)。 */
    for (int a = 0; a < PRISM_BIND_COUNT; a++) {
        char kname[8];
        if (settings_key_name(s->binds[a], kname, sizeof(kname)) < 0) {
            continue;
        }
        pos = append_str(buf, cap, pos, "bind_");
        pos = append_str(buf, cap, pos, PRISM_ACTION_KEYS[a]);
        pos = append_str(buf, cap, pos, "=");
        pos = append_str(buf, cap, pos, kname);
        pos = append_str(buf, cap, pos, "\n");
    }
    return pos;
}

static int parse_hex_colour(const char *s, uint32_t *out) {
    uint32_t value = 0;
    if (s == NULL || s[0] != '#') {
        return 0;
    }
    for (int i = 1; i <= 6; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else {
            return 0;
        }
        value = (value << 4) | d;
    }
    if (s[7] != '\0' && s[7] != '\n' && s[7] != '\r') {
        return 0;
    }
    *out = value;
    return 1;
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (*s != *prefix) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

/* 10 進の整数を読む。数字以外が混じったら -1。 */
static int parse_int_value(const char *s) {
    if (s == NULL || *s < '0' || *s > '9') {
        return -1;
    }
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
        if (v > 100000) {
            return -1;
        }
    }
    if (*s != '\0') {
        return -1;
    }
    return v;
}

int settings_parse(prism_settings *s, const char *text, size_t len) {
    int found = 0;
    size_t i = 0;
    if (s == NULL || text == NULL) {
        return 0;
    }
    while (i < len) {
        /* Take one line (up to 191 chars) into a small buffer.
         * update_path は長いため、64 文字では足りない。 */
        char line[192];
        size_t n = 0;
        while (i < len && text[i] != '\n' && n + 1 < sizeof(line)) {
            line[n++] = text[i++];
        }
        int terminated = (i < len && text[i] == '\n');
        while (i < len && text[i] != '\n') {
            i++;
        }
        if (i < len) {
            i++; /* skip the newline */
        }
        line[n] = '\0';
        if (!terminated) {
            /* 改行で終わっていない最終行は、途中で切れた
             * ファイルの残りかもしれない。数字の途中切断
             * (例: lockout_seconds=300 が =3 に切れる) を
             * 正値として受理しないよう、この行は無視する
             * (監査 M1)。シリアライズは全行を改行で終える
             * ため、正規のファイルでは最終行も終端している。 */
            break;
        }
        if (starts_with(line, "accent=")) {
            uint32_t rgb = 0;
            if (parse_hex_colour(line + 7, &rgb)) {
                s->accent_rgb = rgb;
                found = 1;
            }
        } else if (starts_with(line, "taskbar=")) {
            if (strcmp(line + 8, "top") == 0) {
                s->taskbar_top = 1;
                found = 1;
            } else if (strcmp(line + 8, "bottom") == 0) {
                s->taskbar_top = 0;
                found = 1;
            }
        } else if (starts_with(line, "taskbar_side=")) {
            if (strcmp(line + 13, "left") == 0) {
                s->taskbar_side = 1;
                found = 1;
            } else if (strcmp(line + 13, "right") == 0) {
                s->taskbar_side = 2;
                found = 1;
            } else if (strcmp(line + 13, "none") == 0) {
                s->taskbar_side = 0;
                found = 1;
            }
        } else if (starts_with(line, "taskbar_icons=")) {
            if (strcmp(line + 14, "center") == 0) {
                s->taskbar_center = 1;
                found = 1;
            } else if (strcmp(line + 14, "left") == 0) {
                s->taskbar_center = 0;
                found = 1;
            }
        } else if (starts_with(line, "mode=")) {
            if (strcmp(line + 5, "dark") == 0) {
                s->dark_mode = 1;
                found = 1;
            } else if (strcmp(line + 5, "light") == 0) {
                s->dark_mode = 0;
                found = 1;
            }
        } else if (starts_with(line, "contrast=")) {
            if (strcmp(line + 9, "on") == 0) {
                s->high_contrast = 1;
                found = 1;
            } else if (strcmp(line + 9, "off") == 0) {
                s->high_contrast = 0;
                found = 1;
            }
        } else if (starts_with(line, "text_scale=")) {
            if (strcmp(line + 11, "3") == 0) {
                s->text_scale = 3;
                found = 1;
            } else if (strcmp(line + 11, "2") == 0) {
                s->text_scale = 2;
                found = 1;
            } else if (strcmp(line + 11, "1") == 0) {
                s->text_scale = 1;
                found = 1;
            }
        } else if (starts_with(line, "osk=")) {
            if (strcmp(line + 4, "on") == 0) {
                s->osk = 1;
                found = 1;
            } else if (strcmp(line + 4, "off") == 0) {
                s->osk = 0;
                found = 1;
            }
        } else if (starts_with(line, "narrator=")) {
            if (strcmp(line + 9, "on") == 0) {
                s->narrator = 1;
                found = 1;
            } else if (strcmp(line + 9, "off") == 0) {
                s->narrator = 0;
                found = 1;
            }
        } else if (starts_with(line, "wallpaper=")) {
            if (strcmp(line + 10, "image") == 0) {
                s->wallpaper_image = 1;
                found = 1;
            } else if (strcmp(line + 10, "drawn") == 0) {
                s->wallpaper_image = 0;
                found = 1;
            } else if (strcmp(line + 10, "custom") == 0) {
                s->wallpaper_image = 2;
                found = 1;
            }
        } else if (starts_with(line, "wallpaper_file=")) {
            size_t j = 0;
            while (line[15 + j] != '\0' &&
                   j + 1 < sizeof(s->wallpaper_file)) {
                s->wallpaper_file[j] = line[15 + j];
                j++;
            }
            s->wallpaper_file[j] = '\0';
            found = 1;
        } else if (starts_with(line, "dock=")) {
            if (strcmp(line + 5, "show") == 0) {
                s->dock_show = 1;
                found = 1;
            } else if (strcmp(line + 5, "hide") == 0) {
                s->dock_show = 0;
                found = 1;
            }
        } else if (starts_with(line, "motion=")) {
            if (strcmp(line + 7, "on") == 0) {
                s->ui_motion = 1;
                found = 1;
            } else if (strcmp(line + 7, "off") == 0) {
                s->ui_motion = 0;
                found = 1;
            }
        } else if (starts_with(line, "dock_pos=")) {
            if (strcmp(line + 9, "left") == 0) {
                s->dock_pos = 1;
                found = 1;
            } else if (strcmp(line + 9, "bottom") == 0) {
                s->dock_pos = 0;
                found = 1;
            }
        } else if (starts_with(line, "dock_size=")) {
            if (strcmp(line + 10, "large") == 0) {
                s->dock_size = 1;
                found = 1;
            } else if (strcmp(line + 10, "normal") == 0) {
                s->dock_size = 0;
                found = 1;
            }
        } else if (starts_with(line, "dock_autohide=")) {
            if (strcmp(line + 14, "on") == 0) {
                s->dock_autohide = 1;
                found = 1;
            } else if (strcmp(line + 14, "off") == 0) {
                s->dock_autohide = 0;
                found = 1;
            }
        } else if (starts_with(line, "language=")) {
            if (strcmp(line + 9, "ja") == 0) {
                s->language = 1;
                found = 1;
            } else if (strcmp(line + 9, "en") == 0) {
                s->language = 0;
                found = 1;
            }
        } else if (starts_with(line, "tz_offset=")) {
            const char *v0 = line + 10;
            int sign = 1;
            if (*v0 == '-') {
                sign = -1;
                v0++;
            }
            int v = parse_int_value(v0);
            if (v >= 0) {
                v *= sign;
                if (v >= -720 && v <= 840) {
                    s->tz_offset = v;
                }
            }
        } else if (starts_with(line, "volume=")) {
            int v = parse_int_value(line + 7);
            if (v >= 0 && v <= 100) {
                s->volume = v;
                found = 1;
            }
        } else if (starts_with(line, "res_w=")) {
            int v = parse_int_value(line + 6);
            if (v >= 0 && v <= 7680) {
                s->res_w = v;
                found = 1;
            }
        } else if (starts_with(line, "res_h=")) {
            int v = parse_int_value(line + 6);
            if (v >= 0 && v <= 4320) {
                s->res_h = v;
                found = 1;
            }
        } else if (starts_with(line, "mouse_speed=")) {
            int v = parse_int_value(line + 12);
            if (v >= 25 && v <= 400) {
                s->mouse_speed = v;
                found = 1;
            }
        } else if (starts_with(line, "key_repeat=")) {
            if (strcmp(line + 11, "slow") == 0) {
                s->key_repeat = 0;
                found = 1;
            } else if (strcmp(line + 11, "fast") == 0) {
                s->key_repeat = 2;
                found = 1;
            } else if (strcmp(line + 11, "normal") == 0) {
                s->key_repeat = 1;
                found = 1;
            }
        } else if (starts_with(line, "dblclick_ms=")) {
            int v = parse_int_value(line + 12);
            if (v >= 100 && v <= 2000) {
                s->dblclick_ms = v;
                found = 1;
            }
        } else if (starts_with(line, "kbd_layout=")) {
            if (strcmp(line + 11, "jis") == 0) {
                s->kbd_layout = 1;
                found = 1;
            } else if (strcmp(line + 11, "us") == 0) {
                s->kbd_layout = 0;
                found = 1;
            }
        } else if (starts_with(line, "bind_")) {
            /* bind_<アクション>=<キー名> */
            const char *eq = line + 5;
            while (*eq != '\0' && *eq != '=') {
                eq++;
            }
            if (*eq == '=') {
                size_t klen = (size_t)(eq - (line + 5));
                int code = settings_key_parse(eq + 1);
                if (code >= 0) {
                    for (int a = 0; a < PRISM_BIND_COUNT; a++) {
                        const char *ak = PRISM_ACTION_KEYS[a];
                        size_t alen = 0;
                        while (ak[alen] != '\0') {
                            alen++;
                        }
                        if (alen == klen) {
                            int same = 1;
                            for (size_t k = 0; k < klen; k++) {
                                if (line[5 + k] != ak[k]) {
                                    same = 0;
                                    break;
                                }
                            }
                            if (same) {
                                s->binds[a] = (uint8_t)code;
                                found = 1;
                                break;
                            }
                        }
                    }
                }
            }
        } else if (starts_with(line, "update_channel=")) {
            if (strcmp(line + 15, "beta") == 0) {
                s->update_channel = 1;
                found = 1;
            } else if (strcmp(line + 15, "stable") == 0) {
                s->update_channel = 0;
                found = 1;
            }
        } else if (starts_with(line, "net_mode=")) {
            if (strcmp(line + 9, "static") == 0) {
                s->net_static = 1;
                found = 1;
            } else if (strcmp(line + 9, "dhcp") == 0) {
                s->net_static = 0;
                found = 1;
            }
        } else if (starts_with(line, "net_ip=")) {
            size_t j = 0;
            while (line[7 + j] != '\0' && j + 1 < sizeof(s->net_ip)) {
                s->net_ip[j] = line[7 + j];
                j++;
            }
            s->net_ip[j] = '\0';
            found = 1;
        } else if (starts_with(line, "net_mask=")) {
            size_t j = 0;
            while (line[9 + j] != '\0' &&
                   j + 1 < sizeof(s->net_mask)) {
                s->net_mask[j] = line[9 + j];
                j++;
            }
            s->net_mask[j] = '\0';
            found = 1;
        } else if (starts_with(line, "net_gw=")) {
            size_t j = 0;
            while (line[7 + j] != '\0' && j + 1 < sizeof(s->net_gw)) {
                s->net_gw[j] = line[7 + j];
                j++;
            }
            s->net_gw[j] = '\0';
            found = 1;
        } else if (starts_with(line, "net_dns=")) {
            size_t j = 0;
            while (line[8 + j] != '\0' &&
                   j + 1 < sizeof(s->net_dns)) {
                s->net_dns[j] = line[8 + j];
                j++;
            }
            s->net_dns[j] = '\0';
            found = 1;
        } else if (starts_with(line, "feed_weather=")) {
            size_t j = 0;
            while (line[13 + j] != '\0' &&
                   j + 1 < sizeof(s->feed_weather)) {
                s->feed_weather[j] = line[13 + j];
                j++;
            }
            s->feed_weather[j] = '\0';
            found = 1;
        } else if (starts_with(line, "feed_news=")) {
            size_t j = 0;
            while (line[10 + j] != '\0' &&
                   j + 1 < sizeof(s->feed_news)) {
                s->feed_news[j] = line[10 + j];
                j++;
            }
            s->feed_news[j] = '\0';
            found = 1;
        } else if (starts_with(line, "hidden_apps=")) {
            int v = parse_int_value(line + 12);
            if (v >= 0) {
                s->hidden_apps = v;
                found = 1;
            }
        } else if (starts_with(line, "auto_security=")) {
            if (strcmp(line + 14, "on") == 0) {
                s->auto_security = 1;
                found = 1;
            } else if (strcmp(line + 14, "off") == 0) {
                s->auto_security = 0;
                found = 1;
            }
        } else if (starts_with(line, "update_host=")) {
            size_t j = 0;
            while (line[12 + j] != '\0' &&
                   j + 1 < sizeof(s->update_host)) {
                s->update_host[j] = line[12 + j];
                j++;
            }
            s->update_host[j] = '\0';
            found = 1;
        } else if (starts_with(line, "install_profile=")) {
            if (strcmp(line + 16, "minimal") == 0) {
                s->install_profile = 1;
                found = 1;
            } else if (strcmp(line + 16, "standard") == 0) {
                s->install_profile = 0;
                found = 1;
            }
        } else if (starts_with(line, "usb_media=")) {
            if (strcmp(line + 10, "block") == 0) {
                s->usb_media = 1;
                found = 1;
            } else if (strcmp(line + 10, "allow") == 0) {
                s->usb_media = 0;
                found = 1;
            }
        } else if (starts_with(line, "pin_min_len=")) {
            int v = parse_int_value(line + 12);
            if (v >= 4 && v <= 16) {
                s->pin_min_len = v;
                found = 1;
            }
        } else if (starts_with(line, "pin_require_digit=")) {
            if (strcmp(line + 18, "on") == 0) {
                s->pin_require_digit = 1;
                found = 1;
            } else if (strcmp(line + 18, "off") == 0) {
                s->pin_require_digit = 0;
                found = 1;
            }
        } else if (starts_with(line, "lockout_attempts=")) {
            int v = parse_int_value(line + 17);
            if (v >= 1 && v <= 10) {
                s->lockout_attempts = v;
                found = 1;
            }
        } else if (starts_with(line, "lockout_seconds=")) {
            int v = parse_int_value(line + 16);
            if (v >= 0 && v <= 3600) {
                s->lockout_seconds = v;
                found = 1;
            }
        } else if (starts_with(line, "update_path=")) {
            size_t j = 0;
            while (line[12 + j] != '\0' &&
                   j + 1 < sizeof(s->update_path)) {
                s->update_path[j] = line[12 + j];
                j++;
            }
            s->update_path[j] = '\0';
            found = 1;
        }
    }
    return found;
}
