/* PrismOS — プレゼンテーションの純粋ロジック。 */
#include "slides.h"

#include <string.h>

static void copy_trunc(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void slides_clear(slides_state *s) {
    memset(s, 0, sizeof(*s));
    s->count = 1;
    s->slides[0].title[0] = '\0';
    s->slides[0].line_count = 0;
}

int slides_add(slides_state *s) {
    if (s->count >= SLIDES_MAX) {
        return -1;
    }
    int idx = s->count++;
    s->slides[idx].title[0] = '\0';
    s->slides[idx].line_count = 0;
    return idx;
}

void slides_delete(slides_state *s, int index) {
    if (s->count <= 1 || index < 0 || index >= s->count) {
        return;
    }
    for (int i = index; i + 1 < s->count; i++) {
        s->slides[i] = s->slides[i + 1];
    }
    s->count--;
}

void slides_set_title(slides_state *s, int index, const char *title) {
    if (index < 0 || index >= s->count || title == NULL) {
        return;
    }
    copy_trunc(s->slides[index].title, sizeof(s->slides[index].title),
               title);
}

void slides_set_line(slides_state *s, int index, int line,
                     const char *text) {
    if (index < 0 || index >= s->count || text == NULL) {
        return;
    }
    if (line < 0 || line >= s->slides[index].line_count) {
        return;
    }
    copy_trunc(s->slides[index].lines[line],
               sizeof(s->slides[index].lines[line]), text);
}

int slides_add_line(slides_state *s, int index, const char *text) {
    if (index < 0 || index >= s->count || text == NULL) {
        return -1;
    }
    slide *sl = &s->slides[index];
    if (sl->line_count >= SLIDES_LINES_MAX) {
        return -1;
    }
    int line = sl->line_count++;
    copy_trunc(sl->lines[line], sizeof(sl->lines[line]), text);
    return line;
}

void slides_remove_line(slides_state *s, int index, int line) {
    if (index < 0 || index >= s->count) {
        return;
    }
    slide *sl = &s->slides[index];
    if (line < 0 || line >= sl->line_count) {
        return;
    }
    for (int i = line; i + 1 < sl->line_count; i++) {
        strcpy(sl->lines[i], sl->lines[i + 1]);
    }
    sl->line_count--;
    sl->lines[sl->line_count][0] = '\0';
}

int slides_serialize(const slides_state *s, char *buf, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < s->count; i++) {
        if (i > 0) {
            if (n + 4 >= cap) {
                return -1;
            }
            memcpy(buf + n, "===\n", 4);
            n += 4;
        }
        size_t tl = strlen(s->slides[i].title);
        if (n + tl + 1 >= cap) {
            return -1;
        }
        memcpy(buf + n, s->slides[i].title, tl);
        n += tl;
        buf[n++] = '\n';
        for (int j = 0; j < s->slides[i].line_count; j++) {
            size_t ll = strlen(s->slides[i].lines[j]);
            if (n + 2 + ll + 1 >= cap) {
                return -1;
            }
            memcpy(buf + n, "- ", 2);
            n += 2;
            memcpy(buf + n, s->slides[i].lines[j], ll);
            n += ll;
            buf[n++] = '\n';
        }
    }
    if (n >= cap) {
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

int slides_parse(slides_state *s, const char *data, size_t len) {
    slides_clear(s);
    s->count = 0;
    size_t pos = 0;
    int cur = -1; /* 今書いているスライドの番号 */
    while (pos < len) {
        size_t line_start = pos;
        while (pos < len && data[pos] != '\n') {
            pos++;
        }
        size_t line_len = pos - line_start;
        if (pos < len) {
            pos++; /* 改行を越える */
        }
        if (line_len > 0 && data[line_start + line_len - 1] == '\r') {
            line_len--;
        }
        char line[SLIDES_LINE_MAX + 1];
        size_t ll = line_len > SLIDES_LINE_MAX ? SLIDES_LINE_MAX
                                               : line_len;
        memcpy(line, data + line_start, ll);
        line[ll] = '\0';
        if (strcmp(line, "===") == 0) {
            if (s->count >= SLIDES_MAX) {
                break;
            }
            cur = s->count++;
            s->slides[cur].title[0] = '\0';
            s->slides[cur].line_count = 0;
            continue;
        }
        if (cur < 0) {
            /* 最初のスライドを作る。 */
            if (s->count >= SLIDES_MAX) {
                break;
            }
            cur = s->count++;
            s->slides[cur].title[0] = '\0';
            s->slides[cur].line_count = 0;
        }
        if (line[0] == '-' && line[1] == ' ') {
            slides_add_line(s, cur, line + 2);
        } else if (s->slides[cur].title[0] == '\0') {
            /* タイトルは最初のふつうの行。 */
            slides_set_title(s, cur, line);
        } else {
            /* タイトルの後のふつうの行は箇条書きにする。 */
            slides_add_line(s, cur, line);
        }
    }
    if (s->count == 0) {
        slides_clear(s);
    }
    return s->count;
}
