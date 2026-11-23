/* PrismOS — .docx (OOXML) の読み書き。 */
#include "docx.h"

#include <string.h>

#include "doc.h"
#include "xml.h"
#include "zip.h"

static const char CONTENT_TYPES[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/"
    "content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd."
    "openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\" ContentType=\""
    "application/vnd.openxmlformats-officedocument.wordprocessingml."
    "document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/"
    "vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "</Types>";

/* 見出しの定義。無いと読み手が見出しをふつうの段落に落とす。 */
static const char STYLES[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/"
    "wordprocessingml/2006/main\">"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
    "<w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
    "<w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:next w:val=\"Normal\"/><w:qFormat/>"
    "<w:rPr><w:b/><w:sz w:val=\"32\"/><w:szCs w:val=\"32\"/></w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\">"
    "<w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:next w:val=\"Normal\"/><w:qFormat/>"
    "<w:rPr><w:b/><w:sz w:val=\"28\"/><w:szCs w:val=\"28\"/></w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\">"
    "<w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:next w:val=\"Normal\"/><w:qFormat/>"
    "<w:rPr><w:b/><w:sz w:val=\"26\"/><w:szCs w:val=\"26\"/></w:rPr>"
    "</w:style></w:styles>";

static const char DOC_RELS[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
    "2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
    "officeDocument/2006/relationships/styles\" "
    "Target=\"styles.xml\"/></Relationships>";

static const char RELS[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
    "2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
    "officeDocument/2006/relationships/officeDocument\" "
    "Target=\"word/document.xml\"/></Relationships>";

static const char DOC_HEAD[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
    "wordprocessingml/2006/main\"><w:body>";
static const char DOC_TAIL[] = "</w:body></w:document>";

/* 文字列を xml の末尾に足す。長さは strlen で数える
 * (数え間違いを防ぐため、長さを手で書かない)。足りなけれ
 * ば -1。 */
static int xml_put(char *xml, size_t *n, const char *s) {
    size_t len = strlen(s);
    if (*n + len >= 16384) {
        return -1;
    }
    memcpy(xml + *n, s, len);
    *n += len;
    return 0;
}

static int xml_putn(char *xml, size_t *n, const char *s, size_t len) {
    if (*n + len >= 16384) {
        return -1;
    }
    memcpy(xml + *n, s, len);
    *n += len;
    return 0;
}

int docx_export(const char *text, size_t len, uint8_t *out, size_t cap) {
    if (text == NULL || out == NULL) {
        return -1;
    }
    /* document.xml を作る。行ごとに段落を出す。 */
    static char xml[16384];
    size_t n = 0;
    if (xml_put(xml, &n, DOC_HEAD) != 0) {
        return -1;
    }
    size_t pos = 0;
    int first_line = 1;
    while (first_line || pos < len) {
        first_line = 0;
        size_t line_start = pos;
        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        size_t line_len = pos - line_start;
        if (pos < len) {
            pos++; /* 改行を越える */
        }
        char line[DOC_LINE_MAX + 1];
        size_t ll = line_len > DOC_LINE_MAX ? DOC_LINE_MAX : line_len;
        memcpy(line, text + line_start, ll);
        line[ll] = '\0';
        doc_line_type kind = doc_classify_line(line);
        if (ll == 0) {
            /* 空の段落。 */
            if (xml_put(xml, &n, "<w:p/>") != 0) {
                return -1;
            }
            continue;
        }
        /* <w:p> + 書式 + 本文 + </w:p>。 */
        if (xml_put(xml, &n, "<w:p>") != 0) {
            return -1;
        }
        if (kind == DOC_LINE_H1 || kind == DOC_LINE_H2 ||
            kind == DOC_LINE_H3) {
            const char *style = kind == DOC_LINE_H1 ? "Heading1"
                                : kind == DOC_LINE_H2 ? "Heading2"
                                                      : "Heading3";
            if (xml_put(xml, &n, "<w:pPr><w:pStyle w:val=\"") != 0 ||
                xml_put(xml, &n, style) != 0 ||
                xml_put(xml, &n, "\"/></w:pPr>") != 0) {
                return -1;
            }
        }
        /* 箇条書きは本文の先頭に • を付ける。番号付きは
         * "1. " を本文に残す (番号が消えないようにする)。 */
        size_t marker = doc_line_marker_len(line);
        size_t body_start = (kind == DOC_LINE_NUMBERED) ? 0 : marker;
        if (kind == DOC_LINE_BULLET) {
            if (xml_put(xml, &n,
                        "<w:r><w:t xml:space=\"preserve\">") != 0 ||
                xml_putn(xml, &n, "\xE2\x80\xA2 ", 4) != 0 ||
                xml_put(xml, &n, "</w:t></w:r>") != 0) {
                return -1;
            }
        }
        /* 本文を太字の区切りごとに run に分ける。 */
        size_t seg_pos = body_start;
        while (seg_pos < ll) {
            size_t s = 0, sl = 0;
            int bold = 0;
            if (!doc_next_segment(line, ll, seg_pos, &s, &sl, &bold)) {
                break;
            }
            char esc[DOC_LINE_MAX * 6 + 1];
            int el = xml_escape_text(line + s, sl, esc, sizeof(esc));
            if (el < 0) {
                return -1;
            }
            const char *run_open =
                bold ? "<w:r><w:rPr><w:b/></w:rPr>" : "<w:r>";
            if (xml_put(xml, &n, run_open) != 0 ||
                xml_put(xml, &n, "<w:t xml:space=\"preserve\">") != 0 ||
                xml_putn(xml, &n, esc, (size_t)el) != 0 ||
                xml_put(xml, &n, "</w:t></w:r>") != 0) {
                return -1;
            }
            /* 太字の区切りは閉じの ** も消費する。 */
            seg_pos = s + sl + (bold ? 2 : 0);
        }
        if (xml_put(xml, &n, "</w:p>") != 0) {
            return -1;
        }
    }
    if (xml_put(xml, &n, DOC_TAIL) != 0) {
        return -1;
    }
    xml[n] = '\0';

    /* 静的に置く (64 件分の表が入るためスタックに置けない)。 */
    static zip_writer w;
    zip_write_begin(&w, out, cap);
    if (zip_write_add(&w, "[Content_Types].xml",
                      (const uint8_t *)CONTENT_TYPES,
                      strlen(CONTENT_TYPES)) != 0) {
        return -1;
    }
    if (zip_write_add(&w, "_rels/.rels", (const uint8_t *)RELS,
                      strlen(RELS)) != 0) {
        return -1;
    }
    if (zip_write_add(&w, "word/document.xml", (const uint8_t *)xml, n) !=
        0) {
        return -1;
    }
    if (zip_write_add(&w, "word/styles.xml", (const uint8_t *)STYLES,
                      strlen(STYLES)) != 0) {
        return -1;
    }
    if (zip_write_add(&w, "word/_rels/document.xml.rels",
                      (const uint8_t *)DOC_RELS, strlen(DOC_RELS)) != 0) {
        return -1;
    }
    return zip_write_finish(&w);
}

/* --- 番号づけ (word/numbering.xml + word/styles.xml) ---
 *
 * 他アプリ製 .docx の番号付き段落は、本文に番号が無く、段落
 * の書式 (w:numPr) と word/numbering.xml の定義で決まる。
 * numId → abstractNumId → 段落 (ilvl) の書式 (numFmt /
 * lvlText / start) をたどり、"1. " の形の接頭辞を付けて読む。
 * 段落様式だけの段落 (ListNumber など) は、番号づけ定義の
 * 中の pStyle か、word/styles.xml の様式定義から numId を
 * 引く。styles.xml が 16384 バイトに収まらなければ読まない
 * (大きな styles.xml は pStyle の結びつけだけで済ませる)。
 * numbering.xml が無ければ、番号づけは分からないので箇条
 * 書き "- " に落とす (以前の挙動)。
 */

#define NUM_MAX_ABSTRACT 16
#define NUM_MAX_LEVELS 9
#define NUM_MAX_NUMS 32
#define NUM_MAX_STYLES 32
#define NUM_MAX_COUNTERS 64
#define NUM_STYLE_KEY 0x10000

typedef enum {
    NUM_FMT_DECIMAL = 0,
    NUM_FMT_LOWER_LETTER,
    NUM_FMT_UPPER_LETTER,
    NUM_FMT_LOWER_ROMAN,
    NUM_FMT_UPPER_ROMAN,
    NUM_FMT_BULLET,
    NUM_FMT_NONE
} num_fmt;

typedef struct {
    int defined;   /* この段落が定義されているか */
    int start;     /* 開始番号 (ふつう 1) */
    num_fmt fmt;   /* 書式 */
    char text[32]; /* lvlText (例 "%1.")、空なら既定の形 */
    char style[32]; /* 結びついた段落様式 (空なら無し) */
} num_level;

typedef struct {
    int id; /* abstractNumId (-1 は空き) */
    num_level levels[NUM_MAX_LEVELS];
} num_abstract;

typedef struct {
    int num_id;      /* numId (-1 は空き) */
    int abstract_id; /* 結びついた abstractNumId (-1 は無し) */
} num_def;

typedef struct {
    char style_id[32];
    char based_on[32];
    int num_id; /* -1 は番号づけ無し */
    int ilvl;
} num_style;

typedef struct {
    int key;     /* 直接は numId、様式からは NUM_STYLE_KEY + abstractNumId */
    int level;   /* ilvl */
    int value;   /* 今の番号 */
    int started; /* 数え始めているか */
} num_counter;

static num_abstract num_abstracts[NUM_MAX_ABSTRACT];
static num_def num_defs[NUM_MAX_NUMS];
static num_style num_styles[NUM_MAX_STYLES];
static num_counter num_counters[NUM_MAX_COUNTERS];

/* 10 進の整数を読む。数字以外があれば -1。 */
static int num_atoi(const char *s) {
    int v = 0;
    if (s == NULL || *s == '\0') {
        return -1;
    }
    for (int i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
        if (v > 1000000) {
            return -1;
        }
    }
    return v;
}

/* tag の中から attr="..." の値を out に写す。無ければ -1。 */
static int num_get_attr(const char *tag, const char *attr, char *out,
                        size_t cap) {
    size_t alen = strlen(attr);
    const char *p = tag;
    while ((p = strstr(p, attr)) != NULL) {
        if (p[alen] == '=' && p[alen + 1] == '"') {
            const char *v = p + alen + 2;
            size_t i = 0;
            while (v[i] != '"' && v[i] != '\0' && i + 1 < cap) {
                out[i] = v[i];
                i++;
            }
            if (v[i] != '"') {
                return -1; /* 途中で切れた */
            }
            out[i] = '\0';
            return 0;
        }
        p += alen;
    }
    return -1;
}

static num_fmt num_fmt_from_name(const char *name) {
    if (strcmp(name, "bullet") == 0) {
        return NUM_FMT_BULLET;
    }
    if (strcmp(name, "lowerLetter") == 0) {
        return NUM_FMT_LOWER_LETTER;
    }
    if (strcmp(name, "upperLetter") == 0) {
        return NUM_FMT_UPPER_LETTER;
    }
    if (strcmp(name, "lowerRoman") == 0) {
        return NUM_FMT_LOWER_ROMAN;
    }
    if (strcmp(name, "upperRoman") == 0) {
        return NUM_FMT_UPPER_ROMAN;
    }
    if (strcmp(name, "none") == 0) {
        return NUM_FMT_NONE;
    }
    return NUM_FMT_DECIMAL; /* decimal、decimalZero、その他は 10 進 */
}

/* 番号 n を書式に合わせて out に書く。 */
static void num_format_number(num_fmt fmt, int n, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (fmt == NUM_FMT_DECIMAL || n < 1) {
        /* 10 進 (1 未満はそのまま 10 進で出す)。 */
        char tmp[16];
        int i = 0;
        int neg = n < 0;
        unsigned int v = neg ? (unsigned int)(-n) : (unsigned int)n;
        if (v == 0) {
            tmp[i++] = '0';
        }
        while (v > 0 && i < (int)sizeof(tmp)) {
            tmp[i++] = (char)('0' + v % 10);
            v /= 10;
        }
        size_t o = 0;
        if (neg && o + 1 < cap) {
            out[o++] = '-';
        }
        while (i > 0 && o + 1 < cap) {
            out[o++] = tmp[--i];
        }
        out[o] = '\0';
        return;
    }
    if (fmt == NUM_FMT_LOWER_LETTER || fmt == NUM_FMT_UPPER_LETTER) {
        /* a..z、aa.. (1 始まりの 26 進)。 */
        char tmp[16];
        int i = 0;
        int v = n;
        while (v > 0 && i < (int)sizeof(tmp)) {
            v--;
            tmp[i++] = (char)((fmt == NUM_FMT_LOWER_LETTER ? 'a' : 'A') +
                             v % 26);
            v /= 26;
        }
        size_t o = 0;
        while (i > 0 && o + 1 < cap) {
            out[o++] = tmp[--i];
        }
        out[o] = '\0';
        return;
    }
    /* ローマ数字 (1..3999、超えた分は 10 進)。 */
    static const int vals[] = {1000, 900, 500, 400, 100, 90,
                               50,   40,  10,  9,   5,   4,  1};
    static const char *syms[] = {"M", "CM", "D", "CD", "C", "XC", "L",
                                 "XL", "X", "IX", "V", "IV", "I"};
    if (n > 3999) {
        num_format_number(NUM_FMT_DECIMAL, n, out, cap);
        return;
    }
    size_t o = 0;
    int v = n;
    for (int k = 0; k < 13; k++) {
        while (v >= vals[k]) {
            for (int j = 0; syms[k][j] != '\0' && o + 1 < cap; j++) {
                char c = syms[k][j];
                if (fmt == NUM_FMT_LOWER_ROMAN && c >= 'A' && c <= 'Z') {
                    c = (char)(c - 'A' + 'a');
                }
                out[o++] = c;
            }
            v -= vals[k];
        }
    }
    out[o] = '\0';
}

/* word/numbering.xml を読み、表に入れる。 */
static void num_parse_numbering(const char *s, size_t len) {
    int cur_abstract = -1; /* num_abstracts の添字 */
    int cur_lvl = -1;
    int cur_num = -1; /* num_defs の添字 */
    size_t pos = 0;
    while (pos < len) {
        if (s[pos] != '<') {
            pos++;
            continue;
        }
        size_t end = pos + 1;
        while (end < len && s[end] != '>') {
            end++;
        }
        if (end >= len) {
            return;
        }
        size_t tag_len = end - pos - 1;
        char tag[128];
        size_t tl = tag_len > sizeof(tag) - 1 ? sizeof(tag) - 1 : tag_len;
        memcpy(tag, s + pos + 1, tl);
        tag[tl] = '\0';
        int closing = tag[0] == '/';
        const char *tname = closing ? tag + 1 : tag;
        char val[32];
        if (!closing && strncmp(tname, "w:abstractNumId", 15) == 0 &&
            (tname[15] == ' ' || tname[15] == '/' || tname[15] == '\0')) {
            /* <w:num> の中の結びつけ。 */
            if (cur_num >= 0 &&
                num_get_attr(tag, "w:val", val, sizeof(val)) == 0) {
                int a = num_atoi(val);
                if (a >= 0) {
                    num_defs[cur_num].abstract_id = a;
                }
            }
        } else if (!closing && strncmp(tname, "w:abstractNum", 13) == 0 &&
                   (tname[13] == ' ' || tname[13] == '/' ||
                    tname[13] == '\0')) {
            cur_abstract = -1;
            cur_lvl = -1;
            if (num_get_attr(tag, "w:abstractNumId", val, sizeof(val)) ==
                0) {
                int id = num_atoi(val);
                if (id >= 0) {
                    for (int i = 0; i < NUM_MAX_ABSTRACT; i++) {
                        if (num_abstracts[i].id == id ||
                            num_abstracts[i].id < 0) {
                            if (num_abstracts[i].id < 0) {
                                num_abstracts[i].id = id;
                                for (int l = 0; l < NUM_MAX_LEVELS; l++) {
                                    num_abstracts[i].levels[l].defined = 0;
                                    num_abstracts[i].levels[l].start = 1;
                                    num_abstracts[i].levels[l].fmt =
                                        NUM_FMT_DECIMAL;
                                    num_abstracts[i].levels[l].text[0] =
                                        '\0';
                                    num_abstracts[i].levels[l].style[0] =
                                        '\0';
                                }
                            }
                            cur_abstract = i;
                            break;
                        }
                    }
                }
            }
        } else if (closing && strcmp(tname, "w:abstractNum") == 0) {
            cur_abstract = -1;
            cur_lvl = -1;
        } else if (!closing && strncmp(tname, "w:lvlText", 9) == 0 &&
                   (tname[9] == ' ' || tname[9] == '/' ||
                    tname[9] == '\0')) {
            if (cur_abstract >= 0 && cur_lvl >= 0 &&
                num_get_attr(tag, "w:val", val, sizeof(val)) == 0) {
                num_level *lv =
                    &num_abstracts[cur_abstract].levels[cur_lvl];
                size_t vl = strlen(val);
                if (vl > sizeof(lv->text) - 1) {
                    vl = sizeof(lv->text) - 1;
                }
                memcpy(lv->text, val, vl);
                lv->text[vl] = '\0';
            }
        } else if (!closing && strncmp(tname, "w:lvl", 5) == 0 &&
                   (tname[5] == ' ' || tname[5] == '/' ||
                    tname[5] == '\0')) {
            cur_lvl = -1;
            if (cur_abstract >= 0 &&
                num_get_attr(tag, "w:ilvl", val, sizeof(val)) == 0) {
                int ilvl = num_atoi(val);
                if (ilvl >= 0 && ilvl < NUM_MAX_LEVELS) {
                    cur_lvl = ilvl;
                    num_level *lv =
                        &num_abstracts[cur_abstract].levels[ilvl];
                    lv->defined = 1;
                    lv->start = 1;
                    lv->fmt = NUM_FMT_DECIMAL;
                    lv->text[0] = '\0';
                    lv->style[0] = '\0';
                }
            }
        } else if (closing && strcmp(tname, "w:lvl") == 0) {
            cur_lvl = -1;
        } else if (!closing && strncmp(tname, "w:numFmt", 8) == 0 &&
                   (tname[8] == ' ' || tname[8] == '/' ||
                    tname[8] == '\0')) {
            if (cur_abstract >= 0 && cur_lvl >= 0 &&
                num_get_attr(tag, "w:val", val, sizeof(val)) == 0) {
                num_abstracts[cur_abstract].levels[cur_lvl].fmt =
                    num_fmt_from_name(val);
            }
        } else if (!closing && strncmp(tname, "w:start", 7) == 0 &&
                   (tname[7] == ' ' || tname[7] == '/' ||
                    tname[7] == '\0')) {
            if (cur_abstract >= 0 && cur_lvl >= 0 &&
                num_get_attr(tag, "w:val", val, sizeof(val)) == 0) {
                int st = num_atoi(val);
                if (st >= 0) {
                    num_abstracts[cur_abstract].levels[cur_lvl].start = st;
                }
            }
        } else if (!closing && strncmp(tname, "w:pStyle", 8) == 0 &&
                   (tname[8] == ' ' || tname[8] == '/' ||
                    tname[8] == '\0')) {
            if (cur_abstract >= 0 && cur_lvl >= 0 &&
                num_get_attr(tag, "w:val", val, sizeof(val)) == 0) {
                num_level *lv =
                    &num_abstracts[cur_abstract].levels[cur_lvl];
                size_t vl = strlen(val);
                if (vl > sizeof(lv->style) - 1) {
                    vl = sizeof(lv->style) - 1;
                }
                memcpy(lv->style, val, vl);
                lv->style[vl] = '\0';
            }
        } else if (!closing && strncmp(tname, "w:num", 5) == 0 &&
                   (tname[5] == ' ' || tname[5] == '/' ||
                    tname[5] == '\0')) {
            cur_num = -1;
            if (num_get_attr(tag, "w:numId", val, sizeof(val)) == 0) {
                int id = num_atoi(val);
                if (id >= 0) {
                    for (int i = 0; i < NUM_MAX_NUMS; i++) {
                        if (num_defs[i].num_id == id ||
                            num_defs[i].num_id < 0) {
                            if (num_defs[i].num_id < 0) {
                                num_defs[i].num_id = id;
                                num_defs[i].abstract_id = -1;
                            }
                            cur_num = i;
                            break;
                        }
                    }
                }
            }
        } else if (closing && strcmp(tname, "w:num") == 0) {
            cur_num = -1;
        }
        pos = end + 1;
    }
}

/* word/styles.xml を読み、様式 → numId の表に入れる。 */
static void num_parse_styles(const char *s, size_t len) {
    int cur_style = -1; /* num_styles の添字 */
    int in_numpr = 0;
    size_t pos = 0;
    while (pos < len) {
        if (s[pos] != '<') {
            pos++;
            continue;
        }
        size_t end = pos + 1;
        while (end < len && s[end] != '>') {
            end++;
        }
        if (end >= len) {
            return;
        }
        size_t tag_len = end - pos - 1;
        char tag[128];
        size_t tl = tag_len > sizeof(tag) - 1 ? sizeof(tag) - 1 : tag_len;
        memcpy(tag, s + pos + 1, tl);
        tag[tl] = '\0';
        int closing = tag[0] == '/';
        const char *tname = closing ? tag + 1 : tag;
        /* self_close は元文字列で判定する。tag[] はクランプ後の
         * コピーなので、長いタグでは tag[tag_len-1] が範囲外。 */
        int self_close = tag_len > 0 && s[end - 1] == '/';
        char val[32];
        if (!closing && strncmp(tname, "w:style", 7) == 0 &&
            (tname[7] == ' ' || tname[7] == '/' || tname[7] == '\0')) {
            cur_style = -1;
            in_numpr = 0;
            if (num_get_attr(tag, "w:styleId", val, sizeof(val)) == 0) {
                for (int i = 0; i < NUM_MAX_STYLES; i++) {
                    if (num_styles[i].style_id[0] == '\0' ||
                        strcmp(num_styles[i].style_id, val) == 0) {
                        if (num_styles[i].style_id[0] == '\0') {
                            size_t vl = strlen(val);
                            if (vl > sizeof(num_styles[i].style_id) - 1) {
                                vl = sizeof(num_styles[i].style_id) - 1;
                            }
                            memcpy(num_styles[i].style_id, val, vl);
                            num_styles[i].style_id[vl] = '\0';
                            num_styles[i].based_on[0] = '\0';
                            num_styles[i].num_id = -1;
                            num_styles[i].ilvl = 0;
                        }
                        cur_style = i;
                        break;
                    }
                }
            }
        } else if (closing && strcmp(tname, "w:style") == 0) {
            cur_style = -1;
            in_numpr = 0;
        } else if (cur_style >= 0 && !closing &&
                   strncmp(tname, "w:basedOn", 9) == 0 &&
                   (tname[9] == ' ' || tname[9] == '/' ||
                    tname[9] == '\0')) {
            if (num_get_attr(tag, "w:val", val, sizeof(val)) == 0) {
                size_t vl = strlen(val);
                if (vl > sizeof(num_styles[cur_style].based_on) - 1) {
                    vl = sizeof(num_styles[cur_style].based_on) - 1;
                }
                memcpy(num_styles[cur_style].based_on, val, vl);
                num_styles[cur_style].based_on[vl] = '\0';
            }
        } else if (cur_style >= 0 && !closing &&
                   strncmp(tname, "w:numPr", 7) == 0 &&
                   (tname[7] == ' ' || tname[7] == '/' ||
                    tname[7] == '\0')) {
            in_numpr = !self_close;
        } else if (cur_style >= 0 && closing &&
                   strcmp(tname, "w:numPr") == 0) {
            in_numpr = 0;
        } else if (cur_style >= 0 && in_numpr && !closing &&
                   strncmp(tname, "w:numId", 7) == 0 &&
                   (tname[7] == ' ' || tname[7] == '/' ||
                    tname[7] == '\0')) {
            if (num_get_attr(tag, "w:val", val, sizeof(val)) == 0) {
                int id = num_atoi(val);
                if (id >= 0) {
                    num_styles[cur_style].num_id = id;
                }
            }
        } else if (cur_style >= 0 && in_numpr && !closing &&
                   strncmp(tname, "w:ilvl", 6) == 0 &&
                   (tname[6] == ' ' || tname[6] == '/' ||
                    tname[6] == '\0')) {
            if (num_get_attr(tag, "w:val", val, sizeof(val)) == 0) {
                int il = num_atoi(val);
                if (il >= 0 && il < NUM_MAX_LEVELS) {
                    num_styles[cur_style].ilvl = il;
                }
            }
        }
        pos = end + 1;
    }
}

static const num_abstract *num_find_abstract(int id) {
    for (int i = 0; i < NUM_MAX_ABSTRACT; i++) {
        if (num_abstracts[i].id == id) {
            return &num_abstracts[i];
        }
    }
    return NULL;
}

/* numId → abstract と段落。使う段落 (ilvl) を返す。無ければ -1。 */
static int num_lookup_num(int num_id, int ilvl,
                          const num_abstract **abst_out,
                          const num_level **lvl_out) {
    int abstract_id = -1;
    for (int i = 0; i < NUM_MAX_NUMS; i++) {
        if (num_defs[i].num_id == num_id) {
            abstract_id = num_defs[i].abstract_id;
            break;
        }
    }
    if (abstract_id < 0) {
        return -1;
    }
    const num_abstract *a = num_find_abstract(abstract_id);
    if (a == NULL) {
        return -1;
    }
    int l = ilvl;
    if (l < 0) {
        l = 0;
    }
    if (l >= NUM_MAX_LEVELS) {
        l = NUM_MAX_LEVELS - 1;
    }
    if (!a->levels[l].defined) {
        if (a->levels[0].defined) {
            l = 0;
        } else {
            return -1;
        }
    }
    *abst_out = a;
    *lvl_out = &a->levels[l];
    return l;
}

/* 様式名 → 番号づけ。styles.xml の様式定義か、番号づけ定義の
 * pStyle の結びつけから引く。使う段落を返す。無ければ -1。 */
static int num_lookup_style(const char *style_id, int *key_out,
                            const num_abstract **abst_out,
                            const num_level **lvl_out) {
    /* styles.xml の様式定義 (basedOn を最大 4 段たどる)。 */
    const char *cur = style_id;
    for (int hop = 0; hop < 4 && cur != NULL && cur[0] != '\0'; hop++) {
        const num_style *st = NULL;
        for (int i = 0; i < NUM_MAX_STYLES; i++) {
            if (strcmp(num_styles[i].style_id, cur) == 0) {
                st = &num_styles[i];
                break;
            }
        }
        if (st == NULL) {
            break;
        }
        if (st->num_id >= 0) {
            const num_abstract *a = NULL;
            const num_level *lv = NULL;
            int l = num_lookup_num(st->num_id, st->ilvl, &a, &lv);
            if (l >= 0) {
                *key_out = st->num_id;
                *abst_out = a;
                *lvl_out = lv;
                return l;
            }
            break;
        }
        cur = st->based_on[0] != '\0' ? st->based_on : NULL;
    }
    /* 番号づけ定義の中の pStyle の結びつけ。 */
    for (int i = 0; i < NUM_MAX_ABSTRACT; i++) {
        if (num_abstracts[i].id < 0) {
            continue;
        }
        for (int l = 0; l < NUM_MAX_LEVELS; l++) {
            if (num_abstracts[i].levels[l].defined &&
                strcmp(num_abstracts[i].levels[l].style, style_id) == 0) {
                *key_out = NUM_STYLE_KEY + num_abstracts[i].id;
                *abst_out = &num_abstracts[i];
                *lvl_out = &num_abstracts[i].levels[l];
                return l;
            }
        }
    }
    return -1;
}

/* key + 段落の番号を 1 つ進め、その番号を返す。深い段落の
 * 番号は振り直す (上の段落が進んだら下は 1 から)。 */
static int num_next_number(int key, int ilvl, int start) {
    for (int i = 0; i < NUM_MAX_COUNTERS; i++) {
        if (num_counters[i].started && num_counters[i].key == key &&
            num_counters[i].level > ilvl) {
            num_counters[i].started = 0;
        }
    }
    for (int i = 0; i < NUM_MAX_COUNTERS; i++) {
        if (num_counters[i].started && num_counters[i].key == key &&
            num_counters[i].level == ilvl) {
            return ++num_counters[i].value;
        }
    }
    for (int i = 0; i < NUM_MAX_COUNTERS; i++) {
        if (!num_counters[i].started) {
            num_counters[i].started = 1;
            num_counters[i].key = key;
            num_counters[i].level = ilvl;
            num_counters[i].value = start;
            return start;
        }
    }
    return start; /* 表が満杯でも番号だけは出す */
}

/* key + 段落の今の番号 (数え始めていなければ開始番号)。 */
static int num_current_number(int key, int ilvl, int start) {
    for (int i = 0; i < NUM_MAX_COUNTERS; i++) {
        if (num_counters[i].started && num_counters[i].key == key &&
            num_counters[i].level == ilvl) {
            return num_counters[i].value;
        }
    }
    return start;
}

/* 番号づけの接頭辞 ("1. " や "- ") を out に作る。
 * 戻り値は書いた長さ。番号なしは 0。 */
static int num_build_prefix(int key, const num_abstract *abst,
                            const num_level *lvl, int ilvl, char *out,
                            size_t cap) {
    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (lvl->fmt == NUM_FMT_NONE) {
        return 0;
    }
    if (lvl->fmt == NUM_FMT_BULLET) {
        if (cap < 3) {
            return 0;
        }
        memcpy(out, "- ", 2);
        out[2] = '\0';
        return 2;
    }
    int start = lvl->start < 1 ? 1 : lvl->start;
    int n = num_next_number(key, ilvl, start);
    if (lvl->text[0] == '\0') {
        char num[24];
        num_format_number(lvl->fmt, n, num, sizeof(num));
        size_t nl = strlen(num);
        if (nl + 2 >= cap) {
            return 0;
        }
        memcpy(out, num, nl);
        out[nl++] = '.';
        out[nl++] = ' ';
        out[nl] = '\0';
        return (int)nl;
    }
    /* lvlText の "%1" などを番号に置き換える。 */
    size_t o = 0;
    for (size_t i = 0; lvl->text[i] != '\0'; i++) {
        if (lvl->text[i] == '%' && lvl->text[i + 1] >= '1' &&
            lvl->text[i + 1] <= '9') {
            int dl = lvl->text[i + 1] - '1';
            int dstart = 1;
            num_fmt dfmt = lvl->fmt;
            if (dl < NUM_MAX_LEVELS && abst->levels[dl].defined) {
                dstart = abst->levels[dl].start < 1
                             ? 1
                             : abst->levels[dl].start;
                dfmt = abst->levels[dl].fmt;
            }
            int cn = (dl == ilvl)
                         ? n
                         : num_current_number(key, dl, dstart);
            char num[24];
            num_format_number(dfmt, cn, num, sizeof(num));
            size_t nl = strlen(num);
            if (o + nl >= cap) {
                return 0;
            }
            memcpy(out + o, num, nl);
            o += nl;
            i++;
        } else {
            if (o + 1 >= cap) {
                return 0;
            }
            out[o++] = lvl->text[i];
        }
    }
    if (o == 0) {
        return 0;
    }
    if (out[o - 1] != ' ' && o + 1 < cap) {
        out[o++] = ' ';
    }
    out[o] = '\0';
    return (int)o;
}

/* --- 読み込み: 最小の XML 走査 (shell/xml.h 共用) --- */

int docx_import(const uint8_t *data, size_t len, char *out, size_t cap) {
    if (data == NULL || out == NULL || cap == 0) {
        return -1;
    }
    /* 静的な表にする (カーネルのスタックを圧迫しないため)。 */
    static zip_entry entries[ZIP_MAX_ENTRIES];
    int n = zip_read_entries(data, len, entries, ZIP_MAX_ENTRIES);
    if (n < 0) {
        return -1;
    }
    const zip_entry *e = zip_find_entry(entries, n, "word/document.xml");
    if (e == NULL) {
        return -1;
    }
    /* 番号づけの表を初期化し、numbering.xml / styles.xml が
     * あれば読む (無くても・大きすぎても読み込みは続ける)。 */
    for (int i = 0; i < NUM_MAX_ABSTRACT; i++) {
        num_abstracts[i].id = -1;
        for (int l = 0; l < NUM_MAX_LEVELS; l++) {
            num_abstracts[i].levels[l].defined = 0;
            num_abstracts[i].levels[l].start = 1;
            num_abstracts[i].levels[l].fmt = NUM_FMT_DECIMAL;
            num_abstracts[i].levels[l].text[0] = '\0';
            num_abstracts[i].levels[l].style[0] = '\0';
        }
    }
    for (int i = 0; i < NUM_MAX_NUMS; i++) {
        num_defs[i].num_id = -1;
        num_defs[i].abstract_id = -1;
    }
    for (int i = 0; i < NUM_MAX_STYLES; i++) {
        num_styles[i].style_id[0] = '\0';
        num_styles[i].based_on[0] = '\0';
        num_styles[i].num_id = -1;
        num_styles[i].ilvl = 0;
    }
    for (int i = 0; i < NUM_MAX_COUNTERS; i++) {
        num_counters[i].started = 0;
    }
    static uint8_t num_xml[16384];
    const zip_entry *ne =
        zip_find_entry(entries, n, "word/numbering.xml");
    if (ne != NULL) {
        int nl = zip_read_entry(data, len, ne, num_xml, sizeof(num_xml));
        if (nl > 0) {
            num_parse_numbering((const char *)num_xml, (size_t)nl);
        }
    }
    static uint8_t styles_xml[16384];
    const zip_entry *se = zip_find_entry(entries, n, "word/styles.xml");
    if (se != NULL) {
        int sl = zip_read_entry(data, len, se, styles_xml,
                                sizeof(styles_xml));
        if (sl > 0) {
            num_parse_styles((const char *)styles_xml, (size_t)sl);
        }
    }
    static uint8_t xml[16384];
    int xml_len = zip_read_entry(data, len, e, xml, sizeof(xml));
    if (xml_len < 0) {
        return -1;
    }
    const char *s = (const char *)xml;
    size_t slen = (size_t)xml_len;

    size_t out_len = 0;
    out[0] = '\0';
    int in_para = 0;      /* <w:p> の中か */
    int para_started = 0; /* 段落を 1 つでも出したか */
    int para_style = 0;   /* 0=ふつう 1=H1 2=H2 3=H3 4=箇条書き */
    int run_bold = 0;     /* 今の run が太字か */
    size_t para_start = 0; /* 今の段落の出始め (out 内) */
    char para_style_id[32]; /* w:pStyle の値 */
    int para_num_id = -1;   /* 直接の w:numPr の numId (-1 は無し) */
    int para_ilvl = 0;      /* 直接の w:numPr の ilvl */
    int para_has_num = 0;   /* 直接の w:numPr があるか */
    int para_head_len = 0;  /* 見出しの目印の長さ */
    int para_prefix_done = 0; /* 番号づけの接頭辞を入れたか */
    int in_numpr = 0;         /* <w:numPr> の中か */
    size_t pos = 0;
    while (pos < slen) {
        if (s[pos] != '<') {
            pos++;
            continue;
        }
        /* タグの終わりを探す。 */
        size_t end = pos + 1;
        while (end < slen && s[end] != '>') {
            end++;
        }
        if (end >= slen) {
            return -1;
        }
        size_t tag_len = end - pos - 1;
        char tag[64];
        size_t tl = tag_len > sizeof(tag) - 1 ? sizeof(tag) - 1 : tag_len;
        memcpy(tag, s + pos + 1, tl);
        tag[tl] = '\0';
        int closing = tag[0] == '/';
        const char *tname = closing ? tag + 1 : tag;
        /* self_close は元文字列で判定する (tag[] はクランプ後の
         * コピーで、tag[tag_len-1] は範囲外になりうる)。 */
        int self_close = tag_len > 0 && s[end - 1] == '/';

        if (!closing && strncmp(tname, "w:p", 3) == 0 &&
            (tname[3] == '\0' || tname[3] == ' ' || tname[3] == '/')) {
            if (para_started) {
                if (out_len + 1 >= cap) {
                    return -1;
                }
                out[out_len++] = '\n';
                out[out_len] = '\0';
            }
            para_started = 1;
            in_para = 1;
            para_style = 0;
            para_start = out_len;
            para_style_id[0] = '\0';
            para_num_id = -1;
            para_ilvl = 0;
            para_has_num = 0;
            para_head_len = 0;
            para_prefix_done = 0;
            in_numpr = 0;
            if (self_close) {
                in_para = 0;
            }
        } else if (closing && strcmp(tname, "w:p") == 0) {
            /* 段落の終わり: 先頭の • を "- " に直す (自作の
             * .docx の箇条書きを元の形に戻す)。 */
            if (out_len - para_start >= 4 &&
                (unsigned char)out[para_start] == 0xE2 &&
                (unsigned char)out[para_start + 1] == 0x80 &&
                (unsigned char)out[para_start + 2] == 0xA2 &&
                out[para_start + 3] == ' ') {
                /* 先頭 4 バイトを "- " (2 バイト) に置き換える。 */
                memmove(out + para_start + 2, out + para_start + 4,
                        out_len - para_start - 4);
                out[para_start] = '-';
                out[para_start + 1] = ' ';
                out_len -= 2;
                out[out_len] = '\0';
            }
            in_para = 0;
        } else if (in_para && !closing &&
                   strncmp(tname, "w:pStyle", 8) == 0) {
            if (strstr(tag, "Heading1") != NULL) {
                para_style = 1;
            } else if (strstr(tag, "Heading2") != NULL) {
                para_style = 2;
            } else if (strstr(tag, "Heading3") != NULL) {
                para_style = 3;
            } else if (strstr(tag, "ListParagraph") != NULL) {
                para_style = 4;
            }
            /* 見出しの目印は段落の先頭に付ける。 */
            if (para_style >= 1 && para_style <= 3) {
                const char *mark =
                    para_style == 1 ? "# " : (para_style == 2 ? "## "
                                                             : "### ");
                size_t ml = strlen(mark);
                if (out_len + ml >= cap) {
                    return -1;
                }
                /* 段落の先頭に差し込む。 */
                memmove(out + para_start + ml, out + para_start,
                        out_len - para_start);
                memcpy(out + para_start, mark, ml);
                out_len += ml;
                out[out_len] = '\0';
                para_head_len = (int)ml;
            }
            if (num_get_attr(tag, "w:val", para_style_id,
                             sizeof(para_style_id)) != 0) {
                para_style_id[0] = '\0';
            }
        } else if (in_para && !closing &&
                   strncmp(tname, "w:numPr", 7) == 0 &&
                   (tname[7] == ' ' || tname[7] == '/' ||
                    tname[7] == '\0')) {
            if (para_style == 0) {
                para_style = 4;
            }
            para_has_num = 1;
            in_numpr = !self_close;
        } else if (in_para && closing &&
                   strcmp(tname, "w:numPr") == 0) {
            in_numpr = 0;
        } else if (in_para && in_numpr && !closing &&
                   strncmp(tname, "w:numId", 7) == 0 &&
                   (tname[7] == ' ' || tname[7] == '/' ||
                    tname[7] == '\0')) {
            char v[32];
            if (num_get_attr(tag, "w:val", v, sizeof(v)) == 0) {
                int id = num_atoi(v);
                if (id >= 0) {
                    para_num_id = id;
                }
            }
        } else if (in_para && in_numpr && !closing &&
                   strncmp(tname, "w:ilvl", 6) == 0 &&
                   (tname[6] == ' ' || tname[6] == '/' ||
                    tname[6] == '\0')) {
            char v[32];
            if (num_get_attr(tag, "w:val", v, sizeof(v)) == 0) {
                int il = num_atoi(v);
                if (il >= 0 && il < NUM_MAX_LEVELS) {
                    para_ilvl = il;
                }
            }
        } else if (in_para && closing && strcmp(tname, "w:pPr") == 0) {
            /* 段落書式が揃ったので、番号づけの接頭辞を決める。
             * 直接の numPr が無ければ様式から引く。 */
            if (!para_prefix_done) {
                para_prefix_done = 1;
                char prefix[64];
                int plen = 0;
                if (para_has_num) {
                    const num_abstract *a = NULL;
                    const num_level *lv = NULL;
                    int l = (para_num_id >= 0)
                                ? num_lookup_num(para_num_id, para_ilvl,
                                                 &a, &lv)
                                : -1;
                    if (l >= 0) {
                        plen = num_build_prefix(para_num_id, a, lv, l,
                                                prefix, sizeof(prefix));
                    } else {
                        /* 番号づけが分からない: 箇条書きに落とす。 */
                        memcpy(prefix, "- ", 2);
                        prefix[2] = '\0';
                        plen = 2;
                    }
                } else if (para_style_id[0] != '\0') {
                    int key = 0;
                    const num_abstract *a = NULL;
                    const num_level *lv = NULL;
                    int l = num_lookup_style(para_style_id, &key, &a,
                                             &lv);
                    if (l >= 0) {
                        plen = num_build_prefix(key, a, lv, l, prefix,
                                                sizeof(prefix));
                    }
                }
                if (plen > 0) {
                    size_t at = para_start + (size_t)para_head_len;
                    if (out_len + (size_t)plen >= cap) {
                        return -1;
                    }
                    memmove(out + at + plen, out + at, out_len - at);
                    memcpy(out + at, prefix, (size_t)plen);
                    out_len += (size_t)plen;
                    out[out_len] = '\0';
                    if (para_style == 0) {
                        para_style = 4;
                    }
                }
            }
        } else if (in_para && !closing &&
                   strncmp(tname, "w:r", 3) == 0 &&
                   (tname[3] == '\0' || tname[3] == ' ' ||
                    tname[3] == '/')) {
            run_bold = 0;
        } else if (in_para && !closing &&
                   strncmp(tname, "w:b", 3) == 0 &&
                   (tname[3] == '\0' || tname[3] == ' ' ||
                    tname[3] == '/')) {
            /* <w:b w:val="false"/> は太字でない。 */
            if (strstr(tag, "false") == NULL &&
                strstr(tag, "w:val=\"0\"") == NULL) {
                run_bold = 1;
            }
        } else if (in_para && !closing &&
                   strncmp(tname, "w:t", 3) == 0 &&
                   (tname[3] == '\0' || tname[3] == ' ' ||
                    tname[3] == '/')) {
            if (self_close) {
                /* <w:t/>: 空の run。 */
                pos = end + 1;
                continue;
            }
            size_t text_start = out_len;
            size_t after = xml_read_text(s, slen, end + 1, "</w:t>",
                                         out, &out_len, cap);
            if (after == (size_t)-1) {
                return -1;
            }
            if (run_bold && out_len > text_start) {
                /* 太字の run を ** で囲む。 */
                if (out_len + 4 >= cap) {
                    return -1;
                }
                memmove(out + text_start + 2, out + text_start,
                        out_len - text_start);
                out[text_start] = '*';
                out[text_start + 1] = '*';
                out_len += 2;
                out[out_len++] = '*';
                out[out_len++] = '*';
                out[out_len] = '\0';
            }
            pos = after;
            continue;
        } else if (in_para && !closing &&
                   strncmp(tname, "w:br", 4) == 0 &&
                   (tname[4] == '\0' || tname[4] == ' ' ||
                    tname[4] == '/')) {
            if (out_len + 1 >= cap) {
                return -1;
            }
            out[out_len++] = '\n';
            out[out_len] = '\0';
        } else if (in_para && !closing &&
                   strncmp(tname, "w:tab", 5) == 0 &&
                   (tname[5] == '\0' || tname[5] == ' ' ||
                    tname[5] == '/')) {
            if (out_len + 1 >= cap) {
                return -1;
            }
            out[out_len++] = ' ';
            out[out_len] = '\0';
        }
        pos = end + 1;
    }
    out[out_len] = '\0';
    return (int)out_len;
}
