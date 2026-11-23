/* PrismOS — 小さな JavaScript サブセット。js.h 参照。
 *
 * 字句解析 → 再帰下降で AST (静的プール) → 木をたどる評価。
 * 変数は (名前, 値, スコープ深さ) のスタックで持ち、
 * ブロックを抜けると巻き戻る。関数は名前で引く表に置き、
 * グローバルと引数だけが見える (クロージャなし)。
 */
#include "js.h"

#include <stdint.h>

/* ---------- 共通の小さな道具 ---------- */

static size_t js_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int js_eq(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == b[i];
}

static void js_copy(char *dst, const char *src, size_t cap) {
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

/* ---------- 値 ---------- */

enum { JV_UNDEF, JV_NUM, JV_STR };

typedef struct {
    int type;
    int64_t num;
    char str[160];
} js_value;

static js_value js_num(int64_t n) {
    js_value v;
    v.type = JV_NUM;
    v.num = n;
    v.str[0] = '\0';
    return v;
}

static js_value js_str(const char *s) {
    js_value v;
    v.type = JV_STR;
    v.num = 0;
    js_copy(v.str, s, sizeof(v.str));
    return v;
}

static js_value js_undef(void) {
    js_value v;
    v.type = JV_UNDEF;
    v.num = 0;
    v.str[0] = '\0';
    return v;
}

/* 文字列を数値として読む (先頭の整数部、無ければ 0)。 */
static int64_t js_str_to_num(const char *s) {
    size_t i = 0;
    int neg = 0;
    while (s[i] == ' ' || s[i] == '\t') {
        i++;
    }
    if (s[i] == '-') {
        neg = 1;
        i++;
    }
    int64_t v = 0;
    int any = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
        any = 1;
    }
    if (!any) {
        return 0;
    }
    return neg ? -v : v;
}

static int64_t js_to_num(const js_value *v) {
    if (v->type == JV_NUM) {
        return v->num;
    }
    if (v->type == JV_STR) {
        return js_str_to_num(v->str);
    }
    return 0;
}

static void js_num_to_str(int64_t n, char *out, size_t cap) {
    char tmp[24];
    int i = 0;
    int neg = 0;
    if (n < 0) {
        neg = 1;
        n = -n;
    }
    if (n == 0) {
        tmp[i++] = '0';
    }
    while (n > 0) {
        tmp[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    size_t p = 0;
    if (neg && p + 1 < cap) {
        out[p++] = '-';
    }
    while (i > 0 && p + 1 < cap) {
        out[p++] = tmp[--i];
    }
    out[p] = '\0';
}

/* 値を表示用の文字列にする。 */
static void js_to_str(const js_value *v, char *out, size_t cap) {
    if (v->type == JV_STR) {
        js_copy(out, v->str, cap);
    } else if (v->type == JV_NUM) {
        js_num_to_str(v->num, out, cap);
    } else {
        js_copy(out, "undefined", cap);
    }
}

static int js_truthy(const js_value *v) {
    if (v->type == JV_NUM) {
        return v->num != 0;
    }
    if (v->type == JV_STR) {
        return v->str[0] != '\0';
    }
    return 0;
}

/* ---------- 字句解析 ---------- */

enum {
    TK_EOF,
    TK_NUM,
    TK_STR,
    TK_IDENT,
    TK_PUNCT
};

typedef struct {
    const char *src;
    size_t pos;
    int tok;
    int64_t num;
    char text[160]; /* 文字列リテラル / 識別子 / 記号 */
    char err[96];
} js_lexer;

static int js_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$';
}

static int js_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static void js_lex_skip(js_lexer *lx) {
    for (;;) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            lx->pos++;
            continue;
        }
        if (c == '/' && lx->src[lx->pos + 1] == '/') {
            while (lx->src[lx->pos] != '\0' &&
                   lx->src[lx->pos] != '\n') {
                lx->pos++;
            }
            continue;
        }
        if (c == '/' && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (lx->src[lx->pos] != '\0' &&
                   !(lx->src[lx->pos] == '*' &&
                     lx->src[lx->pos + 1] == '/')) {
                lx->pos++;
            }
            if (lx->src[lx->pos] != '\0') {
                lx->pos += 2;
            }
            continue;
        }
        break;
    }
}

static int js_lex_next(js_lexer *lx) {
    js_lex_skip(lx);
    char c = lx->src[lx->pos];
    if (c == '\0') {
        lx->tok = TK_EOF;
        lx->text[0] = '\0';
        return 0;
    }
    if (js_is_digit(c)) {
        int64_t v = 0;
        while (js_is_digit(lx->src[lx->pos])) {
            v = v * 10 + (lx->src[lx->pos] - '0');
            lx->pos++;
        }
        lx->tok = TK_NUM;
        lx->num = v;
        return 0;
    }
    if (c == '\'' || c == '"') {
        char q = c;
        lx->pos++;
        size_t n = 0;
        while (lx->src[lx->pos] != '\0' &&
               lx->src[lx->pos] != q) {
            char ch = lx->src[lx->pos];
            if (ch == '\\' && lx->src[lx->pos + 1] != '\0') {
                lx->pos++;
                ch = lx->src[lx->pos];
                if (ch == 'n') {
                    ch = '\n';
                } else if (ch == 't') {
                    ch = '\t';
                }
            }
            if (n + 1 < sizeof(lx->text)) {
                lx->text[n++] = ch;
            }
            lx->pos++;
        }
        if (lx->src[lx->pos] != q) {
            js_copy(lx->err, "unterminated string", sizeof(lx->err));
            return -1;
        }
        lx->pos++;
        lx->text[n] = '\0';
        lx->tok = TK_STR;
        return 0;
    }
    if (js_is_alpha(c)) {
        size_t n = 0;
        while (js_is_alpha(lx->src[lx->pos]) ||
               js_is_digit(lx->src[lx->pos])) {
            if (n + 1 < sizeof(lx->text)) {
                lx->text[n++] = lx->src[lx->pos];
            }
            lx->pos++;
        }
        lx->text[n] = '\0';
        lx->tok = TK_IDENT;
        return 0;
    }
    /* 記号 (長いものから)。 */
    static const char *const ops[] = {
        "===", "!==", "==", "!=", "<=", ">=", "&&", "||",
        "++", "--", "+=", "-=", "*=", "/=",
        "+", "-", "*", "/", "%", "<", ">", "=", "!",
        "(", ")", "{", "}", ";", ",", ".",
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        const char *op = ops[i];
        size_t l = js_len(op);
        size_t k = 0;
        while (k < l && lx->src[lx->pos + k] == op[k]) {
            k++;
        }
        if (k == l) {
            js_copy(lx->text, op, sizeof(lx->text));
            lx->pos += l;
            lx->tok = TK_PUNCT;
            return 0;
        }
    }
    js_copy(lx->err, "unexpected character", sizeof(lx->err));
    return -1;
}

/* ---------- AST ---------- */

enum {
    ND_NUM,
    ND_STR,
    ND_IDENT,
    ND_BIN,
    ND_UN,
    ND_CALL,
    ND_MEMBER,
    ND_ASSIGN,
    ND_VAR,
    ND_EXPR,
    ND_BLOCK,
    ND_IF,
    ND_WHILE,
    ND_FOR,
    ND_RETURN,
    ND_FUNC,
    ND_SEQ,
    ND_POSTFIX
};

#define JS_MAX_NODES 4096
#define JS_NAME_CAP 48

typedef struct {
    int kind;
    int op; /* BIN/UN の演算子 (下の OP_*) */
    int64_t num;
    char name[JS_NAME_CAP];  /* 識別子 / メンバ名 / 関数名 */
    char name2[JS_NAME_CAP]; /* メンバのオブジェクト側など */
    char text[160];          /* 文字列リテラル */
    int a, b, c, d;          /* 子ノード (プールの添字、-1 無し) */
} js_node;

enum {
    OP_NONE,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_AND,
    OP_OR,
    OP_NEG,
    OP_NOT,
    OP_PREINC,
    OP_PREDEC
};

static js_node js_nodes[JS_MAX_NODES];
static int js_node_count;

typedef struct {
    js_lexer lx;
    char err[96];
} js_parser;

static int js_new_node(js_parser *ps, int kind) {
    if (js_node_count >= JS_MAX_NODES) {
        js_copy(ps->err, "program too large", sizeof(ps->err));
        return -1;
    }
    int idx = js_node_count++;
    js_node *n = &js_nodes[idx];
    n->kind = kind;
    n->op = OP_NONE;
    n->num = 0;
    n->name[0] = '\0';
    n->name2[0] = '\0';
    n->text[0] = '\0';
    n->a = n->b = n->c = n->d = -1;
    return idx;
}

static int js_p_err(js_parser *ps, const char *msg) {
    if (ps->err[0] == '\0') {
        js_copy(ps->err, msg, sizeof(ps->err));
    }
    return -1;
}

static int js_p_is(js_parser *ps, const char *punct) {
    return ps->lx.tok == TK_PUNCT && js_eq(ps->lx.text, punct);
}

static int js_p_kw(js_parser *ps, const char *kw) {
    return ps->lx.tok == TK_IDENT && js_eq(ps->lx.text, kw);
}

static int js_p_take(js_parser *ps, const char *punct) {
    if (js_p_is(ps, punct)) {
        return js_lex_next(&ps->lx);
    }
    return 1; /* 取らなかった */
}

static int js_p_expect(js_parser *ps, const char *punct) {
    if (!js_p_is(ps, punct)) {
        return js_p_err(ps, "unexpected token");
    }
    return js_lex_next(&ps->lx);
}

static int js_parse_expr(js_parser *ps);
static int js_parse_stmt(js_parser *ps);

static int js_parse_primary(js_parser *ps) {
    if (ps->lx.tok == TK_NUM) {
        int idx = js_new_node(ps, ND_NUM);
        if (idx < 0) {
            return -1;
        }
        js_nodes[idx].num = ps->lx.num;
        if (js_lex_next(&ps->lx) != 0) {
            return js_p_err(ps, ps->lx.err);
        }
        return idx;
    }
    if (ps->lx.tok == TK_STR) {
        int idx = js_new_node(ps, ND_STR);
        if (idx < 0) {
            return -1;
        }
        js_copy(js_nodes[idx].text, ps->lx.text,
                sizeof(js_nodes[idx].text));
        if (js_lex_next(&ps->lx) != 0) {
            return js_p_err(ps, ps->lx.err);
        }
        return idx;
    }
    if (js_p_is(ps, "(")) {
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        int e = js_parse_expr(ps);
        if (e < 0) {
            return -1;
        }
        if (js_p_expect(ps, ")") != 0) {
            return -1;
        }
        return e;
    }
    if (ps->lx.tok == TK_IDENT) {
        int idx = js_new_node(ps, ND_IDENT);
        if (idx < 0) {
            return -1;
        }
        js_copy(js_nodes[idx].name, ps->lx.text,
                sizeof(js_nodes[idx].name));
        if (js_lex_next(&ps->lx) != 0) {
            return js_p_err(ps, ps->lx.err);
        }
        return idx;
    }
    return js_p_err(ps, "expected a value");
}

/* 後置: 呼び出し・メンバ・++ -- */
static int js_parse_postfix(js_parser *ps) {
    int e = js_parse_primary(ps);
    if (e < 0) {
        return -1;
    }
    for (;;) {
        if (js_p_is(ps, "(")) {
            if (js_lex_next(&ps->lx) != 0) {
                return -1;
            }
            int args = -1, tail = -1;
            if (!js_p_is(ps, ")")) {
                for (;;) {
                    int arg = js_parse_expr(ps);
                    if (arg < 0) {
                        return -1;
                    }
                    int link = js_new_node(ps, ND_SEQ);
                    if (link < 0) {
                        return -1;
                    }
                    js_nodes[link].a = arg;
                    js_nodes[link].b = -1;
                    if (tail < 0) {
                        args = link;
                    } else {
                        js_nodes[tail].b = link;
                    }
                    tail = link;
                    if (js_p_take(ps, ",") == 1) {
                        break;
                    }
                }
            }
            if (js_p_expect(ps, ")") != 0) {
                return -1;
            }
            int call = js_new_node(ps, ND_CALL);
            if (call < 0) {
                return -1;
            }
            js_nodes[call].a = e;
            js_nodes[call].b = args;
            e = call;
            continue;
        }
        if (js_p_is(ps, ".")) {
            if (js_lex_next(&ps->lx) != 0) {
                return -1;
            }
            if (ps->lx.tok != TK_IDENT) {
                return js_p_err(ps, "expected a member name");
            }
            int m = js_new_node(ps, ND_MEMBER);
            if (m < 0) {
                return -1;
            }
            js_nodes[m].a = e;
            js_copy(js_nodes[m].name, ps->lx.text,
                    sizeof(js_nodes[m].name));
            if (js_lex_next(&ps->lx) != 0) {
                return js_p_err(ps, ps->lx.err);
            }
            e = m;
            continue;
        }
        if (js_p_is(ps, "++") || js_p_is(ps, "--")) {
            int p = js_new_node(ps, ND_POSTFIX);
            if (p < 0) {
                return -1;
            }
            js_nodes[p].op = js_p_is(ps, "++") ? 1 : -1;
            js_nodes[p].a = e;
            if (js_lex_next(&ps->lx) != 0) {
                return -1;
            }
            e = p;
            continue;
        }
        break;
    }
    return e;
}

static int js_parse_unary(js_parser *ps) {
    if (js_p_is(ps, "-") || js_p_is(ps, "!") || js_p_is(ps, "++") ||
        js_p_is(ps, "--")) {
        int op = js_p_is(ps, "-")    ? OP_NEG
                 : js_p_is(ps, "!")  ? OP_NOT
                 : js_p_is(ps, "++") ? OP_PREINC
                                     : OP_PREDEC;
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        int e = js_parse_unary(ps);
        if (e < 0) {
            return -1;
        }
        int u = js_new_node(ps, ND_UN);
        if (u < 0) {
            return -1;
        }
        js_nodes[u].op = op;
        js_nodes[u].a = e;
        return u;
    }
    return js_parse_postfix(ps);
}

static int js_bin_op(const char *s) {
    if (js_eq(s, "+")) return OP_ADD;
    if (js_eq(s, "-")) return OP_SUB;
    if (js_eq(s, "*")) return OP_MUL;
    if (js_eq(s, "/")) return OP_DIV;
    if (js_eq(s, "%")) return OP_MOD;
    if (js_eq(s, "==") || js_eq(s, "===")) return OP_EQ;
    if (js_eq(s, "!=") || js_eq(s, "!==")) return OP_NE;
    if (js_eq(s, "<")) return OP_LT;
    if (js_eq(s, "<=")) return OP_LE;
    if (js_eq(s, ">")) return OP_GT;
    if (js_eq(s, ">=")) return OP_GE;
    if (js_eq(s, "&&")) return OP_AND;
    if (js_eq(s, "||")) return OP_OR;
    return OP_NONE;
}

static int js_parse_bin(js_parser *ps, int min_prec) {
    int lhs = js_parse_unary(ps);
    if (lhs < 0) {
        return -1;
    }
    for (;;) {
        if (ps->lx.tok != TK_PUNCT) {
            break;
        }
        int op = js_bin_op(ps->lx.text);
        if (op == OP_NONE) {
            break;
        }
        int prec = (op == OP_OR)                     ? 1
                   : (op == OP_AND)                  ? 2
                   : (op == OP_EQ || op == OP_NE)    ? 3
                   : (op == OP_LT || op == OP_LE ||
                      op == OP_GT || op == OP_GE)    ? 4
                   : (op == OP_ADD || op == OP_SUB)  ? 5
                                                     : 6;
        if (prec < min_prec) {
            break;
        }
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        int rhs = js_parse_bin(ps, prec + 1);
        if (rhs < 0) {
            return -1;
        }
        int n = js_new_node(ps, ND_BIN);
        if (n < 0) {
            return -1;
        }
        js_nodes[n].op = op;
        js_nodes[n].a = lhs;
        js_nodes[n].b = rhs;
        lhs = n;
    }
    return lhs;
}

static int js_parse_expr(js_parser *ps) {
    int lhs = js_parse_bin(ps, 1);
    if (lhs < 0) {
        return -1;
    }
    if (js_p_is(ps, "=") || js_p_is(ps, "+=") || js_p_is(ps, "-=") ||
        js_p_is(ps, "*=") || js_p_is(ps, "/=")) {
        int mode = js_p_is(ps, "=")    ? 0
                   : js_p_is(ps, "+=") ? OP_ADD
                   : js_p_is(ps, "-=") ? OP_SUB
                   : js_p_is(ps, "*=") ? OP_MUL
                                       : OP_DIV;
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        int rhs = js_parse_expr(ps);
        if (rhs < 0) {
            return -1;
        }
        int n = js_new_node(ps, ND_ASSIGN);
        if (n < 0) {
            return -1;
        }
        js_nodes[n].op = mode;
        js_nodes[n].a = lhs;
        js_nodes[n].b = rhs;
        return n;
    }
    return lhs;
}

/* 文の並びを ND_SEQ のリストにする。終端は '}' か EOF。
 * 空なら -1、エラーなら -2 を返す。 */
static int js_parse_stmt_list(js_parser *ps) {
    int head = -1, tail = -1;
    while (ps->lx.tok != TK_EOF && !js_p_is(ps, "}")) {
        int s = js_parse_stmt(ps);
        if (s < 0) {
            return -2;
        }
        int link = js_new_node(ps, ND_SEQ);
        if (link < 0) {
            return -1;
        }
        js_nodes[link].a = s;
        js_nodes[link].b = -1;
        if (tail < 0) {
            head = link;
        } else {
            js_nodes[tail].b = link;
        }
        tail = link;
    }
    return head;
}

static int js_parse_block(js_parser *ps) {
    if (js_p_expect(ps, "{") != 0) {
        return -1;
    }
    int body = js_parse_stmt_list(ps);
    if (body == -2) {
        return -1;
    }
    if (js_p_expect(ps, "}") != 0) {
        return -1;
    }
    int blk = js_new_node(ps, ND_BLOCK);
    if (blk < 0) {
        return -1;
    }
    js_nodes[blk].a = body;
    return blk;
}

static int js_parse_stmt(js_parser *ps) {
    if (js_p_is(ps, "{")) {
        return js_parse_block(ps);
    }
    if (js_p_kw(ps, "var") || js_p_kw(ps, "let") ||
        js_p_kw(ps, "const")) {
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        if (ps->lx.tok != TK_IDENT) {
            return js_p_err(ps, "expected a variable name");
        }
        int v = js_new_node(ps, ND_VAR);
        if (v < 0) {
            return -1;
        }
        js_copy(js_nodes[v].name, ps->lx.text,
                sizeof(js_nodes[v].name));
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        if (js_p_take(ps, "=") == 0) {
            int init = js_parse_expr(ps);
            if (init < 0) {
                return -1;
            }
            js_nodes[v].a = init;
        }
        (void)js_p_take(ps, ";");
        return v;
    }
    if (js_p_kw(ps, "function")) {
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        if (ps->lx.tok != TK_IDENT) {
            return js_p_err(ps, "expected a function name");
        }
        int f = js_new_node(ps, ND_FUNC);
        if (f < 0) {
            return -1;
        }
        js_copy(js_nodes[f].name, ps->lx.text,
                sizeof(js_nodes[f].name));
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        if (js_p_expect(ps, "(") != 0) {
            return -1;
        }
        /* 引数名は "a,b,c" の形で text に持つ。 */
        size_t pn = 0;
        js_nodes[f].text[0] = '\0';
        if (!js_p_is(ps, ")")) {
            for (;;) {
                if (ps->lx.tok != TK_IDENT) {
                    return js_p_err(ps, "expected a parameter");
                }
                size_t l = js_len(ps->lx.text);
                if (pn > 0 &&
                    pn + 1 < sizeof(js_nodes[f].text)) {
                    js_nodes[f].text[pn++] = ',';
                }
                for (size_t k = 0; k < l &&
                               pn + 1 < sizeof(js_nodes[f].text);
                     k++) {
                    js_nodes[f].text[pn++] = ps->lx.text[k];
                }
                js_nodes[f].text[pn] = '\0';
                if (js_lex_next(&ps->lx) != 0) {
                    return -1;
                }
                if (js_p_take(ps, ",") == 1) {
                    break;
                }
            }
        }
        if (js_p_expect(ps, ")") != 0) {
            return -1;
        }
        int body = js_parse_block(ps);
        if (body < 0) {
            return -1;
        }
        js_nodes[f].a = body;
        return f;
    }
    if (js_p_kw(ps, "if")) {
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        if (js_p_expect(ps, "(") != 0) {
            return -1;
        }
        int cond = js_parse_expr(ps);
        if (cond < 0) {
            return -1;
        }
        if (js_p_expect(ps, ")") != 0) {
            return -1;
        }
        int then_s = js_parse_stmt(ps);
        if (then_s < 0) {
            return -1;
        }
        int n = js_new_node(ps, ND_IF);
        if (n < 0) {
            return -1;
        }
        js_nodes[n].a = cond;
        js_nodes[n].b = then_s;
        if (js_p_kw(ps, "else")) {
            if (js_lex_next(&ps->lx) != 0) {
                return -1;
            }
            int else_s = js_parse_stmt(ps);
            if (else_s < 0) {
                return -1;
            }
            js_nodes[n].c = else_s;
        }
        return n;
    }
    if (js_p_kw(ps, "while")) {
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        if (js_p_expect(ps, "(") != 0) {
            return -1;
        }
        int cond = js_parse_expr(ps);
        if (cond < 0) {
            return -1;
        }
        if (js_p_expect(ps, ")") != 0) {
            return -1;
        }
        int body = js_parse_stmt(ps);
        if (body < 0) {
            return -1;
        }
        int n = js_new_node(ps, ND_WHILE);
        if (n < 0) {
            return -1;
        }
        js_nodes[n].a = cond;
        js_nodes[n].b = body;
        return n;
    }
    if (js_p_kw(ps, "for")) {
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        if (js_p_expect(ps, "(") != 0) {
            return -1;
        }
        int init = -1;
        if (!js_p_is(ps, ";")) {
            /* var 宣言か式かで分ける: 先読みのため現在の
             * トークンで判断する (var のときは文として)。 */
            if (js_p_kw(ps, "var") || js_p_kw(ps, "let") ||
                js_p_kw(ps, "const")) {
                /* ND_VAR は末尾の ';' を自分で取るので、
                 * ここでは ';' を残したまま解析し、後で
                 * 消費する。 */
                init = js_parse_stmt(ps);
                if (init < 0) {
                    return -1;
                }
                /* js_parse_stmt が ';' を取った場合は
                 * for の区切りが消える。ND_VAR の解析は
                 * ';' を任意としているため、取られていたら
                 * 次は条件から始まる。区別できないので、
                 * ';' が残っていれば消費する。 */
                (void)js_p_take(ps, ";");
            } else {
                init = js_parse_expr(ps);
                if (init < 0) {
                    return -1;
                }
                if (js_p_expect(ps, ";") != 0) {
                    return -1;
                }
            }
        } else {
            if (js_lex_next(&ps->lx) != 0) {
                return -1;
            }
        }
        int cond = -1;
        if (!js_p_is(ps, ";")) {
            cond = js_parse_expr(ps);
            if (cond < 0) {
                return -1;
            }
        }
        if (js_p_expect(ps, ";") != 0) {
            return -1;
        }
        int step = -1;
        if (!js_p_is(ps, ")")) {
            step = js_parse_expr(ps);
            if (step < 0) {
                return -1;
            }
        }
        if (js_p_expect(ps, ")") != 0) {
            return -1;
        }
        int body = js_parse_stmt(ps);
        if (body < 0) {
            return -1;
        }
        int n = js_new_node(ps, ND_FOR);
        if (n < 0) {
            return -1;
        }
        js_nodes[n].a = init;
        js_nodes[n].b = cond;
        js_nodes[n].c = step;
        js_nodes[n].d = body;
        return n;
    }
    if (js_p_kw(ps, "return")) {
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        int n = js_new_node(ps, ND_RETURN);
        if (n < 0) {
            return -1;
        }
        if (!js_p_is(ps, ";") && !js_p_is(ps, "}")) {
            int e = js_parse_expr(ps);
            if (e < 0) {
                return -1;
            }
            js_nodes[n].a = e;
        }
        (void)js_p_take(ps, ";");
        return n;
    }
    if (js_p_is(ps, ";")) {
        if (js_lex_next(&ps->lx) != 0) {
            return -1;
        }
        int n = js_new_node(ps, ND_EXPR);
        if (n < 0) {
            return -1;
        }
        return n;
    }
    /* 式文。 */
    {
        int e = js_parse_expr(ps);
        if (e < 0) {
            return -1;
        }
        (void)js_p_take(ps, ";");
        int n = js_new_node(ps, ND_EXPR);
        if (n < 0) {
            return -1;
        }
        js_nodes[n].a = e;
        return n;
    }
}

/* ---------- 評価 ---------- */

#define JS_MAX_VARS 256
#define JS_MAX_FUNCS 32
#define JS_STEP_LIMIT 2000000L
#define JS_DEPTH_LIMIT 64

typedef struct {
    char name[JS_NAME_CAP];
    js_value val;
    int depth; /* スコープの深さ */
    int frame; /* 呼び出しフレーム (0 = トップレベル) */
} js_var;

typedef struct {
    char name[JS_NAME_CAP];
    int node; /* ND_FUNC */
} js_func_ent;

typedef struct {
    js_var vars[JS_MAX_VARS];
    int var_count;
    js_func_ent funcs[JS_MAX_FUNCS];
    int func_count;
    int scope; /* 現在のスコープ深さ */
    int frame; /* 現在の呼び出しフレーム */
    int frame_seq;
    long steps;
    int call_depth;
    const js_host *host;
    char err[96];
    int returned;
    js_value ret_val;
} js_env;

static int js_fail(js_env *env, const char *msg) {
    if (env->err[0] == '\0') {
        js_copy(env->err, msg, sizeof(env->err));
    }
    return -1;
}

/* 変数を探す: 現在のフレームの変数と、トップレベル
 * (frame 0) の変数だけが見える。他のフレームの
 * ローカルは見えない (クロージャなし)。 */
static js_var *js_find_var(js_env *env, const char *name) {
    for (int i = env->var_count - 1; i >= 0; i--) {
        if (env->vars[i].frame != env->frame &&
            env->vars[i].frame != 0) {
            continue;
        }
        if (js_eq(env->vars[i].name, name)) {
            return &env->vars[i];
        }
    }
    return NULL;
}

static int js_declare(js_env *env, const char *name, js_value v) {
    /* 同じフレーム・同じスコープの同名は置き換える。 */
    for (int i = env->var_count - 1; i >= 0; i--) {
        if (env->vars[i].frame != env->frame ||
            env->vars[i].depth < env->scope) {
            continue;
        }
        if (js_eq(env->vars[i].name, name)) {
            env->vars[i].val = v;
            return 0;
        }
    }
    if (env->var_count >= JS_MAX_VARS) {
        return js_fail(env, "too many variables");
    }
    js_var *slot = &env->vars[env->var_count++];
    js_copy(slot->name, name, sizeof(slot->name));
    slot->val = v;
    slot->depth = env->scope;
    slot->frame = env->frame;
    return 0;
}

static void js_scope_push(js_env *env) {
    env->scope++;
}

static void js_scope_pop(js_env *env) {
    while (env->var_count > 0) {
        js_var *top = &env->vars[env->var_count - 1];
        if (top->frame == env->frame && top->depth >= env->scope) {
            env->var_count--;
        } else {
            break;
        }
    }
    env->scope--;
}

static js_value js_eval(js_env *env, int idx);
static int js_exec(js_env *env, int idx);

static int js_step(js_env *env) {
    if (++env->steps > JS_STEP_LIMIT) {
        return js_fail(env, "step limit exceeded");
    }
    return 0;
}

/* ND_SEQ の引数リストを数える/取り出す。 */
static int js_arg_count(int seq) {
    int n = 0;
    while (seq >= 0) {
        n++;
        seq = js_nodes[seq].b;
    }
    return n;
}

static int js_arg_at(int seq, int i) {
    while (seq >= 0 && i > 0) {
        seq = js_nodes[seq].b;
        i--;
    }
    return seq >= 0 ? js_nodes[seq].a : -1;
}

static js_value js_call_func(js_env *env, int fnode, int argseq) {
    js_node *f = &js_nodes[fnode];
    /* 引数を現在のスコープで評価してから新しいスコープへ。 */
    js_value argv[16];
    int argc = js_arg_count(argseq);
    if (argc > 16) {
        js_fail(env, "too many arguments");
        return js_undef();
    }
    for (int i = 0; i < argc; i++) {
        argv[i] = js_eval(env, js_arg_at(argseq, i));
        if (env->err[0] != '\0') {
            return js_undef();
        }
    }
    if (env->call_depth >= JS_DEPTH_LIMIT) {
        js_fail(env, "call depth exceeded");
        return js_undef();
    }
    /* 新しいフレームで実行する。呼び出し元の変数は
     * スタックに残すが、フレームが違うので見えない。
     * 引数は新しいフレームの深さ 1 で宣言する。 */
    int saved_count = env->var_count;
    int saved_scope = env->scope;
    int saved_frame = env->frame;
    env->frame = ++env->frame_seq;
    env->scope = 1;
    /* 引数名リスト "a,b,c" を分解する。 */
    {
        const char *p = f->text;
        int ai = 0;
        while (*p != '\0' && ai < argc) {
            char pname[JS_NAME_CAP];
            size_t n = 0;
            while (*p != '\0' && *p != ',') {
                if (n + 1 < sizeof(pname)) {
                    pname[n++] = *p;
                }
                p++;
            }
            if (*p == ',') {
                p++;
            }
            pname[n] = '\0';
            if (js_declare(env, pname, argv[ai]) != 0) {
                env->frame = saved_frame;
                env->scope = saved_scope;
                env->var_count = saved_count;
                return js_undef();
            }
            ai++;
        }
    }
    env->call_depth++;
    int saved_returned = env->returned;
    env->returned = 0;
    js_value result = js_undef();
    if (js_exec(env, f->a) == 0 || env->returned) {
        if (env->returned) {
            result = env->ret_val;
        }
    }
    env->returned = saved_returned;
    env->call_depth--;
    /* 後片付け: このフレームの変数を捨てる。ただし
     * 実行中に作られた frame 0 (グローバル) の変数は
     * 残す (詰め直す)。 */
    {
        int w = saved_count;
        for (int i = saved_count; i < env->var_count; i++) {
            if (env->vars[i].frame == 0) {
                env->vars[w++] = env->vars[i];
            }
        }
        env->var_count = w;
    }
    env->frame = saved_frame;
    env->scope = saved_scope;
    return result;
}

static js_value js_eval(js_env *env, int idx) {
    if (idx < 0) {
        return js_undef();
    }
    if (js_step(env) != 0) {
        return js_undef();
    }
    js_node *n = &js_nodes[idx];
    switch (n->kind) {
        case ND_NUM:
            return js_num(n->num);
        case ND_STR:
            return js_str(n->text);
        case ND_IDENT: {
            js_var *v = js_find_var(env, n->name);
            if (v == NULL) {
                js_fail(env, "undefined variable");
                return js_undef();
            }
            return v->val;
        }
        case ND_UN: {
            if (n->op == OP_PREINC || n->op == OP_PREDEC) {
                if (js_nodes[n->a].kind != ND_IDENT) {
                    js_fail(env, "++/-- need a variable");
                    return js_undef();
                }
                js_var *v = js_find_var(env, js_nodes[n->a].name);
                if (v == NULL) {
                    js_fail(env, "undefined variable");
                    return js_undef();
                }
                v->val = js_num(js_to_num(&v->val) +
                                (n->op == OP_PREINC ? 1 : -1));
                return v->val;
            }
            js_value x = js_eval(env, n->a);
            if (n->op == OP_NEG) {
                return js_num(-js_to_num(&x));
            }
            /* OP_NOT */
            return js_num(js_truthy(&x) ? 0 : 1);
        }
        case ND_POSTFIX: {
            if (js_nodes[n->a].kind != ND_IDENT) {
                js_fail(env, "++/-- need a variable");
                return js_undef();
            }
            js_var *v = js_find_var(env, js_nodes[n->a].name);
            if (v == NULL) {
                js_fail(env, "undefined variable");
                return js_undef();
            }
            js_value old = v->val;
            v->val = js_num(js_to_num(&v->val) + n->op);
            return old;
        }
        case ND_BIN: {
            if (n->op == OP_AND) {
                js_value l = js_eval(env, n->a);
                if (!js_truthy(&l)) {
                    return js_num(0);
                }
                js_value r = js_eval(env, n->b);
                return js_num(js_truthy(&r) ? 1 : 0);
            }
            if (n->op == OP_OR) {
                js_value l = js_eval(env, n->a);
                if (js_truthy(&l)) {
                    return js_num(1);
                }
                js_value r = js_eval(env, n->b);
                return js_num(js_truthy(&r) ? 1 : 0);
            }
            js_value l = js_eval(env, n->a);
            js_value r = js_eval(env, n->b);
            if (env->err[0] != '\0') {
                return js_undef();
            }
            if (n->op == OP_ADD &&
                (l.type == JV_STR || r.type == JV_STR)) {
                char ls[176], rs[176], out[160];
                js_to_str(&l, ls, sizeof(ls));
                js_to_str(&r, rs, sizeof(rs));
                size_t p = 0;
                for (size_t i = 0; ls[i] != '\0' && p + 1 < sizeof(out); i++) {
                    out[p++] = ls[i];
                }
                for (size_t i = 0; rs[i] != '\0' && p + 1 < sizeof(out); i++) {
                    out[p++] = rs[i];
                }
                out[p] = '\0';
                return js_str(out);
            }
            if ((n->op == OP_EQ || n->op == OP_NE) &&
                l.type == JV_STR && r.type == JV_STR) {
                int eq = js_eq(l.str, r.str);
                return js_num(n->op == OP_EQ ? eq : !eq);
            }
            if ((n->op == OP_LT || n->op == OP_LE ||
                 n->op == OP_GT || n->op == OP_GE) &&
                l.type == JV_STR && r.type == JV_STR) {
                int cmp = 0;
                size_t i = 0;
                while (l.str[i] != '\0' && r.str[i] != '\0' &&
                       l.str[i] == r.str[i]) {
                    i++;
                }
                cmp = (int)(unsigned char)l.str[i] -
                      (int)(unsigned char)r.str[i];
                switch (n->op) {
                    case OP_LT: return js_num(cmp < 0);
                    case OP_LE: return js_num(cmp <= 0);
                    case OP_GT: return js_num(cmp > 0);
                    default: return js_num(cmp >= 0);
                }
            }
            {
                int64_t a = js_to_num(&l);
                int64_t b = js_to_num(&r);
                switch (n->op) {
                    case OP_ADD: return js_num(a + b);
                    case OP_SUB: return js_num(a - b);
                    case OP_MUL: return js_num(a * b);
                    case OP_DIV:
                        if (b == 0) {
                            js_fail(env, "division by zero");
                            return js_undef();
                        }
                        return js_num(a / b);
                    case OP_MOD:
                        if (b == 0) {
                            js_fail(env, "division by zero");
                            return js_undef();
                        }
                        return js_num(a % b);
                    case OP_EQ: return js_num(a == b);
                    case OP_NE: return js_num(a != b);
                    case OP_LT: return js_num(a < b);
                    case OP_LE: return js_num(a <= b);
                    case OP_GT: return js_num(a > b);
                    case OP_GE: return js_num(a >= b);
                    default: break;
                }
            }
            js_fail(env, "bad operator");
            return js_undef();
        }
        case ND_ASSIGN: {
            js_value rhs = js_eval(env, n->b);
            if (env->err[0] != '\0') {
                return js_undef();
            }
            if (js_nodes[n->a].kind != ND_IDENT) {
                js_fail(env, "invalid assignment target");
                return js_undef();
            }
            const char *name = js_nodes[n->a].name;
            js_var *v = js_find_var(env, name);
            if (v == NULL) {
                /* 未宣言への代入はグローバルを作る
                 * (非 strict の JS と同じ)。 */
                if (js_declare(env, name, js_undef()) != 0) {
                    return js_undef();
                }
                v = js_find_var(env, name);
                /* グローバル (frame 0) として作り直す。 */
                v->frame = 0;
            }
            if (n->op == 0) {
                v->val = rhs;
            } else {
                js_value cur = v->val;
                int64_t a = js_to_num(&cur);
                int64_t b = js_to_num(&rhs);
                int64_t nv = 0;
                switch (n->op) {
                    case OP_ADD:
                        if (cur.type == JV_STR ||
                            rhs.type == JV_STR) {
                            char ls[176], rs[176];
                            js_to_str(&cur, ls, sizeof(ls));
                            js_to_str(&rhs, rs, sizeof(rs));
                            char out[160];
                            size_t p = 0;
                            for (size_t i = 0; ls[i] != '\0' && p + 1 < sizeof(out); i++) {
                                out[p++] = ls[i];
                            }
                            for (size_t i = 0; rs[i] != '\0' && p + 1 < sizeof(out); i++) {
                                out[p++] = rs[i];
                            }
                            out[p] = '\0';
                            v->val = js_str(out);
                            return v->val;
                        }
                        nv = a + b;
                        break;
                    case OP_SUB: nv = a - b; break;
                    case OP_MUL: nv = a * b; break;
                    case OP_DIV:
                        if (b == 0) {
                            js_fail(env, "division by zero");
                            return js_undef();
                        }
                        nv = a / b;
                        break;
                    default:
                        js_fail(env, "bad assignment");
                        return js_undef();
                }
                v->val = js_num(nv);
            }
            return v->val;
        }
        case ND_MEMBER: {
            /* メンバ式は document.write の呼び出し形でのみ
             * 評価される。それ以外は未対応。 */
            js_fail(env, "unsupported member access");
            return js_undef();
        }
        case ND_CALL: {
            js_node *callee = &js_nodes[n->a];
            if (callee->kind == ND_MEMBER &&
                js_nodes[callee->a].kind == ND_IDENT &&
                js_eq(js_nodes[callee->a].name, "document") &&
                js_eq(callee->name, "write")) {
                /* document.write(x): 文字列化してホストへ。 */
                if (n->b >= 0) {
                    js_value arg = js_eval(env, js_arg_at(n->b, 0));
                    char text[176];
                    js_to_str(&arg, text, sizeof(text));
                    if (env->host != NULL &&
                        env->host->write != NULL) {
                        env->host->write(env->host->ctx, text);
                    }
                }
                return js_undef();
            }
            if (callee->kind == ND_IDENT) {
                for (int i = 0; i < env->func_count; i++) {
                    if (js_eq(env->funcs[i].name, callee->name)) {
                        return js_call_func(env,
                                            env->funcs[i].node,
                                            n->b);
                    }
                }
                js_fail(env, "undefined function");
                return js_undef();
            }
            js_fail(env, "unsupported call");
            return js_undef();
        }
        default:
            js_fail(env, "not an expression");
            return js_undef();
    }
}

static int js_exec_seq(js_env *env, int seq) {
    while (seq >= 0) {
        if (js_exec(env, js_nodes[seq].a) != 0) {
            return -1;
        }
        if (env->returned) {
            return 0;
        }
        seq = js_nodes[seq].b;
    }
    return 0;
}

static int js_exec(js_env *env, int idx) {
    if (idx < 0) {
        return 0;
    }
    if (js_step(env) != 0) {
        return -1;
    }
    js_node *n = &js_nodes[idx];
    switch (n->kind) {
        case ND_SEQ:
            return js_exec_seq(env, idx);
        case ND_BLOCK: {
            js_scope_push(env);
            int rc = js_exec_seq(env, n->a);
            js_scope_pop(env);
            return rc;
        }
        case ND_VAR: {
            js_value v = js_undef();
            if (n->a >= 0) {
                v = js_eval(env, n->a);
                if (env->err[0] != '\0') {
                    return -1;
                }
            }
            return js_declare(env, n->name, v);
        }
        case ND_EXPR: {
            if (n->a >= 0) {
                (void)js_eval(env, n->a);
                if (env->err[0] != '\0') {
                    return -1;
                }
            }
            return 0;
        }
        case ND_IF: {
            js_value c = js_eval(env, n->a);
            if (env->err[0] != '\0') {
                return -1;
            }
            if (js_truthy(&c)) {
                return js_exec(env, n->b);
            }
            if (n->c >= 0) {
                return js_exec(env, n->c);
            }
            return 0;
        }
        case ND_WHILE: {
            for (;;) {
                js_value c = js_eval(env, n->a);
                if (env->err[0] != '\0') {
                    return -1;
                }
                if (!js_truthy(&c)) {
                    return 0;
                }
                if (js_exec(env, n->b) != 0) {
                    return -1;
                }
                if (env->returned) {
                    return 0;
                }
            }
        }
        case ND_FOR: {
            js_scope_push(env);
            int rc = 0;
            if (n->a >= 0) {
                rc = (js_nodes[n->a].kind == ND_VAR)
                         ? js_exec(env, n->a)
                         : (js_eval(env, n->a),
                            env->err[0] != '\0' ? -1 : 0);
            }
            while (rc == 0) {
                if (n->b >= 0) {
                    js_value c = js_eval(env, n->b);
                    if (env->err[0] != '\0') {
                        rc = -1;
                        break;
                    }
                    if (!js_truthy(&c)) {
                        break;
                    }
                }
                if (js_exec(env, n->d) != 0) {
                    rc = -1;
                    break;
                }
                if (env->returned) {
                    break;
                }
                if (n->c >= 0) {
                    (void)js_eval(env, n->c);
                    if (env->err[0] != '\0') {
                        rc = -1;
                        break;
                    }
                }
            }
            js_scope_pop(env);
            return rc;
        }
        case ND_RETURN: {
            if (n->a >= 0) {
                env->ret_val = js_eval(env, n->a);
                if (env->err[0] != '\0') {
                    return -1;
                }
            } else {
                env->ret_val = js_undef();
            }
            env->returned = 1;
            return 0;
        }
        case ND_FUNC: {
            /* 関数宣言は登録だけ (巻き上げはしない)。 */
            for (int i = 0; i < env->func_count; i++) {
                if (js_eq(env->funcs[i].name, n->name)) {
                    env->funcs[i].node = idx;
                    return 0;
                }
            }
            if (env->func_count >= JS_MAX_FUNCS) {
                return js_fail(env, "too many functions");
            }
            js_func_ent *f = &env->funcs[env->func_count++];
            js_copy(f->name, n->name, sizeof(f->name));
            f->node = idx;
            return 0;
        }
        default:
            return js_fail(env, "not a statement");
    }
}

int js_run(const char *source, const js_host *host, char *err,
           size_t err_cap) {
    if (err != NULL && err_cap > 0) {
        err[0] = '\0';
    }
    if (source == NULL) {
        if (err != NULL && err_cap > 0) {
            js_copy(err, "no source", err_cap);
        }
        return -1;
    }
    js_node_count = 0;
    js_parser ps;
    ps.err[0] = '\0';
    ps.lx.src = source;
    ps.lx.pos = 0;
    ps.lx.tok = TK_EOF;
    ps.lx.num = 0;
    ps.lx.text[0] = '\0';
    ps.lx.err[0] = '\0';
    if (js_lex_next(&ps.lx) != 0) {
        if (err != NULL && err_cap > 0) {
            js_copy(err, ps.lx.err, err_cap);
        }
        return -1;
    }
    int prog = js_parse_stmt_list(&ps);
    if (prog == -2 || ps.err[0] != '\0') {
        if (err != NULL && err_cap > 0) {
            js_copy(err, ps.err[0] != '\0' ? ps.err : "parse error",
                    err_cap);
        }
        return -1;
    }
    if (ps.lx.tok != TK_EOF) {
        if (err != NULL && err_cap > 0) {
            js_copy(err, "trailing garbage", err_cap);
        }
        return -1;
    }
    static js_env env;
    env.var_count = 0;
    env.func_count = 0;
    env.scope = 0;
    env.frame = 0;
    env.frame_seq = 0;
    env.steps = 0;
    env.call_depth = 0;
    env.host = host;
    env.err[0] = '\0';
    env.returned = 0;
    env.ret_val = js_undef();
    if (js_exec_seq(&env, prog) != 0 || env.err[0] != '\0') {
        if (err != NULL && err_cap > 0) {
            js_copy(err,
                    env.err[0] != '\0' ? env.err : "runtime error",
                    err_cap);
        }
        return -1;
    }
    return 0;
}
