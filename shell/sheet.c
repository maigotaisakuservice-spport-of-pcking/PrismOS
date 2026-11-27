/* PrismOS — 表計算の純粋ロジック (sheet.h を見ること)。 */
#include "sheet.h"

static int sheet_idx(int col, int row) {
    return row * SHEET_COLS + col;
}

static int sheet_in_range(int col, int row) {
    return col >= 0 && col < SHEET_COLS && row >= 0 && row < SHEET_ROWS;
}

void sheet_clear(sheet_state *s) {
    for (int i = 0; i < SHEET_ROWS * SHEET_COLS; i++) {
        s->cells[i].raw[0] = '\0';
        s->cells[i].state = 0;
        s->cells[i].kind = 0;
        s->cells[i].value = 0;
        s->cells[i].err[0] = '\0';
    }
}

/* ---- 数値の表示 ---- */

int sheet_format_milli(int64_t milli, char *buf, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    int neg = 0;
    uint64_t mag;
    if (milli < 0) {
        neg = 1;
        /* INT64_MIN の絶対値は unsigned で取る。 */
        mag = (uint64_t)(-(milli + 1)) + 1;
    } else {
        mag = (uint64_t)milli;
    }
    uint64_t ip = mag / 1000;
    uint64_t fp = mag % 1000;
    char tmp[32];
    size_t n = 0;
    if (neg) {
        tmp[n++] = '-';
    }
    /* 整数部。 */
    char rev[21];
    int ri = 0;
    if (ip == 0) {
        rev[ri++] = '0';
    } else {
        while (ip > 0 && ri < 20) {
            rev[ri++] = (char)('0' + (ip % 10));
            ip /= 10;
        }
    }
    while (ri > 0 && n + 1 < sizeof(tmp)) {
        tmp[n++] = rev[--ri];
    }
    /* 小数部: 末尾の 0 を落とす。 */
    if (fp != 0) {
        char frac[4];
        frac[0] = (char)('0' + (fp / 100));
        frac[1] = (char)('0' + ((fp / 10) % 10));
        frac[2] = (char)('0' + (fp % 10));
        int fl = 3;
        while (fl > 0 && frac[fl - 1] == '0') {
            fl--;
        }
        if (n + 1 < sizeof(tmp)) {
            tmp[n++] = '.';
        }
        for (int i = 0; i < fl && n + 1 < sizeof(tmp); i++) {
            tmp[n++] = frac[i];
        }
    }
    tmp[n] = '\0';
    size_t out = 0;
    while (tmp[out] != '\0' && out + 1 < cap) {
        buf[out] = tmp[out];
        out++;
    }
    buf[out] = '\0';
    return (int)out;
}

/* ---- 数式のパーサ (再帰下降) ---- */

enum {
    P_OK = 0,
    P_DIV0 = 1,
    P_CIRC = 2,
    P_ERR = 3,
    P_NUM = 4
};

typedef struct {
    const char *p;
    sheet_state *s;
    int err; /* 上の P_* */
} parser;

/* 前方宣言: セル参照の評価がパーサからセル評価を呼ぶ。 */
static int sheet_eval_cell(sheet_state *s, int col, int row, int64_t *out,
                           char *err_out, size_t err_cap);

static void p_skip(parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t') {
        ps->p++;
    }
}

static int64_t p_expr(parser *ps);

static int p_number(parser *ps, int64_t *out) {
    /* 整数部。 */
    if (*ps->p < '0' || *ps->p > '9') {
        ps->err = P_ERR;
        return 0;
    }
    int64_t ip = 0;
    while (*ps->p >= '0' && *ps->p <= '9') {
        int d = *ps->p - '0';
        if (ip > (INT64_MAX - d) / 10) {
            ps->err = P_NUM;
            return 0;
        }
        ip = ip * 10 + d;
        ps->p++;
    }
    int64_t milli;
    if (__builtin_mul_overflow(ip, 1000, &milli)) {
        ps->err = P_NUM;
        return 0;
    }
    if (*ps->p == '.') {
        ps->p++;
        int64_t frac = 0;
        int digits = 0;
        while (*ps->p >= '0' && *ps->p <= '9') {
            if (digits < 3) {
                frac = frac * 10 + (*ps->p - '0');
                digits++;
            }
            /* 4 桁目以降は切り捨てる。 */
            ps->p++;
        }
        while (digits < 3) {
            frac *= 10;
            digits++;
        }
        int64_t add;
        if (__builtin_add_overflow(milli, frac, &add)) {
            ps->err = P_NUM;
            return 0;
        }
        milli = add;
    }
    *out = milli;
    return 1;
}

/* セル参照を読む: A1〜J20 (小文字も可)。成功したら 1。 */
static int p_cellref(parser *ps, int *col, int *row) {
    char c = *ps->p;
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c < 'A' || c >= 'A' + SHEET_COLS) {
        return 0;
    }
    const char *save = ps->p;
    ps->p++;
    if (*ps->p < '1' || *ps->p > '9') {
        ps->p = save;
        return 0;
    }
    int r = 0;
    while (*ps->p >= '0' && *ps->p <= '9') {
        r = r * 10 + (*ps->p - '0');
        ps->p++;
    }
    if (r < 1 || r > SHEET_ROWS) {
        ps->p = save;
        return 0;
    }
    *col = c - 'A';
    *row = r - 1;
    return 1;
}

static int64_t p_ref_value(parser *ps, int col, int row) {
    int64_t v = 0;
    char err[8];
    int kind = sheet_eval_cell(ps->s, col, row, &v, err, sizeof(err));
    if (kind == 3) {
        /* 参照先のエラーをそのまま伝える。 */
        if (err[0] == '#') {
            if (err[1] == 'D') {
                ps->err = P_DIV0;
            } else if (err[1] == 'C') {
                ps->err = P_CIRC;
            } else if (err[1] == 'N') {
                ps->err = P_NUM;
            } else {
                ps->err = P_ERR;
            }
        } else {
            ps->err = P_ERR;
        }
        return 0;
    }
    if (kind == 2) {
        /* 文字列のセルは数式の中では 0 として扱わない: 書式誤り。 */
        ps->err = P_ERR;
        return 0;
    }
    return v;
}

/* 範囲の集計。func: 0 = SUM, 1 = AVG, 2 = MIN, 3 = MAX。 */
static int64_t p_range(parser *ps, int func, int c0, int r0, int c1,
                       int r1) {
    int64_t sum = 0;
    int64_t best = 0;
    int count = 0;
    int have = 0;
    if (c0 > c1) {
        int t = c0;
        c0 = c1;
        c1 = t;
    }
    if (r0 > r1) {
        int t = r0;
        r0 = r1;
        r1 = t;
    }
    for (int r = r0; r <= r1 && ps->err == P_OK; r++) {
        for (int c = c0; c <= c1 && ps->err == P_OK; c++) {
            int64_t v = 0;
            char err[8];
            int kind = sheet_eval_cell(ps->s, c, r, &v, err, sizeof(err));
            if (kind == 3) {
                if (err[1] == 'D') {
                    ps->err = P_DIV0;
                } else if (err[1] == 'C') {
                    ps->err = P_CIRC;
                } else if (err[1] == 'N') {
                    ps->err = P_NUM;
                } else {
                    ps->err = P_ERR;
                }
                return 0;
            }
            if (kind != 1) {
                continue; /* 空・文字列は集計に入れない */
            }
            if (!have) {
                best = v;
                have = 1;
            } else if (func == 2 && v < best) {
                best = v;
            } else if (func == 3 && v > best) {
                best = v;
            }
            if (__builtin_add_overflow(sum, v, &sum)) {
                ps->err = P_NUM;
                return 0;
            }
            count++;
        }
    }
    if (ps->err != P_OK) {
        return 0;
    }
    if (func == 0) {
        return sum;
    }
    if (func == 1) {
        if (count == 0) {
            ps->err = P_DIV0;
            return 0;
        }
        return sum / count;
    }
    /* MIN / MAX: 数値が 1 つも無ければ 0。 */
    return have ? best : 0;
}

static int64_t p_factor(parser *ps) {
    p_skip(ps);
    if (ps->err != P_OK) {
        return 0;
    }
    if (*ps->p == '-') {
        ps->p++;
        int64_t v = p_factor(ps);
        if (ps->err != P_OK) {
            return 0;
        }
        if (v == INT64_MIN) {
            ps->err = P_NUM;
            return 0;
        }
        return -v;
    }
    if (*ps->p == '+') {
        ps->p++;
        return p_factor(ps);
    }
    if (*ps->p == '(') {
        ps->p++;
        int64_t v = p_expr(ps);
        p_skip(ps);
        if (*ps->p != ')') {
            ps->err = P_ERR;
            return 0;
        }
        ps->p++;
        return v;
    }
    if (*ps->p >= '0' && *ps->p <= '9') {
        int64_t v = 0;
        if (!p_number(ps, &v)) {
            return 0;
        }
        return v;
    }
    /* 関数名かセル参照。 */
    if ((*ps->p >= 'A' && *ps->p <= 'Z') ||
        (*ps->p >= 'a' && *ps->p <= 'z')) {
        /* 関数名を読む (英字の並び)。 */
        const char *start = ps->p;
        while ((*ps->p >= 'A' && *ps->p <= 'Z') ||
               (*ps->p >= 'a' && *ps->p <= 'z')) {
            ps->p++;
        }
        size_t n = (size_t)(ps->p - start);
        int func = -1;
        if (n == 3) {
            char up[4];
            for (size_t i = 0; i < 3; i++) {
                char c = start[i];
                if (c >= 'a' && c <= 'z') {
                    c = (char)(c - 'a' + 'A');
                }
                up[i] = c;
            }
            up[3] = '\0';
            if (up[0] == 'S' && up[1] == 'U' && up[2] == 'M') {
                func = 0;
            } else if (up[0] == 'A' && up[1] == 'V' && up[2] == 'G') {
                func = 1;
            } else if (up[0] == 'M' && up[1] == 'I' && up[2] == 'N') {
                func = 2;
            } else if (up[0] == 'M' && up[1] == 'A' && up[2] == 'X') {
                func = 3;
            }
        }
        if (func >= 0) {
            p_skip(ps);
            if (*ps->p != '(') {
                ps->err = P_ERR;
                return 0;
            }
            ps->p++;
            p_skip(ps);
            int c0 = 0, r0 = 0, c1 = 0, r1 = 0;
            if (!p_cellref(ps, &c0, &r0)) {
                ps->err = P_ERR;
                return 0;
            }
            c1 = c0;
            r1 = r0;
            p_skip(ps);
            if (*ps->p == ':') {
                ps->p++;
                p_skip(ps);
                if (!p_cellref(ps, &c1, &r1)) {
                    ps->err = P_ERR;
                    return 0;
                }
                p_skip(ps);
            }
            if (*ps->p != ')') {
                ps->err = P_ERR;
                return 0;
            }
            ps->p++;
            return p_range(ps, func, c0, r0, c1, r1);
        }
        /* 関数でなければセル参照のはず: 1 文字の列 + 行番号。 */
        if (n == 1) {
            ps->p = start;
            int col = 0, row = 0;
            if (!p_cellref(ps, &col, &row)) {
                ps->err = P_ERR;
                return 0;
            }
            return p_ref_value(ps, col, row);
        }
        ps->err = P_ERR;
        return 0;
    }
    ps->err = P_ERR;
    return 0;
}

static int64_t p_term(parser *ps) {
    int64_t v = p_factor(ps);
    for (;;) {
        p_skip(ps);
        if (ps->err != P_OK) {
            return 0;
        }
        if (*ps->p == '*') {
            ps->p++;
            int64_t rhs = p_factor(ps);
            if (ps->err != P_OK) {
                return 0;
            }
            int64_t prod;
            if (__builtin_mul_overflow(v, rhs, &prod)) {
                ps->err = P_NUM;
                return 0;
            }
            v = prod / 1000;
        } else if (*ps->p == '/') {
            ps->p++;
            int64_t rhs = p_factor(ps);
            if (ps->err != P_OK) {
                return 0;
            }
            if (rhs == 0) {
                ps->err = P_DIV0;
                return 0;
            }
            /* v / rhs (実数) を milli で: (v * 1000) / rhs。
             * v は milli なので、実数値の割り算は v / rhs、
             * milli に直すと v * 1000 / rhs。 */
            if (v > INT64_MAX / 1000 || v < INT64_MIN / 1000) {
                ps->err = P_NUM;
                return 0;
            }
            v = (v * 1000) / rhs;
        } else {
            return v;
        }
    }
}

static int64_t p_expr(parser *ps) {
    int64_t v = p_term(ps);
    for (;;) {
        p_skip(ps);
        if (ps->err != P_OK) {
            return 0;
        }
        if (*ps->p == '+') {
            ps->p++;
            int64_t rhs = p_term(ps);
            if (ps->err != P_OK) {
                return 0;
            }
            if (__builtin_add_overflow(v, rhs, &v)) {
                ps->err = P_NUM;
                return 0;
            }
        } else if (*ps->p == '-') {
            ps->p++;
            int64_t rhs = p_term(ps);
            if (ps->err != P_OK) {
                return 0;
            }
            if (__builtin_sub_overflow(v, rhs, &v)) {
                ps->err = P_NUM;
                return 0;
            }
        } else {
            return v;
        }
    }
}

/* ---- セルの評価 ---- */

/* 戻り値: 0 = 空、1 = 数値 (out に milli)、2 = 文字列、3 = エラー
 * (err_out に "#DIV/0!" など)。 */
static int sheet_eval_cell(sheet_state *s, int col, int row, int64_t *out,
                           char *err_out, size_t err_cap) {
    if (!sheet_in_range(col, row)) {
        if (err_cap > 0) {
            err_out[0] = '\0';
        }
        return 0;
    }
    sheet_cell *cell = &s->cells[sheet_idx(col, row)];
    if (cell->state == 2) {
        if (cell->kind == 1 && out != NULL) {
            *out = cell->value;
        }
        if (cell->kind == 3 && err_out != NULL && err_cap > 0) {
            size_t i = 0;
            while (cell->err[i] != '\0' && i + 1 < err_cap) {
                err_out[i] = cell->err[i];
                i++;
            }
            err_out[i] = '\0';
        }
        return cell->kind;
    }
    if (cell->state == 1) {
        /* 評価中に戻ってきた = 循環参照。 */
        cell->kind = 3;
        cell->state = 2;
        const char *e = "#CIRC!";
        size_t i = 0;
        while (e[i] != '\0' && i + 1 < sizeof(cell->err)) {
            cell->err[i] = e[i];
            i++;
        }
        cell->err[i] = '\0';
        if (err_out != NULL && err_cap > 0) {
            size_t j = 0;
            while (cell->err[j] != '\0' && j + 1 < err_cap) {
                err_out[j] = cell->err[j];
                j++;
            }
            err_out[j] = '\0';
        }
        return 3;
    }
    cell->state = 1;
    cell->kind = 0;
    cell->value = 0;
    cell->err[0] = '\0';
    if (cell->raw[0] == '\0') {
        cell->state = 2;
        return 0;
    }
    if (cell->raw[0] == '=') {
        parser ps;
        ps.p = cell->raw + 1;
        ps.s = s;
        ps.err = P_OK;
        int64_t v = p_expr(&ps);
        p_skip(&ps);
        if (ps.err == P_OK && *ps.p != '\0') {
            ps.err = P_ERR;
        }
        if (ps.err != P_OK) {
            const char *e = "#ERR";
            if (ps.err == P_DIV0) {
                e = "#DIV/0!";
            } else if (ps.err == P_CIRC) {
                e = "#CIRC!";
            } else if (ps.err == P_NUM) {
                e = "#NUM!";
            }
            cell->kind = 3;
            size_t i = 0;
            while (e[i] != '\0' && i + 1 < sizeof(cell->err)) {
                cell->err[i] = e[i];
                i++;
            }
            cell->err[i] = '\0';
            if (err_out != NULL && err_cap > 0) {
                size_t j = 0;
                while (cell->err[j] != '\0' && j + 1 < err_cap) {
                    err_out[j] = cell->err[j];
                    j++;
                }
                err_out[j] = '\0';
            }
        } else {
            cell->kind = 1;
            cell->value = v;
            if (out != NULL) {
                *out = v;
            }
        }
        cell->state = 2;
        return cell->kind;
    }
    /* 数式でなければ、まず数値として読む。 */
    {
        const char *p = cell->raw;
        int is_num = 1;
        int seen_digit = 0;
        int seen_dot = 0;
        if (*p == '-') {
            p++;
        }
        if (*p == '\0') {
            is_num = 0;
        }
        for (; *p != '\0'; p++) {
            if (*p >= '0' && *p <= '9') {
                seen_digit = 1;
            } else if (*p == '.' && !seen_dot) {
                seen_dot = 1;
            } else {
                is_num = 0;
                break;
            }
        }
        if (is_num && seen_digit) {
            parser ps;
            ps.p = cell->raw;
            ps.s = s;
            ps.err = P_OK;
            int neg = 0;
            if (*ps.p == '-') {
                neg = 1;
                ps.p++;
            }
            int64_t v = 0;
            if (p_number(&ps, &v) && ps.err == P_OK && *ps.p == '\0') {
                cell->kind = 1;
                cell->value = neg ? -v : v;
                if (out != NULL) {
                    *out = cell->value;
                }
                cell->state = 2;
                return 1;
            }
            /* 読めない数値 (桁あふれ) はエラーにする。 */
            cell->kind = 3;
            const char *e = "#NUM!";
            size_t i = 0;
            while (e[i] != '\0' && i + 1 < sizeof(cell->err)) {
                cell->err[i] = e[i];
                i++;
            }
            cell->err[i] = '\0';
            if (err_out != NULL && err_cap > 0) {
                size_t j = 0;
                while (cell->err[j] != '\0' && j + 1 < err_cap) {
                    err_out[j] = cell->err[j];
                    j++;
                }
                err_out[j] = '\0';
            }
            cell->state = 2;
            return 3;
        }
    }
    cell->kind = 2;
    cell->state = 2;
    return 2;
}

void sheet_recompute(sheet_state *s) {
    for (int i = 0; i < SHEET_ROWS * SHEET_COLS; i++) {
        s->cells[i].state = 0;
        s->cells[i].kind = 0;
        s->cells[i].value = 0;
        s->cells[i].err[0] = '\0';
    }
    for (int row = 0; row < SHEET_ROWS; row++) {
        for (int col = 0; col < SHEET_COLS; col++) {
            (void)sheet_eval_cell(s, col, row, NULL, NULL, 0);
        }
    }
}

void sheet_set(sheet_state *s, int col, int row, const char *raw) {
    if (!sheet_in_range(col, row) || raw == NULL) {
        return;
    }
    sheet_cell *cell = &s->cells[sheet_idx(col, row)];
    size_t i = 0;
    while (raw[i] != '\0' && i < SHEET_CELL_MAX) {
        cell->raw[i] = raw[i];
        i++;
    }
    cell->raw[i] = '\0';
    sheet_recompute(s);
}

const char *sheet_raw(const sheet_state *s, int col, int row) {
    if (!sheet_in_range(col, row)) {
        return NULL;
    }
    return s->cells[sheet_idx(col, row)].raw;
}

int sheet_display(const sheet_state *s, int col, int row, char *buf,
                  size_t cap) {
    if (cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (!sheet_in_range(col, row)) {
        return 0;
    }
    const sheet_cell *cell = &s->cells[sheet_idx(col, row)];
    if (cell->kind == 1) {
        return sheet_format_milli(cell->value, buf, cap);
    }
    if (cell->kind == 3) {
        size_t i = 0;
        while (cell->err[i] != '\0' && i + 1 < cap) {
            buf[i] = cell->err[i];
            i++;
        }
        buf[i] = '\0';
        return (int)i;
    }
    if (cell->kind == 2) {
        size_t i = 0;
        while (cell->raw[i] != '\0' && i + 1 < cap) {
            buf[i] = cell->raw[i];
            i++;
        }
        buf[i] = '\0';
        return (int)i;
    }
    return 0;
}

/* ---- 保存・読み込み ---- */

int sheet_serialize(const sheet_state *s, char *buf, size_t cap) {
    size_t pos = 0;
    for (int row = 0; row < SHEET_ROWS; row++) {
        for (int col = 0; col < SHEET_COLS; col++) {
            const sheet_cell *cell = &s->cells[sheet_idx(col, row)];
            if (cell->raw[0] == '\0') {
                continue;
            }
            /* "A1=内容\n" */
            if (pos + 4 >= cap) {
                return -1;
            }
            buf[pos++] = (char)('A' + col);
            if (row + 1 >= 10) {
                if (pos + 1 >= cap) {
                    return -1;
                }
                buf[pos++] = (char)('0' + ((row + 1) / 10));
            }
            if (pos + 1 >= cap) {
                return -1;
            }
            buf[pos++] = (char)('0' + ((row + 1) % 10));
            if (pos + 1 >= cap) {
                return -1;
            }
            buf[pos++] = '=';
            size_t i = 0;
            while (cell->raw[i] != '\0') {
                if (pos + 1 >= cap) {
                    return -1;
                }
                buf[pos++] = cell->raw[i++];
            }
            if (pos + 1 >= cap) {
                return -1;
            }
            buf[pos++] = '\n';
        }
    }
    if (pos + 1 > cap) {
        return -1;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int sheet_parse(sheet_state *s, const char *data, size_t len) {
    sheet_clear(s);
    int count = 0;
    size_t pos = 0;
    while (pos < len) {
        /* 1 行読む。 */
        size_t start = pos;
        while (pos < len && data[pos] != '\n') {
            pos++;
        }
        size_t line_len = pos - start;
        if (pos < len) {
            pos++; /* '\n' を飛ばす */
        }
        if (line_len < 4) {
            continue;
        }
        const char *line = data + start;
        char c = line[0];
        if (c >= 'a' && c <= 'j') {
            c = (char)(c - 'a' + 'A');
        }
        if (c < 'A' || c > 'J') {
            continue;
        }
        int col = c - 'A';
        size_t p = 1;
        int row = 0;
        while (p < line_len && line[p] >= '0' && line[p] <= '9') {
            row = row * 10 + (line[p] - '0');
            p++;
        }
        if (row < 1 || row > SHEET_ROWS) {
            continue;
        }
        if (p >= line_len || line[p] != '=') {
            continue;
        }
        p++;
        char raw[SHEET_CELL_MAX + 1];
        size_t i = 0;
        while (p < line_len && i < SHEET_CELL_MAX) {
            raw[i++] = line[p++];
        }
        raw[i] = '\0';
        sheet_cell *cell = &s->cells[sheet_idx(col, row - 1)];
        size_t j = 0;
        while (raw[j] != '\0') {
            cell->raw[j] = raw[j];
            j++;
        }
        cell->raw[j] = '\0';
        count++;
    }
    sheet_recompute(s);
    return count;
}
