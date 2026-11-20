/* PrismOS — シェルコアのホストテスト
 *
 * カーネルと同じ shell/core.c をホストの gcc でコンパイルして
 * 実行する。QEMU は不要。`make test` で走る。
 * フレームワーク無し: CHECK マクロで数えて最後に集計する。
 */
#include <stdio.h>
#include <string.h>

#include "core.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

#define CHECK_CONTAINS(out, needle) CHECK(strstr((out), (needle)) != NULL)

static void run(prism_shell_state *st, const char *line, char *out,
                size_t cap) {
    out[0] = '\0';
    prism_shell_execute(st, line, out, cap);
}

int main(void) {
    prism_shell_state st;
    char out[2048];

    prism_shell_init(&st);
    CHECK(strcmp(st.cwd, "/") == 0);
    CHECK(st.should_reboot == 0);
    CHECK(st.should_clear == 0);

    /* 別名テーブル */
    CHECK(strcmp(prism_shell_canonical("dir"), "ls") == 0);
    CHECK(strcmp(prism_shell_canonical("ls"), "ls") == 0);
    CHECK(strcmp(prism_shell_canonical("type"), "cat") == 0);
    CHECK(strcmp(prism_shell_canonical("cls"), "clear") == 0);
    CHECK(strcmp(prism_shell_canonical("version"), "ver") == 0);
    CHECK(strcmp(prism_shell_canonical("?"), "help") == 0);
    CHECK(prism_shell_canonical("nosuchcommand") == NULL);

    /* help / ver / about */
    run(&st, "help", out, sizeof(out));
    CHECK_CONTAINS(out, "ls");
    CHECK_CONTAINS(out, "dir");
    CHECK_CONTAINS(out, "reboot");

    run(&st, "ver", out, sizeof(out));
    CHECK_CONTAINS(out, "PrismOS version 0.1.0");
    run(&st, "version", out, sizeof(out));
    CHECK_CONTAINS(out, "PrismOS version 0.1.0");

    run(&st, "about", out, sizeof(out));
    CHECK_CONTAINS(out, "PrismOS 0.1.0");
    CHECK_CONTAINS(out, "Windows 11");

    /* echo (クォート対応も) */
    run(&st, "echo hello world", out, sizeof(out));
    CHECK(strcmp(out, "hello world\n") == 0);
    run(&st, "echo \"hello world\"", out, sizeof(out));
    CHECK(strcmp(out, "hello world\n") == 0);
    run(&st, "echo", out, sizeof(out));
    CHECK(strcmp(out, "\n") == 0);

    /* pwd / cd */
    run(&st, "pwd", out, sizeof(out));
    CHECK(strcmp(out, "/\n") == 0);
    run(&st, "cd /home", out, sizeof(out));
    CHECK_CONTAINS(out, "No such directory");
    run(&st, "cd /", out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    run(&st, "pwd", out, sizeof(out));
    CHECK(strcmp(out, "/\n") == 0);

    /* ls / dir (仮想 FS) */
    run(&st, "ls", out, sizeof(out));
    CHECK_CONTAINS(out, "README.txt");
    CHECK_CONTAINS(out, "VERSION");
    CHECK_CONTAINS(out, "theme.toml");
    CHECK_CONTAINS(out, "about.txt");

    run(&st, "dir", out, sizeof(out));
    CHECK_CONTAINS(out, "Directory of C:");
    CHECK_CONTAINS(out, "README.txt");
    CHECK_CONTAINS(out, "File(s)");

    /* cat / type */
    run(&st, "cat README.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "PrismOS 0.1.0");
    run(&st, "type theme.toml", out, sizeof(out));
    CHECK_CONTAINS(out, "accent");
    run(&st, "cat VERSION", out, sizeof(out));
    CHECK_CONTAINS(out, "0.1.0");
    run(&st, "cat missing.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "No such file");
    run(&st, "cat", out, sizeof(out));
    CHECK_CONTAINS(out, "usage:");

    /* theme */
    run(&st, "theme", out, sizeof(out));
    CHECK_CONTAINS(out, "#4CC2FF");
    CHECK_CONTAINS(out, "prism-dusk.png");

    /* clear / cls はフラグだけ立てて出力を出さない */
    run(&st, "clear", out, sizeof(out));
    CHECK(st.should_clear == 1);
    CHECK(strcmp(out, "") == 0);
    st.should_clear = 0;
    run(&st, "cls", out, sizeof(out));
    CHECK(st.should_clear == 1);
    st.should_clear = 0;

    /* 空行・不明なコマンド・exit */
    run(&st, "", out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    run(&st, "   ", out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    run(&st, "frobnicate", out, sizeof(out));
    CHECK_CONTAINS(out, "Unknown command");
    CHECK_CONTAINS(out, "frobnicate");
    run(&st, "exit", out, sizeof(out));
    CHECK_CONTAINS(out, "reboot");

    /* reboot はフラグを立てる */
    run(&st, "reboot", out, sizeof(out));
    CHECK(st.should_reboot == 1);
    CHECK_CONTAINS(out, "Rebooting");

    printf("shell tests: %d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
