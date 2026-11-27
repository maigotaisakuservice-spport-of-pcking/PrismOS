/* PrismOS — シェルコアのホストテスト
 *
 * カーネルと同じ shell/core.c をホストの gcc でコンパイルして
 * 実行する。QEMU は不要。`make test` で走る。
 * フレームワーク無し: CHECK マクロで数えて最後に集計する。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "vfs.h"

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
    char out[4096];

    /* VFS のファイル内容はホストの malloc/free で確保する
     * (カーネル側では kmalloc/kfree を渡す)。 */
    vfs_set_allocator(malloc, free);
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
    CHECK_CONTAINS(out, "PrismOS version 27H1");
    run(&st, "version", out, sizeof(out));
    CHECK_CONTAINS(out, "PrismOS version 27H1");

    run(&st, "about", out, sizeof(out));
    CHECK_CONTAINS(out, "PrismOS 27H1");
    CHECK_CONTAINS(out, "customizable");

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
    CHECK_CONTAINS(out, "PrismOS 27H1");
    run(&st, "type theme.toml", out, sizeof(out));
    CHECK_CONTAINS(out, "accent");
    run(&st, "cat VERSION", out, sizeof(out));
    CHECK_CONTAINS(out, "27H1");
    run(&st, "cat missing.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "No such file");
    run(&st, "cat", out, sizeof(out));
    CHECK_CONTAINS(out, "usage:");

    /* theme: prints the real settings (defaults here),
     * and follows a changed settings.txt. */
    run(&st, "theme", out, sizeof(out));
    CHECK_CONTAINS(out, "#4CC2FF");
    CHECK_CONTAINS(out, "wallpaper = \"image\"");
    CHECK_CONTAINS(out, "mode = \"dark\"");
    {
        const char *txt = "accent=#FF6FB5\nmode=light\ntaskbar=top\n";
        CHECK(vfs_write("settings.txt", txt, strlen(txt)) == 0);
    }
    run(&st, "theme", out, sizeof(out));
    CHECK_CONTAINS(out, "#FF6FB5");
    CHECK_CONTAINS(out, "mode = \"light\"");
    CHECK_CONTAINS(out, "taskbar_position = \"top\"");
    run(&st, "rm settings.txt", out, sizeof(out));

    /* VFS: echo > で作成・cat で読み・ls で一覧・rm で削除 */
    run(&st, "echo hello prism > note.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "wrote");
    CHECK_CONTAINS(out, "note.txt");
    run(&st, "cat note.txt", out, sizeof(out));
    CHECK(strcmp(out, "hello prism\n") == 0);
    run(&st, "ls", out, sizeof(out));
    CHECK_CONTAINS(out, "note.txt");
    run(&st, "type note.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "hello prism");
    /* 上書き: 古い中身は置き換わる */
    run(&st, "echo second line > note.txt", out, sizeof(out));
    run(&st, "cat note.txt", out, sizeof(out));
    CHECK(strcmp(out, "second line\n") == 0);
    /* touch: 空ファイルを作り、既存ファイルは壊さない */
    run(&st, "touch empty.txt", out, sizeof(out));
    run(&st, "cat empty.txt", out, sizeof(out));
    CHECK(strcmp(out, "\n") == 0);
    run(&st, "touch note.txt", out, sizeof(out));
    run(&st, "cat note.txt", out, sizeof(out));
    CHECK(strcmp(out, "second line\n") == 0);
    /* rm / del */
    run(&st, "rm note.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "removed note.txt");
    run(&st, "cat note.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "No such file");
    run(&st, "ls", out, sizeof(out));
    CHECK(strstr(out, "note.txt") == NULL);
    run(&st, "rm note.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "No such file");
    run(&st, "del empty.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "removed empty.txt");
    /* cp: 複写 (元は残る)、mv: 移動 (元は消える) */
    run(&st, "echo copy me > orig.txt", out, sizeof(out));
    run(&st, "cp orig.txt copy.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "copied orig.txt to copy.txt");
    run(&st, "cat copy.txt", out, sizeof(out));
    CHECK(strcmp(out, "copy me\n") == 0);
    run(&st, "cat orig.txt", out, sizeof(out));
    CHECK(strcmp(out, "copy me\n") == 0);
    run(&st, "cp orig.txt orig.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "same file");
    run(&st, "cp nofile.txt copy.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "No such file");
    run(&st, "cp", out, sizeof(out));
    CHECK_CONTAINS(out, "usage:");
    run(&st, "mv copy.txt moved.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "moved copy.txt to moved.txt");
    run(&st, "cat moved.txt", out, sizeof(out));
    CHECK(strcmp(out, "copy me\n") == 0);
    run(&st, "cat copy.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "No such file");
    run(&st, "mv moved.txt moved.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "same file");
    run(&st, "mv nofile.txt moved.txt", out, sizeof(out));
    CHECK_CONTAINS(out, "No such file");
    run(&st, "mv", out, sizeof(out));
    CHECK_CONTAINS(out, "usage:");
    run(&st, "rm orig.txt", out, sizeof(out));
    run(&st, "rm moved.txt", out, sizeof(out));
    /* 使い方の誤り */
    run(&st, "echo hi >", out, sizeof(out));
    CHECK_CONTAINS(out, "usage:");
    run(&st, "rm", out, sizeof(out));
    CHECK_CONTAINS(out, "usage:");

    /* VFS の上限: 128 件まで。組み込みファイルの件数は
     * 増えることがあるため、空きを数えてから満杯にする。 */
    {
        int room = 128 - vfs_file_count();
        char name[16];
        for (int i = 0; i < room; i++) {
            snprintf(name, sizeof(name), "f%d.txt", i);
            CHECK(vfs_write(name, "x", 1) == 0);
        }
        CHECK(vfs_file_count() == 128);
        CHECK(vfs_write("one-more.txt", "x", 1) == -2);
        static char big[PRISM_VFS_MAX_FILE_SIZE + 1];
        memset(big, 'y', sizeof(big));
        CHECK(vfs_write("README.txt", big, sizeof(big)) == -1);
        CHECK(vfs_file_count() == 128);
        /* 不正な名前 */
        CHECK(vfs_write("../evil", "x", 1) == -3);
        CHECK(vfs_write("a//b", "x", 1) == -3);
        CHECK(vfs_write("/a", "x", 1) == -3);
        CHECK(vfs_write("a/", "x", 1) == -3);
        CHECK(vfs_write("", "x", 1) == -3);
        /* 1 件消せばまた作れる */
        CHECK(vfs_delete("f0.txt") == 0);
        CHECK(vfs_write("new.txt", "ok", 2) == 0);
        CHECK(vfs_file_count() == 128);
        /* ディレクトリ区切りの名前は正規 (サブディレクトリ) */
        CHECK(vfs_delete("new.txt") == 0);
        CHECK(vfs_write("sub/file.txt", "x", 1) == 0);
        CHECK(vfs_delete("sub/file.txt") == 0);
        /* アロケータ未設定では -4 */
        vfs_set_allocator(NULL, NULL);
        CHECK(vfs_write("x.txt", "x", 1) == -4);
        vfs_set_allocator(malloc, free);
        prism_shell_init(&st); /* 組み込みファイルの件数に戻す */
        CHECK(vfs_file_count() == 128 - room);
    }

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
    st.should_reboot = 0;

    /* 長い入力 (parse_line のバッファあふれ回帰テスト)。
     * バッファは 512 バイト。トークンの合計があふれても、書き込みが
     * はみ出さず、出力は切り詰められて、後続のコマンドが壊れないこと。 */
    {
        char long_line[640];
        char needle_a[401];
        size_t i;
        /* "echo " + 'a' x400 + ' ' + 'b' x200 */
        strcpy(long_line, "echo ");
        for (i = 0; i < 400; i++) {
            long_line[5 + i] = 'a';
        }
        long_line[405] = ' ';
        for (i = 0; i < 200; i++) {
            long_line[406 + i] = 'b';
        }
        long_line[606] = '\0';
        for (i = 0; i < 400; i++) {
            needle_a[i] = 'a';
        }
        needle_a[400] = '\0';
        run(&st, long_line, out, sizeof(out));
        /* echo の出力: 'a' x400 + ' ' + 切り詰められた 'b' 群 + '\n'。
         * トークン合計が 512 バイトに収まるよう切り詰められる。 */
        CHECK_CONTAINS(out, needle_a);
        CHECK_CONTAINS(out, "bbb");
        CHECK(strlen(out) <= 512);
        /* 後続のコマンドが正常に動く (スタックの隣が壊れていない) */
        run(&st, "ver", out, sizeof(out));
        CHECK_CONTAINS(out, "PrismOS version 27H1");
    }
    {
        /* 空白なしの 600 文字トークン: 途中で切れて、クラッシュしない */
        char long_line[640];
        size_t i;
        strcpy(long_line, "echo ");
        for (i = 0; i < 600; i++) {
            long_line[5 + i] = 'a';
        }
        long_line[605] = '\0';
        run(&st, long_line, out, sizeof(out));
        CHECK(strlen(out) <= 512);
        run(&st, "help", out, sizeof(out));
        CHECK_CONTAINS(out, "ls");
        /* 言語設定でヘルプが切り替わる (既定は英語のみ、
         * 日本語を選ぶと日本語のヘルプが出る)。 */
        CHECK(strstr(out, "ファイルの一覧を表示する") == NULL);
        st.language = 1;
        run(&st, "help", out, sizeof(out));
        CHECK_CONTAINS(out, "ファイルの一覧を表示する");
        CHECK_CONTAINS(out, "耐量子の鍵交換をする");
        st.language = 0;
    }
    {
        /* 300 文字のトークンは 512 バイトのバッファに収まり、途中で
         * 切れずにそのまま出力される (256 バイト時代は切れていた)。 */
        char long_line[320];
        char needle_a[301];
        size_t i;
        strcpy(long_line, "echo ");
        for (i = 0; i < 300; i++) {
            long_line[5 + i] = 'a';
        }
        long_line[305] = '\0';
        for (i = 0; i < 300; i++) {
            needle_a[i] = 'a';
        }
        needle_a[300] = '\0';
        run(&st, long_line, out, sizeof(out));
        CHECK_CONTAINS(out, needle_a);
        CHECK(strlen(out) == 301); /* 300 文字 + '\n' */
    }
    {
        /* 長い未知コマンド: "Unknown command" が出て、クラッシュしない */
        char long_line[320];
        size_t i;
        for (i = 0; i < 300; i++) {
            long_line[i] = 'z';
        }
        long_line[300] = '\0';
        run(&st, long_line, out, sizeof(out));
        CHECK_CONTAINS(out, "Unknown command");
    }

    /* ディレクトリとゴミ箱 */
    {
        run(&st, "mkdir docs", out, sizeof(out));
        CHECK_CONTAINS(out, "created directory docs");
        run(&st, "mkdir docs", out, sizeof(out));
        CHECK_CONTAINS(out, "already exists");
        run(&st, "cd docs", out, sizeof(out));
        CHECK(strcmp(st.cwd, "/docs") == 0);
        run(&st, "touch a.txt", out, sizeof(out));
        CHECK(vfs_exists("docs/a.txt"));
        run(&st, "ls", out, sizeof(out));
        CHECK_CONTAINS(out, "a.txt");
        CHECK(strstr(out, "note.txt") == NULL);
        run(&st, "cat a.txt", out, sizeof(out));
        CHECK(strstr(out, "No such file") == NULL);
        run(&st, "rmdir docs", out, sizeof(out));
        CHECK_CONTAINS(out, "No such directory"); /* cwd 相対では
                                                     見えない */
        run(&st, "cd ..", out, sizeof(out));
        CHECK(strcmp(st.cwd, "/") == 0);
        run(&st, "rmdir docs", out, sizeof(out));
        CHECK_CONTAINS(out, "directory not empty");
        run(&st, "ls", out, sizeof(out));
        CHECK_CONTAINS(out, "docs/");
        run(&st, "trash docs/a.txt", out, sizeof(out));
        CHECK_CONTAINS(out, "trashed docs/a.txt");
        CHECK(!vfs_exists("docs/a.txt"));
        CHECK(vfs_exists("trash/a.txt"));
        run(&st, "trash list", out, sizeof(out));
        CHECK_CONTAINS(out, "a.txt");
        run(&st, "trash restore a.txt", out, sizeof(out));
        CHECK_CONTAINS(out, "restored a.txt");
        CHECK(vfs_exists("a.txt"));
        run(&st, "trash a.txt", out, sizeof(out));
        run(&st, "trash empty", out, sizeof(out));
        CHECK(!vfs_exists("trash/a.txt"));
        CHECK(!vfs_exists("a.txt"));
        run(&st, "rmdir docs", out, sizeof(out));
        CHECK_CONTAINS(out, "removed directory docs");
    }

    printf("shell tests: %d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
