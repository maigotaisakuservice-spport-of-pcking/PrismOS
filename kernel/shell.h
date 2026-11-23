/* PrismOS — シリアルシェルのカーネル側ラッパー */
#ifndef PRISM_SHELL_H
#define PRISM_SHELL_H

#include <stddef.h>

/* シリアルシェルに入り、戻らない。 */
void shell_run(void);

/* 今すぐ再起動する (キーボードコントローラ経由のリセット)。
 * 戻らない。 */
void shell_reboot_now(void);

/* ユーザー切替: デスクトップの電源メニューから呼ばれる。
 * シェルのメインループが次の周期でログイン画面へ戻る。 */
void shell_request_user_switch(void);

/* 言語の変更通知: 設定アプリが言語を切り替えて保存した
 * ときに呼ばれる (保存経路から)。シェルのメインループが
 * 次のコマンド実行前に、実行中セッションの表示言語
 * (state.language) へ反映する。再起動しなくても help の
 * 言語が切り替わるようにするための窓口。 */
void shell_notify_language_changed(int language);

/* ---- デスクトップ設定アプリとの共用 ----
 * 設定アプリ (アカウント/セキュリティ/更新ページ) が、
 * シェルと同じ作法でユーザー管理と更新を行うための窓口。
 * 実体は shell.c 側にあり、状態 (現在のユーザーや
 * セッション種別) はシェルだけが持つ。 */

/* 現在のユーザー名 ("admin" / 追加ユーザー名 / "guest")。 */
const char *shell_current_user_name(void);

/* ゲストセッションなら 1。 */
int shell_session_is_guest(void);

/* 現在のユーザーが管理者なら 1 (主アカウント、または
 * 役割が admin の追加ユーザー)。ゲストは常に 0。 */
int shell_current_user_is_admin(void);

/* 管理者 PIN (主アカウントの PIN) の照合。対話入出力の
 * ある admin_check と同じ照合を、入力された PIN で行う。
 * 1 = 合う、0 = 合わない / PIN 未設定。 */
int shell_admin_pin_verify(const char *pin);

/* 更新チェックで分かったマニフェストの要約。 */
typedef struct {
    char version[16];
    char type[16];    /* "security" / "feature" */
    char channel[16]; /* "stable" / "beta" */
    int has_kernel;
    int file_count;
} shell_update_info;

/* shell_update_check の戻り値。 */
#define SHELL_UPDATE_UP_TO_DATE 0
#define SHELL_UPDATE_AVAILABLE 1
#define SHELL_UPDATE_NO_SERVER (-1)
#define SHELL_UPDATE_UNREACHABLE (-2)
#define SHELL_UPDATE_BAD_SIGNATURE (-3)
#define SHELL_UPDATE_BAD_MANIFEST (-4)

/* 更新の確認: 設定を読み直し、マニフェスト取得 → ML-DSA
 * 署名検証 → 解析 → 版比較までを `update check` と同じ
 * 経路で行う。SHELL_UPDATE_AVAILABLE のとき info に要約が
 * 入り、続けて shell_update_apply_pending() で適用できる。 */
int shell_update_check(shell_update_info *info);

/* 直前の shell_update_check で見つけた更新を適用する
 * (復元ポイント作成と巻き戻しを含む update_apply_manifest
 * そのもの)。0 = 適用した、-1 = 失敗 / 未チェック。 */
int shell_update_apply_pending(void);

/* いちばん新しい pre-update-* 復元ポイントへ戻す。
 * 0 = 戻した (name_out に復元ポイント名)、
 * -1 = 復元ポイントが無い、-2 = 更新用が無い、
 * -3 = 復元に失敗した。 */
int shell_update_rollback(char *name_out, size_t cap);

/* 管理画面 (設定アプリ) 用の薄いラッパー。実装はシェルの
 * driver / fw / snapshot と同じ内部処理を使う。 */
int shell_driver_name_disabled(const char *name);
void shell_driver_set_disabled(const char *name, int disable);
void shell_fw_save(void);
void shell_snapshot_auto_name(char *out, size_t cap);

#endif /* PRISM_SHELL_H */
