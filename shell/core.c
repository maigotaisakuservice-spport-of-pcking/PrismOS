/* PrismOS — シェルコア (カーネルとホストテストで共有)
 *
 * 仮想ファイルシステム: v0.1 にはまだディスクドライバが無いため、
 * ls / cat が見るファイルは組み込みの固定データ。
 * ディスク・GUI が来るフェーズでここを実ファイルシステムに
 * 差し替えるのが筋で、コマンド側のインタフェースは変えない。
 */
#include "core.h"

/* ---- 自前の文字列ヘルパー (libc 不使用) ---- */

static size_t s_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int s_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void s_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

/* ---- 出力バッファへの追記 ---- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} out_buf;

static void out_append(out_buf *o, const char *s) {
    while (*s != '\0' && o->len + 1 < o->cap) {
        o->buf[o->len++] = *s++;
    }
    o->buf[o->len] = '\0';
}

static void out_line(out_buf *o, const char *s) {
    out_append(o, s);
    out_append(o, "\n");
}

/* ---- 別名テーブル ---- */

typedef struct {
    const char *alias;
    const char *canonical;
} alias_entry;

static const alias_entry ALIASES[] = {
    { "dir", "ls" },   { "ls", "ls" },
    { "type", "cat" }, { "cat", "cat" },
    { "cls", "clear" }, { "clear", "clear" },
    { "version", "ver" }, { "ver", "ver" },
    { "?", "help" },   { "help", "help" },
    { "echo", "echo" }, { "pwd", "pwd" }, { "cd", "cd" },
    { "about", "about" }, { "theme", "theme" },
    { "reboot", "reboot" }, { "exit", "exit" },
};

const char *prism_shell_canonical(const char *name) {
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
        if (s_eq(name, ALIASES[i].alias)) {
            return ALIASES[i].canonical;
        }
    }
    return NULL;
}

/* ---- 仮想ファイルシステム ---- */

typedef struct {
    const char *name;
    const char *content;
} vfile;

static const vfile VFILES[] = {
    { "README.txt",
      "PrismOS 0.1.0\n"
      "A customizable OS: Windows 11-style UI, Mac-style features,\n"
      "and a shell that speaks both Linux and Windows.\n" },
    { "VERSION", "0.1.0\n" },
    { "theme.toml",
      "[theme]\n"
      "accent = \"#4CC2FF\"\n"
      "wallpaper = \"prism-dusk.png\"\n"
      "taskbar_position = \"bottom\"\n" },
    { "about.txt",
      "PrismOS is built to be customized anywhere:\n"
      "theme, wallpaper, taskbar and Dock from config/theme.toml.\n" },
};

static const vfile *find_file(const char *name) {
    for (size_t i = 0; i < sizeof(VFILES) / sizeof(VFILES[0]); i++) {
        if (s_eq(name, VFILES[i].name)) {
            return &VFILES[i];
        }
    }
    return NULL;
}

/* ---- パーサ: 空白区切り + ダブルクォート対応 ---- */

static int parse_line(const char *line, char *argv_buf, char **argv,
                      int max_argv) {
    int argc = 0;
    size_t pos = 0;
    const char *p = line;

    while (*p != '\0' && argc < max_argv) {
        while (is_space(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc] = argv_buf + pos;
        if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"' &&
                   pos + 1 < PRISM_SHELL_LINE_CAP) {
                argv_buf[pos++] = *p++;
            }
            if (*p == '"') {
                p++;
            }
        } else {
            while (*p != '\0' && !is_space(*p) &&
                   pos + 1 < PRISM_SHELL_LINE_CAP) {
                argv_buf[pos++] = *p++;
            }
        }
        argv_buf[pos++] = '\0';
        argc++;
    }
    return argc;
}

/* ---- コマンド実装 ---- */

static void cmd_help(out_buf *o) {
    out_line(o, "PrismOS shell — Linux & Windows commands work side by side.");
    out_line(o, "");
    out_line(o, "  ls, dir          list files");
    out_line(o, "  cat, type <file> show a file");
    out_line(o, "  pwd              print working directory");
    out_line(o, "  cd <dir>         change directory");
    out_line(o, "  echo <text>      print text");
    out_line(o, "  clear, cls       clear the screen");
    out_line(o, "  ver, version     show PrismOS version");
    out_line(o, "  about            about PrismOS");
    out_line(o, "  theme            show the current theme");
    out_line(o, "  help             show this help");
    out_line(o, "  reboot           reboot the system");
}

static void cmd_about(out_buf *o) {
    out_line(o, "PrismOS 0.1.0");
    out_line(o, "A customizable OS: Windows 11-style UI, Mac-style features");
    out_line(o, "(Dock, Spotlight, Mission Control), and a shell that speaks");
    out_line(o, "both Linux and Windows.");
    out_line(o, "License: MIT — https://github.com/ (see README.md)");
}

static void cmd_ls(out_buf *o, const char *canonical, int argc) {
    (void)argc;
    if (s_eq(canonical, "dir")) {
        /* Windows の dir 風に表示する。ls と中身は同じ仮想 FS。 */
        out_line(o, " Volume in drive C is PRISMOS");
        out_line(o, " Directory of C:\\");
        out_line(o, "");
        for (size_t i = 0; i < sizeof(VFILES) / sizeof(VFILES[0]); i++) {
            out_append(o, "  <FILE>  ");
            out_line(o, VFILES[i].name);
        }
        out_line(o, "");
        out_append(o, "               ");
        /* ファイル数を数字で出す (v0.1 は 4 固定ではなく数える) */
        {
            char num[4];
            size_t n = sizeof(VFILES) / sizeof(VFILES[0]);
            size_t k = 0;
            char rev[4];
            size_t r = 0;
            if (n == 0) {
                rev[r++] = '0';
            }
            while (n > 0) {
                rev[r++] = (char)('0' + (n % 10));
                n /= 10;
            }
            while (r > 0) {
                num[k++] = rev[--r];
            }
            num[k] = '\0';
            out_append(o, num);
        }
        out_line(o, " File(s)");
    } else {
        for (size_t i = 0; i < sizeof(VFILES) / sizeof(VFILES[0]); i++) {
            out_line(o, VFILES[i].name);
        }
    }
}

static void cmd_cat(out_buf *o, int argc, char **argv) {
    if (argc < 2) {
        out_line(o, "usage: cat <file>   (type <file> also works)");
        return;
    }
    const vfile *f = find_file(argv[1]);
    if (f == NULL) {
        out_append(o, "cat: ");
        out_append(o, argv[1]);
        out_line(o, ": No such file");
        return;
    }
    out_append(o, f->content);
    if (s_len(f->content) == 0 ||
        f->content[s_len(f->content) - 1] != '\n') {
        out_append(o, "\n");
    }
}

static void cmd_echo(out_buf *o, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            out_append(o, " ");
        }
        out_append(o, argv[i]);
    }
    out_append(o, "\n");
}

static void cmd_theme(out_buf *o) {
    out_line(o, "[theme]  (from config/theme.toml)");
    out_line(o, "accent = \"#4CC2FF\"");
    out_line(o, "wallpaper = \"prism-dusk.png\"");
    out_line(o, "taskbar_position = \"bottom\"");
    out_line(o, "dock = \"shown\"");
}

/* ---- エントリポイント ---- */

void prism_shell_init(prism_shell_state *state) {
    s_copy(state->cwd, "/", sizeof(state->cwd));
    state->should_reboot = 0;
    state->should_clear = 0;
}

int prism_shell_execute(prism_shell_state *state, const char *line,
                        char *out, size_t out_cap) {
    char argv_buf[PRISM_SHELL_LINE_CAP];
    char *argv[PRISM_SHELL_MAX_ARGS];
    out_buf o = { out, out_cap, 0 };
    if (out_cap > 0) {
        out[0] = '\0';
    }

    int argc = parse_line(line, argv_buf, argv, PRISM_SHELL_MAX_ARGS);
    if (argc == 0) {
        return 0;
    }

    const char *cmd = prism_shell_canonical(argv[0]);
    if (cmd == NULL) {
        out_append(&o, "Unknown command: '");
        out_append(&o, argv[0]);
        out_line(&o, "'");
        out_line(&o, "Type `help` to see available commands.");
        return 0;
    }

    if (s_eq(cmd, "help")) {
        cmd_help(&o);
    } else if (s_eq(cmd, "about")) {
        cmd_about(&o);
    } else if (s_eq(cmd, "ver")) {
        out_line(&o, "PrismOS version 0.1.0");
    } else if (s_eq(cmd, "echo")) {
        cmd_echo(&o, argc, argv);
    } else if (s_eq(cmd, "pwd")) {
        out_line(&o, state->cwd);
    } else if (s_eq(cmd, "ls")) {
        /* 呼び出し名が dir なら Windows 風、ls なら Unix 風に表示 */
        cmd_ls(&o, argv[0], argc);
    } else if (s_eq(cmd, "cat")) {
        cmd_cat(&o, argc, argv);
    } else if (s_eq(cmd, "cd")) {
        if (argc < 2 || s_eq(argv[1], "/") || s_eq(argv[1], ".") ||
            s_eq(argv[1], "..")) {
            s_copy(state->cwd, "/", sizeof(state->cwd));
        } else {
            out_append(&o, "cd: ");
            out_append(&o, argv[1]);
            out_line(&o, ": No such directory");
        }
    } else if (s_eq(cmd, "clear")) {
        state->should_clear = 1;
    } else if (s_eq(cmd, "theme")) {
        cmd_theme(&o);
    } else if (s_eq(cmd, "reboot")) {
        state->should_reboot = 1;
        out_line(&o, "Rebooting...");
    } else if (s_eq(cmd, "exit")) {
        out_line(&o, "There is nowhere to exit to in PrismOS yet. Use `reboot`.");
    }
    return 0;
}
