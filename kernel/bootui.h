/* PrismOS — 起動時グラフィカル画面 (bootui)
 *
 * デスクトップが始まる前の画面 (ディスク暗号化の解除、
 * ログイン、回復メニュー) をフレームバッファへ描くための
 * 小さな部品。入力そのものは従来どおりシリアル/キーボードの
 * 行入力が持ち、bootui はその内容を画面へ映す (ミラー) 役に
 * 徹する。回復メニューだけは bootui 自身が選択状態を持つ。
 *
 * 言語は bootui_set_language で渡す (0 = 英語、1 = 日本語)。
 * FDE 解除のように設定を読む前の画面では、タイトルを
 * 日本語、サブタイトルを英語にする二言語表示もできる
 * (呼び出し側が文字列を選ぶ)。
 */
#ifndef PRISM_BOOTUI_H
#define PRISM_BOOTUI_H

/* 画面が使えるか (フレームバッファ準備済みか)。 */
int bootui_available(void);

/* 表示言語を設定する (0 = 英語、1 = 日本語)。 */
void bootui_set_language(int lang);

/* ---- プロンプト画面 (FDE 解除 / ログイン / TOTP) ----
 * タイトル・サブタイトル・入力欄ラベルを描き、入力欄と
 * メッセージ行を空にする。 */
void bootui_prompt(const char *title_en, const char *title_ja,
                   const char *sub_en, const char *sub_ja,
                   const char *label_en, const char *label_ja);

/* 入力欄を len 個のドットで描く (秘密入力用)。 */
void bootui_field_dots(int len);

/* 入力欄に文字列を描く (ユーザー名など)。 */
void bootui_field_text(const char *text);

/* 入力欄の下にメッセージを描く (is_error != 0 で赤)。 */
void bootui_message(const char *en, const char *ja, int is_error);

/* ---- ログインのユーザー一覧 ----
 * プロンプト画面の入力欄の上に、ユーザー選択チップ
 * (頭文字アイコン + 名前) を最大 8 件まで描く。
 * selected が選ばれている行 (-1 でハイライト無し)。 */
void bootui_login_users(const char *const *names, int count,
                        int selected);

/* ---- メニュー画面 (回復モード) ----
 * タイトルと項目一覧を描く。selected の行を反転する。
 * 項目は最大 8 件。 */
void bootui_menu(const char *title_en, const char *title_ja,
                 const char *const *items_en,
                 const char *const *items_ja, int count, int selected);

/* メニュー画面の下部に複数行のテキストを描く (復元ポイント
 * 一覧や結果表示用)。最大 8 行。 */
void bootui_text_lines(const char *const *lines, int count);

/* マウスカーソルを (x, y) に描く (回復メニューのクリック用)。
 * 描く前に bootui_menu 等で背景を描き直す前提。 */
void bootui_draw_cursor(int x, int y);

/* メニュー項目のヒットテスト: (x, y) が項目の上なら
 * その番号、そうでなければ -1 を返す。 */
int bootui_menu_hit(int x, int y);

#endif /* PRISM_BOOTUI_H */
