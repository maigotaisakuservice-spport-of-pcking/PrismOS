/* PrismOS — シェルコア (カーネルとホストテストで共有)
 *
 * 設計の肝: このファイルは libc に依存しない。使うのは
 * <stddef.h> / <stdint.h> の型だけで、文字列操作は自前。
 * そのためカーネル (-nostdlib) でも、ホストのユニットテスト
 * (tests/test_shell.c) でも同じソースをコンパイルできる。
 *
 * 2つのコマンド体系に対応: 各コマンドは正規名を持ち、別名でも
 * 動く。ls=dir、cat=type、clear=cls、ver=version、など。
 */
#ifndef PRISM_SHELL_CORE_H
#define PRISM_SHELL_CORE_H

#include <stddef.h>
#include <stdint.h>

#define PRISM_SHELL_MAX_ARGS 16
#define PRISM_SHELL_LINE_CAP 512

typedef struct {
    char cwd[64];
    int should_reboot; /* reboot コマンドが実行された */
    int should_clear;  /* clear / cls が実行された */
    int language;      /* 0 = 英語、1 = 日本語 (help の言語) */
} prism_shell_state;

void prism_shell_init(prism_shell_state *state);

/* 1 行を実行し、結果を out (NUL 終端) に書く。
 * 戻り値は常に 0。reboot / clear は state のフラグで知らせる。
 * 不明なコマンドもエラーではなくメッセージを out に書く。 */
int prism_shell_execute(prism_shell_state *state, const char *line,
                        char *out, size_t out_cap);

/* 別名を正規名に変換する。未知の名前は NULL を返す。 */
const char *prism_shell_canonical(const char *name);

#endif /* PRISM_SHELL_CORE_H */
