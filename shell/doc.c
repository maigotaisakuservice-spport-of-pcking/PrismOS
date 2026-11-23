/* PrismOS — 文書の純粋ロジック。 */
#include "doc.h"

#include <string.h>

void doc_clear(doc_state *d) {
    d->buf[0] = '\0';
    d->len = 0;
    d->cursor = 0;
}

void doc_load(doc_state *d, const char *text, size_t len) {
    doc_clear(d);
    if (text == NULL) {
        return;
    }
    for (size_t i = 0; i < len && d->len < DOC_MAX; i++) {
        if (text[i] == '\r') {
            continue;
        }
        d->buf[d->len++] = text[i];
    }
    d->buf[d->len] = '\0';
    d->cursor = d->len;
}

doc_line_type doc_classify_line(const char *line) {
    if (line == NULL) {
        return DOC_LINE_PLAIN;
    }
    if (line[0] == '#' && line[1] == ' ' && line[2] != '\0') {
        return DOC_LINE_H1;
    }
    if (line[0] == '#' && line[1] == '#' && line[2] == ' ' &&
        line[3] != '\0') {
        return DOC_LINE_H2;
    }
    if (line[0] == '#' && line[1] == '#' && line[2] == '#' &&
        line[3] == ' ' && line[4] != '\0') {
        return DOC_LINE_H3;
    }
    if (line[0] == '-' && line[1] == ' ' && line[2] != '\0') {
        return DOC_LINE_BULLET;
    }
    /* 番号付き: 数字 1〜3 桁 + ". " + 本文。 */
    if (line[0] >= '0' && line[0] <= '9') {
        size_t i = 0;
        while (line[i] >= '0' && line[i] <= '9') {
            i++;
        }
        if (i >= 1 && i <= 3 && line[i] == '.' && line[i + 1] == ' ' &&
            line[i + 2] != '\0') {
            return DOC_LINE_NUMBERED;
        }
    }
    return DOC_LINE_PLAIN;
}

size_t doc_line_marker_len(const char *line) {
    doc_line_type t = doc_classify_line(line);
    if (t == DOC_LINE_H1 || t == DOC_LINE_BULLET) {
        return 2;
    }
    if (t == DOC_LINE_H2) {
        return 3;
    }
    if (t == DOC_LINE_H3) {
        return 4;
    }
    if (t == DOC_LINE_NUMBERED) {
        size_t i = 0;
        while (line[i] >= '0' && line[i] <= '9') {
            i++;
        }
        return i + 2; /* 数字 + ". " */
    }
    return 0;
}

int doc_next_segment(const char *line, size_t len, size_t pos,
                     size_t *seg_start, size_t *seg_len, int *bold) {
    if (line == NULL || pos >= len) {
        return 0;
    }
    if (pos + 1 < len && line[pos] == '*' && line[pos + 1] == '*') {
        /* 太字の区切り: 閉じの ** まで。 */
        size_t start = pos + 2;
        size_t i = start;
        while (i + 1 < len &&
               !(line[i] == '*' && line[i + 1] == '*')) {
            i++;
        }
        if (i + 1 >= len) {
            /* 閉じが無い: 残り全部をふつうの文字として扱う。 */
            *seg_start = pos;
            *seg_len = len - pos;
            *bold = 0;
            return 1;
        }
        *seg_start = start;
        *seg_len = i - start;
        *bold = 1;
        return 1;
    }
    /* ふつうの区切り: 次の ** の直前まで。 */
    size_t i = pos;
    while (i + 1 < len && !(line[i] == '*' && line[i + 1] == '*')) {
        i++;
    }
    if (i + 1 < len) {
        *seg_start = pos;
        *seg_len = i - pos;
    } else {
        *seg_start = pos;
        *seg_len = len - pos;
    }
    *bold = 0;
    return 1;
}

void doc_insert_char(doc_state *d, char c) {
    if (d->len + 1 >= DOC_MAX) {
        return;
    }
    for (size_t i = d->len; i > d->cursor; i--) {
        d->buf[i] = d->buf[i - 1];
    }
    d->buf[d->cursor] = c;
    d->len++;
    d->cursor++;
    d->buf[d->len] = '\0';
}

void doc_backspace(doc_state *d) {
    if (d->cursor == 0) {
        return;
    }
    for (size_t i = d->cursor - 1; i < d->len; i++) {
        d->buf[i] = d->buf[i + 1];
    }
    d->len--;
    d->cursor--;
    d->buf[d->len] = '\0';
}

void doc_insert_newline(doc_state *d) {
    doc_insert_char(d, '\n');
}

void doc_cursor_left(doc_state *d) {
    if (d->cursor > 0) {
        d->cursor--;
    }
}

void doc_cursor_right(doc_state *d) {
    if (d->cursor < d->len) {
        d->cursor++;
    }
}

size_t doc_cursor_row(const doc_state *d) {
    size_t row = 0;
    for (size_t i = 0; i < d->cursor; i++) {
        if (d->buf[i] == '\n') {
            row++;
        }
    }
    return row;
}

size_t doc_cursor_col(const doc_state *d) {
    size_t col = 0;
    size_t i = d->cursor;
    while (i > 0 && d->buf[i - 1] != '\n') {
        i--;
        col++;
    }
    return col;
}

size_t doc_line_count(const doc_state *d) {
    if (d->len == 0) {
        return 1;
    }
    size_t n = 1;
    for (size_t i = 0; i < d->len; i++) {
        if (d->buf[i] == '\n') {
            n++;
        }
    }
    if (d->buf[d->len - 1] == '\n') {
        n--; /* 末尾の改行の後ろに空行は数えない */
    }
    return n;
}

int doc_line_bounds(const doc_state *d, size_t row, size_t *start,
                    size_t *end) {
    size_t r = 0;
    size_t i = 0;
    while (r < row) {
        while (i < d->len && d->buf[i] != '\n') {
            i++;
        }
        if (i >= d->len) {
            return 0;
        }
        i++; /* 改行を越える */
        r++;
    }
    *start = i;
    while (i < d->len && d->buf[i] != '\n') {
        i++;
    }
    *end = i;
    return 1;
}

doc_line_type doc_cursor_line_type(const doc_state *d) {
    size_t start = 0;
    size_t end = 0;
    if (!doc_line_bounds(d, doc_cursor_row(d), &start, &end)) {
        return DOC_LINE_PLAIN;
    }
    char line[DOC_LINE_MAX + 1];
    size_t n = end - start;
    if (n > DOC_LINE_MAX) {
        n = DOC_LINE_MAX;
    }
    for (size_t i = 0; i < n; i++) {
        line[i] = d->buf[start + i];
    }
    line[n] = '\0';
    return doc_classify_line(line);
}

void doc_set_line_type(doc_state *d, doc_line_type kind) {
    size_t row = doc_cursor_row(d);
    size_t start = 0;
    size_t end = 0;
    if (!doc_line_bounds(d, row, &start, &end)) {
        return;
    }
    char line[DOC_LINE_MAX + 1];
    size_t n = end - start;
    if (n > DOC_LINE_MAX) {
        n = DOC_LINE_MAX;
    }
    for (size_t i = 0; i < n; i++) {
        line[i] = d->buf[start + i];
    }
    line[n] = '\0';
    size_t old_marker = doc_line_marker_len(line);
    const char *body = line + old_marker;
    size_t body_len = n - old_marker;
    const char *marker = "";
    size_t marker_len = 0;
    char numbuf[8];
    if (kind == DOC_LINE_H1) {
        marker = "# ";
        marker_len = 2;
    } else if (kind == DOC_LINE_H2) {
        marker = "## ";
        marker_len = 3;
    } else if (kind == DOC_LINE_H3) {
        marker = "### ";
        marker_len = 4;
    } else if (kind == DOC_LINE_BULLET) {
        marker = "- ";
        marker_len = 2;
    } else if (kind == DOC_LINE_NUMBERED) {
        /* 番号は行番号から付ける (1 から)。3 桁まで。 */
        size_t num = row + 1;
        size_t p = 0;
        if (num >= 100) {
            numbuf[p++] = (char)('0' + num / 100);
            numbuf[p++] = (char)('0' + (num / 10) % 10);
        } else if (num >= 10) {
            numbuf[p++] = (char)('0' + num / 10);
        }
        numbuf[p++] = (char)('0' + num % 10);
        numbuf[p++] = '.';
        numbuf[p++] = ' ';
        numbuf[p] = '\0';
        marker = numbuf;
        marker_len = p;
    }
    size_t new_len = d->len - old_marker + marker_len;
    if (new_len > DOC_MAX - 1) {
        return;
    }
    /* 行頭の目印を置き換える: まず後ろをずらす。 */
    size_t tail_start = start + old_marker;
    size_t tail_len = d->len - tail_start;
    size_t new_tail = start + marker_len;
    if (marker_len >= old_marker) {
        for (size_t i = tail_len; i-- > 0;) {
            d->buf[new_tail + i] = d->buf[tail_start + i];
        }
    } else {
        for (size_t i = 0; i < tail_len; i++) {
            d->buf[new_tail + i] = d->buf[tail_start + i];
        }
    }
    for (size_t i = 0; i < marker_len; i++) {
        d->buf[start + i] = marker[i];
    }
    d->len = new_len;
    d->buf[d->len] = '\0';
    /* カーソルが行頭の目印の中にいたら、本文の先頭に置く。 */
    size_t rel = d->cursor - start;
    if (rel < marker_len) {
        d->cursor = start + marker_len;
    } else {
        d->cursor += marker_len - old_marker;
    }
    (void)body;
    (void)body_len;
}
