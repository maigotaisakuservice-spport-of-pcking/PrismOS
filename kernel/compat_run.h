/* PrismOS — 互換レイヤーのランナー (カーネル側)
 *
 * VFS の実行ファイルを読み、ELF を載せ、スタックを組んで
 * 入口を呼ぶ。ゲストの出力はシリアルと画面の両方に出す。
 * 終了コードを返す。失敗した理由はシリアルに出す。
 *
 * ゲストはサンドボックスで動かす: リング 3 + ゲスト専用の
 * ページテーブル (kernel/sandbox.c)。ゲストがカーネルの
 * メモリを触るとページフォルトになり、ゲストだけが
 * 終わってカーネルは生き続ける。暴走したゲストは
 * ウォッチドッグ (約 5 秒) が止める。
 */
#ifndef PRISM_COMPAT_RUN_H
#define PRISM_COMPAT_RUN_H

#include <stddef.h>
#include <stdint.h>

/* ゲストが異常終了させられたときの終了コード (通常の
 * exit コードと区別できる大きな負数)。 */
#define COMPAT_KILL_FAULT (-1001)
#define COMPAT_KILL_WATCHDOG (-1002)

/* HHDM のずれを覚える (kmain が起動時に呼ぶ)。 */
void compat_set_hhdm(uint64_t hhdm_offset);

/* ゲスト実行中は 1。kernel/sandbox.c が実体を持つ。 */
extern volatile int compat_guest_active;

/* ゲストを見捨ててランナーへ戻る (アセンブリ、戻らない)。
 * 例外ハンドラとウォッチドッグが使う。code は
 * compat_call_entry の戻り値になる。 */
void compat_guest_bail(int code);

/* ゲストのフォルトがゲスト自身の登録範囲内で起きたか
 * (ディスパッチャがゲストのページを触って落ちた場合に
 * ゲストのせいにするための判定)。 */
int compat_guest_addr_in_range(uint64_t addr);

/* VFS の実行ファイルを動かす。0 = 動かした (終了コードを
 * *exit_code に入れる)、-1 = ファイルが無い、-2 = ELF では
 * ない・対応していない、-3 = メモリが足りない。 */
int compat_run_file(const char *name, const char *const *args, int nargs,
                    int *exit_code);

/* 次のゲスト実行で fd 0 から読ませる内容を設定する
 * (シェルのパイプ用)。NULL/0 で解除。 */
void compat_set_stdin(const char *data, size_t len);

/* ELF の情報をテキストで出す。0 = 成功、-1 = ファイルが無い、
 * -2 = ELF ではない・対応していない。 */
int compat_info_file(const char *name, char *out, size_t out_cap);

/* 名前が互換アプリ (".elf" で終わる) かどうか。 */
int compat_is_app(const char *name);

#endif /* PRISM_COMPAT_RUN_H */
