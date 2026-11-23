/* PrismOS - desktop settings (pure logic).
 *
 * The settings the Settings app edits: accent colour, taskbar
 * position and dark/light mode. This file has no hardware or
 * file access in it: it only holds the values and converts them
 * to and from the text form stored in the file "settings.txt".
 * The screen app (desktop.c) owns loading and saving through
 * the file system; the host unit tests cover this file directly.
 *
 * Defaults come from config/theme.toml: accent #4CC2FF,
 * taskbar at the bottom, dark mode.
 */
#ifndef PRISM_SETTINGS_H
#define PRISM_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

/* ショートカットで起動できるアクション (アプリと検索)。
 * 並びは設定画面のショートカットページの表示順でもある。 */
enum {
    PRISM_ACT_TERMINAL = 0,
    PRISM_ACT_FILES,
    PRISM_ACT_SETTINGS,
    PRISM_ACT_BROWSER,
    PRISM_ACT_NOTES,
    PRISM_ACT_CALENDAR,
    PRISM_ACT_SEARCH,
    PRISM_ACT_PHOTOS,
    PRISM_ACT_MUSIC,
    PRISM_ACT_CARDS,
    PRISM_ACT_SHOOTER,
    PRISM_ACT_SHEET,
    PRISM_ACT_VIDEO,
    PRISM_ACT_INSTALLER,
    PRISM_ACT_MAIL,
    PRISM_ACT_DOCS,
    PRISM_ACT_SLIDES,
    PRISM_ACT_CONTACTS,
    PRISM_ACT_EDITOR,
    PRISM_ACT_PAINT,
    PRISM_ACT_STORE,
    PRISM_BIND_COUNT
};

/* アクションの settings.txt 上のキー名 ("terminal" 等) と
 * 設定画面での表示名 ("Terminal" 等)。 */
extern const char *const PRISM_ACTION_KEYS[PRISM_BIND_COUNT];
extern const char *const PRISM_ACTION_NAMES[PRISM_BIND_COUNT];

/* 割り当てを既定値 (F1..F12 と従来の Ctrl 組み合わせ) にする。 */
void settings_binds_defaults(uint8_t *binds);

/* キーコードを "F4" / "C-b" のような名前へ書く。buf に書けた
 * バイト数 (NUL 含まず) を返し、名前のないコードは -1。 */
int settings_key_name(int code, char *buf, size_t cap);

/* 名前からキーコードを返す。解釈できなければ -1。 */
int settings_key_parse(const char *name);

typedef struct {
    uint32_t accent_rgb; /* 0xRRGGBB */
    int taskbar_top;     /* 0 = bottom, 1 = top */
    /* タスクバーの縦配置: 0 = 横 (taskbar_top で上下)、
     * 1 = 左端、2 = 右端。 */
    int taskbar_side;
    int dark_mode;       /* 1 = dark, 0 = light */
    int high_contrast;   /* 1 = ハイコントラスト (アクセシビリティ) */
    /* アクセシビリティ (継続フェーズ)。
     * text_scale: 1 = 標準、2 = 大 (セル寸法とグリフ倍率が
     *   実行時に変わり、レイアウトが追従する)。
     * osk: 1 = 画面キーボードを表示。
     * narrator: 1 = ナレーターのキャプション表示
     *   (通知と画面遷移を画面下部に文字で出し、シリアルにも
     *   記録する読み上げの基礎。音声合成ではない)。 */
    int text_scale;
    int osk;
    int narrator;
    /* 音量 (0-100、%)。SB16 のミキサとサンプル利得に
     * 反映される。既定は 100。 */
    int volume;
    /* 壁紙: 0 = 描画した壁紙、1 = 標準の壁紙画像、
     * 2 = 自分の画像 (wallpaper_file の 24bit BMP)。 */
    int wallpaper_image;
    /* 自分の壁紙画像のファイル名 (VFS/diskfs 上)。無いときは
     * 空文字列。写真アプリで選ぶ。 */
    char wallpaper_file[64];
    int dock_show;       /* 1 = Dock を表示、0 = 隠す (設定画面の k) */
    /* Dock の拡張設定。dock_pos: 0 = 下 (タスクバーの上)、
     * 1 = 左端 (縦置き)。dock_size: 0 = 標準、1 = 大。
     * dock_autohide: 1 = マウスが近づいたときだけ出す。 */
    int dock_pos;
    int dock_size;
    int dock_autohide;
    /* 言語: 0 = 英語、1 = 日本語。初回セットアップと
     * 設定画面で選ぶ。ヘルプと主要な画面表示が切り替わる。 */
    int language;
    /* キーボード配列: 0 = US、1 = JIS。 */
    int kbd_layout;
    /* タイムゾーン: UTC からのオフセット (分)。
     * 既定は +540 (日本)。RTC はローカル時刻を保持し、
     * NTP 同期とメールの Date 表記で使う。 */
    int tz_offset;
    /* 更新の設定 (フェーズ9)。channel: 0 = 安定版、1 = ベータ。
     * auto_security: 1 = セキュリティ更新を起動時に自動で
     * 適用する。host/path が空なら更新サーバは無い。 */
    int update_channel;
    int auto_security;
    char update_host[64];
    char update_path[128];
    /* インストールの構成 (フェーズ11)。0 = 標準 (全ファイル)、
     * 1 = 最小構成 (ブラウザ・ファイル管理・設定だけ)。 */
    int install_profile;
    /* セキュリティ (フェーズ14)。
     * usb_media: 0 = 外部メディアを許可、1 = 禁止 (起動時に
     *   ディスクの利用者ファイルを読まない・書かない)。
     * pin_min_len: PIN の最小長 (4〜16、既定 4)。
     * pin_require_digit: 1 = PIN に数字を 1 つ以上必須。
     * lockout_attempts: 連続失敗の許容回数 (1〜10、既定 3)。
     * lockout_seconds: ロックの秒数 (0〜3600、既定 300)。
     *   0 = ロックせず、回数を超えたら止まる (従来の動作)。 */
    int usb_media;
    int pin_min_len;
    int pin_require_digit;
    int lockout_attempts;
    int lockout_seconds;
    /* 入力デバイス。mouse_speed: カーソル速度 (%、
     * 25-400、既定 100)。key_repeat: 0 = 遅い、1 = 標準、
     * 2 = 速い (PS/2 のタイプラマチック設定)。
     * dblclick_ms: ダブルクリックとみなす間隔 (ms)。 */
    int mouse_speed;
    int key_repeat;
    int dblclick_ms;
    /* 画面の解像度 (実行時変更の永続化)。0 = 既定
     * (ブート時のモードのまま)。res コマンドと設定画面の
     * 解像度切替が保存し、desktop_init が適用する。 */
    int res_w;
    int res_h;
    /* ネットワークの手動設定。net_static: 0 = DHCP、
     * 1 = 固定 IP。アドレス類は "192.168.0.2" 形式の
     * 文字列で保持する (空 = 未設定)。 */
    int net_static;
    char net_ip[16];
    char net_mask[16];
    char net_gw[16];
    char net_dns[16];
    /* ウィジェットの天気・ニュースの取得先 URL
     * (プレーンテキストのフィード)。空 = 未設定。 */
    char feed_weather[128];
    char feed_news[128];
    /* アンインストール済み標準アプリのビットマスク
     * (bit i = desk_standard_apps の i 番目を隠す。
     * 先頭 3 つ (端末/ファイル/設定) は外せない)。 */
    int hidden_apps;
    /* ショートカット (アプリ起動キー) の割り当て。
     * binds[action] がそのアクションを起こすキーコード
     * (keyboard.h の PRISM_KEY_* 特殊コード)。設定画面の
     * ショートカットページで変更でき、settings.txt に
     * bind_<アクション>=<キー名> の形で保存する。 */
    uint8_t binds[PRISM_BIND_COUNT];
} prism_settings;

void settings_defaults(prism_settings *s);

/* The accent colours the Settings app offers. */
#define PRISM_ACCENT_COUNT 7
extern const uint32_t PRISM_ACCENTS[PRISM_ACCENT_COUNT];

/* settings.txt のシリアライズ用バッファの上限。全項目を
 * 最大長にしてもテキストは約 1,600 バイトに収まるため、
 * この大きさがあれば切り捨ては起きない。保存側は必ず
 * この大きさのバッファを使い、戻り値がバッファ長以上
 * なら切り捨てとして保存を止めること (監査 M1)。 */
#define SETTINGS_TEXT_CAP 4096

/* Write the settings as text into buf (NUL-terminated).
 * Returns the number of bytes the full text needs (without
 * the NUL). When that is >= cap, the buffer holds only a
 * truncated prefix and the caller must NOT save it: the
 * tail items (security settings, key bindings) would be
 * lost silently. */
size_t settings_serialize(const prism_settings *s, char *buf, size_t cap);

/* Parse text in the same form. Unknown lines are ignored.
 * Returns 1 if at least one known key was read. */
int settings_parse(prism_settings *s, const char *text, size_t len);

/* Format an accent colour as "#RRGGBB" (needs 8 bytes). */
void settings_accent_hex(uint32_t rgb, char *buf);

/* Field overlays for merged saves. Two holders keep the
 * settings in memory (the shell and the desktop); before
 * saving, each reloads the stored settings and overlays
 * only the fields it owns, so one side's save cannot
 * roll back the other side's changes. The shell owns the
 * security fields; the desktop owns appearance, the
 * install profile and the key bindings. Update fields
 * are written by the update commands, which already
 * reload before they modify. */
void settings_overlay_security(prism_settings *dst,
                               const prism_settings *src);
void settings_overlay_desktop(prism_settings *dst,
                              const prism_settings *src);

#endif /* PRISM_SETTINGS_H */
