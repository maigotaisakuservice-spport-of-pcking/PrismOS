/* PrismOS — .pptx (OOXML) の読み書き。 */
#include "pptx.h"

#include <string.h>

#include "xml.h"
#include "zip.h"

#define PPTX_NS \
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" " \
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/" \
    "relationships\" " \
    "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/" \
    "main\""

/* r:id を使わない部品 (スライド・マスタ) 用。 */
#define PPTX_NS_NO_R \
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" " \
    "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/" \
    "main\""

static const char PPTX_XML_HEAD[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";

/* 文字列を buf の末尾に足す。長さは strlen で数える。 */
static int put(char *buf, size_t *n, size_t cap, const char *s) {
    size_t len = strlen(s);
    if (*n + len >= cap) {
        return -1;
    }
    memcpy(buf + *n, s, len);
    *n += len;
    return 0;
}

/* グループの枠 (スライド・マスタ・レイアウトで共用)。 */
static const char GRP_SP[] =
    "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/>"
    "<p:nvPr/></p:nvGrpSpPr>"
    "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
    "</a:xfrm></p:grpSpPr>";

/* タイトルと本文のプレースホルダ (中身は後で入れる)。 */
static int put_title_shape(char *buf, size_t *n, size_t cap,
                           const char *title) {
    char esc[SLIDES_TITLE_MAX * 6 + 1];
    if (xml_escape_text(title, strlen(title), esc, sizeof(esc)) < 0) {
        return -1;
    }
    if (put(buf, n, cap,
            "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title 1\"/>"
            "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
            "<p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
            "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/>"
            "<a:p><a:r><a:rPr lang=\"en-US\" sz=\"3200\"/><a:t>") != 0) {
        return -1;
    }
    if (put(buf, n, cap, esc) != 0) {
        return -1;
    }
    return put(buf, n, cap, "</a:t></a:r></a:p></p:txBody></p:sp>");
}

static int put_body_shape(char *buf, size_t *n, size_t cap,
                          const slide *sl) {
    if (put(buf, n, cap,
            "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Content 1\"/>"
            "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
            "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr>"
            "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/>") != 0) {
        return -1;
    }
    for (int i = 0; i < sl->line_count; i++) {
        char esc[SLIDES_LINE_MAX * 6 + 1];
        if (xml_escape_text(sl->lines[i], strlen(sl->lines[i]), esc,
                            sizeof(esc)) < 0) {
            return -1;
        }
        if (put(buf, n, cap,
                "<a:p><a:pPr><a:buChar char=\"\xE2\x80\xA2\"/>"
                "</a:pPr><a:r><a:rPr lang=\"en-US\"/>"
                "<a:t>") != 0 ||
            put(buf, n, cap, esc) != 0 ||
            put(buf, n, cap, "</a:t></a:r></a:p>") != 0) {
            return -1;
        }
    }
    return put(buf, n, cap, "</p:txBody></p:sp>");
}

/* マスタとレイアウトで使う空のタイトル・本文 (見本の文字)。 */
static const char MASTER_SHAPES[] =
    "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title 1\"/>"
    "<p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
    "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/>"
    "<a:p/></p:txBody></p:sp>"
    "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Text 1\"/>"
    "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr>"
    "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/>"
    "<a:p/></p:txBody></p:sp>";

static int build_master(char *buf, size_t cap) {
    size_t n = 0;
    if (put(buf, &n, cap, PPTX_XML_HEAD) != 0 ||
        put(buf, &n, cap, "<p:sldMaster ") != 0 ||
        put(buf, &n, cap, PPTX_NS_NO_R) != 0 ||
        put(buf, &n, cap, "><p:cSld><p:spTree>") != 0 ||
        put(buf, &n, cap, GRP_SP) != 0 ||
        put(buf, &n, cap, MASTER_SHAPES) != 0 ||
        put(buf, &n, cap,
            "</p:spTree></p:cSld>"
            "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" "
            "accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
            "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" "
            "hlink=\"hlink\" folHlink=\"folHlink\"/>"
            "</p:sldMaster>") != 0) {
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

static int build_slide(char *buf, size_t cap, const slide *sl) {
    size_t n = 0;
    if (put(buf, &n, cap, PPTX_XML_HEAD) != 0 ||
        put(buf, &n, cap, "<p:sld ") != 0 ||
        put(buf, &n, cap, PPTX_NS_NO_R) != 0 ||
        put(buf, &n, cap, "><p:cSld><p:spTree>") != 0 ||
        put(buf, &n, cap, GRP_SP) != 0) {
        return -1;
    }
    if (put_title_shape(buf, &n, cap, sl->title) != 0) {
        return -1;
    }
    if (put_body_shape(buf, &n, cap, sl) != 0) {
        return -1;
    }
    if (put(buf, &n, cap,
            "</p:spTree></p:cSld><p:clrMapOvr>"
            "<a:masterClrMapping/></p:clrMapOvr></p:sld>") != 0) {
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

static const char RELS_HEAD[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
    "2006/relationships\">";
static const char RELS_TAIL[] = "</Relationships>";

int pptx_export(const slides_state *s, uint8_t *out, size_t cap) {
    if (s == NULL || out == NULL || s->count <= 0) {
        return -1;
    }
    static char xml[16384];
    static char types[2048];
    static char pres[2048];
    static char rels[2048];
    size_t n = 0;

    /* [Content_Types].xml (スライドの枚数で変わる)。 */
    if (put(types, &n, sizeof(types),
            "<?xml version=\"1.0\" encoding=\"UTF-8\" "
            "standalone=\"yes\"?>\n"
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/"
            "2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd."
            "openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Override PartName=\"/ppt/presentation.xml\" "
            "ContentType=\"application/vnd.openxmlformats-officedocument."
            "presentationml.presentation.main+xml\"/>"
            "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" "
            "ContentType=\"application/vnd.openxmlformats-officedocument."
            "presentationml.slideMaster+xml\"/>") != 0) {
        return -1;
    }
    for (int i = 0; i < s->count; i++) {
        char part[160];
        size_t p = 0;
        const char *a = "<Override PartName=\"/ppt/slides/slide";
        const char *b =
            ".xml\" ContentType=\"application/vnd.openxmlformats-"
            "officedocument.presentationml.slide+xml\"/>";
        memcpy(part + p, a, strlen(a));
        p += strlen(a);
        part[p++] = (char)('1' + i);
        memcpy(part + p, b, strlen(b));
        p += strlen(b);
        part[p] = '\0';
        if (put(types, &n, sizeof(types), part) != 0) {
            return -1;
        }
    }
    if (put(types, &n, sizeof(types), "</Types>") != 0) {
        return -1;
    }

    /* ppt/presentation.xml。 */
    n = 0;
    if (put(pres, &n, sizeof(pres), PPTX_XML_HEAD) != 0 ||
        put(pres, &n, sizeof(pres), "<p:presentation ") != 0 ||
        put(pres, &n, sizeof(pres), PPTX_NS) != 0 ||
        put(pres, &n, sizeof(pres),
            "><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" "
            "r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>") != 0) {
        return -1;
    }
    for (int i = 0; i < s->count; i++) {
        char id[64];
        size_t p = 0;
        const char *a = "<p:sldId id=\"";
        const char *b = "\" r:id=\"rId";
        const char *c = "\"/>";
        memcpy(id + p, a, strlen(a));
        p += strlen(a);
        int num = 256 + i;
        char numbuf[8];
        int nd = 0;
        if (num >= 100) {
            numbuf[nd++] = (char)('0' + num / 100);
        }
        numbuf[nd++] = (char)('0' + (num / 10) % 10);
        numbuf[nd++] = (char)('0' + num % 10);
        memcpy(id + p, numbuf, (size_t)nd);
        p += (size_t)nd;
        memcpy(id + p, b, strlen(b));
        p += strlen(b);
        id[p++] = (char)('2' + i);
        memcpy(id + p, c, strlen(c));
        p += strlen(c);
        id[p] = '\0';
        if (put(pres, &n, sizeof(pres), id) != 0) {
            return -1;
        }
    }
    if (put(pres, &n, sizeof(pres),
            "</p:sldIdLst><p:sldSz cx=\"9144000\" cy=\"6858000\"/>"
            "<p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
            "</p:presentation>") != 0) {
        return -1;
    }

    /* _rels/.rels と ppt/_rels/presentation.xml.rels。 */
    static char root_rels[1024];
    n = 0;
    if (put(root_rels, &n, sizeof(root_rels), RELS_HEAD) != 0 ||
        put(root_rels, &n, sizeof(root_rels),
            "<Relationship Id=\"rId1\" Type=\"http://schemas."
            "openxmlformats.org/officeDocument/2006/relationships/"
            "officeDocument\" Target=\"ppt/presentation.xml\"/>") != 0 ||
        put(root_rels, &n, sizeof(root_rels), RELS_TAIL) != 0) {
        return -1;
    }
    n = 0;
    if (put(rels, &n, sizeof(rels), RELS_HEAD) != 0 ||
        put(rels, &n, sizeof(rels),
            "<Relationship Id=\"rId1\" Type=\"http://schemas."
            "openxmlformats.org/officeDocument/2006/relationships/"
            "slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>") !=
            0) {
        return -1;
    }
    for (int i = 0; i < s->count; i++) {
        char rel[256];
        size_t p = 0;
        const char *a =
            "<Relationship Id=\"rId";
        const char *b =
            "\" Type=\"http://schemas.openxmlformats.org/"
            "officeDocument/2006/relationships/slide\" Target=\"slides/"
            "slide";
        const char *c = ".xml\"/>";
        memcpy(rel + p, a, strlen(a));
        p += strlen(a);
        rel[p++] = (char)('2' + i);
        memcpy(rel + p, b, strlen(b));
        p += strlen(b);
        rel[p++] = (char)('1' + i);
        memcpy(rel + p, c, strlen(c));
        p += strlen(c);
        rel[p] = '\0';
        if (put(rels, &n, sizeof(rels), rel) != 0) {
            return -1;
        }
    }
    if (put(rels, &n, sizeof(rels), RELS_TAIL) != 0) {
        return -1;
    }

    /* 静的に置く (64 件分の表が入るためスタックに置けない)。 */
    static zip_writer w;
    zip_write_begin(&w, out, cap);
    if (zip_write_add_deflated(&w, "[Content_Types].xml",
                               (const uint8_t *)types, strlen(types)) !=
            0 ||
        zip_write_add_deflated(&w, "_rels/.rels",
                               (const uint8_t *)root_rels,
                               strlen(root_rels)) != 0 ||
        zip_write_add_deflated(&w, "ppt/presentation.xml",
                               (const uint8_t *)pres, strlen(pres)) != 0 ||
        zip_write_add_deflated(&w, "ppt/_rels/presentation.xml.rels",
                               (const uint8_t *)rels, strlen(rels)) != 0) {
        return -1;
    }
    int master_len = build_master(xml, sizeof(xml));
    if (master_len < 0) {
        return -1;
    }
    if (zip_write_add_deflated(&w, "ppt/slideMasters/slideMaster1.xml",
                               (const uint8_t *)xml,
                               (size_t)master_len) != 0) {
        return -1;
    }
    /* レイアウトファイルと rels は出さない。マスタはレイアウト
     * を参照しないので、マスタの rels も不要。 */
    for (int i = 0; i < s->count; i++) {
        int slide_len = build_slide(xml, sizeof(xml), &s->slides[i]);
        if (slide_len < 0) {
            return -1;
        }
        char name[32];
        size_t p = 0;
        const char *a = "ppt/slides/slide";
        const char *b = ".xml";
        memcpy(name + p, a, strlen(a));
        p += strlen(a);
        name[p++] = (char)('1' + i);
        memcpy(name + p, b, strlen(b));
        p += strlen(b);
        name[p] = '\0';
        if (zip_write_add_deflated(&w, name, (const uint8_t *)xml,
                                   (size_t)slide_len) != 0) {
            return -1;
        }
        /* スライドごとの rels ファイルは出さない (レイアウトは
         * 既定を使う)。16384 バイトのファイル制限に収めるため。 */
    }
    return zip_write_finish(&w);
}

/* --- 読み込み --- */

int pptx_import(const uint8_t *data, size_t len, slides_state *out) {
    if (data == NULL || out == NULL) {
        return -1;
    }
    /* 静的な表にする (カーネルのスタックを圧迫しないため)。 */
    static zip_entry entries[ZIP_MAX_ENTRIES];
    int n = zip_read_entries(data, len, entries, ZIP_MAX_ENTRIES);
    if (n < 0) {
        return -1;
    }
    slides_clear(out);
    out->count = 0;
    static uint8_t xml[16384];
    for (int i = 1; i <= SLIDES_MAX; i++) {
        char name[32];
        size_t p = 0;
        const char *a = "ppt/slides/slide";
        const char *b = ".xml";
        memcpy(name + p, a, strlen(a));
        p += strlen(a);
        name[p++] = (char)('0' + i);
        memcpy(name + p, b, strlen(b));
        p += strlen(b);
        name[p] = '\0';
        const zip_entry *e = zip_find_entry(entries, n, name);
        if (e == NULL) {
            break;
        }
        int xml_len = zip_read_entry(data, len, e, xml, sizeof(xml));
        if (xml_len < 0) {
            return -1;
        }
        const char *s = (const char *)xml;
        size_t slen = (size_t)xml_len;
        /* <a:t> を順に読む。最初がタイトル、残りが箇条書き。 */
        int idx = out->count++;
        out->slides[idx].title[0] = '\0';
        out->slides[idx].line_count = 0;
        int seen_text = 0;
        size_t pos = 0;
        while (pos < slen) {
            if (s[pos] != '<') {
                pos++;
                continue;
            }
            size_t end = pos + 1;
            while (end < slen && s[end] != '>') {
                end++;
            }
            if (end >= slen) {
                return -1;
            }
            size_t tag_len = end - pos - 1;
            char tag[32];
            size_t tl = tag_len > sizeof(tag) - 1 ? sizeof(tag) - 1
                                                  : tag_len;
            memcpy(tag, s + pos + 1, tl);
            tag[tl] = '\0';
            int closing = tag[0] == '/';
            const char *tname = closing ? tag + 1 : tag;
            if (!closing && strncmp(tname, "a:t", 3) == 0 &&
                (tname[3] == '\0' || tname[3] == ' ' ||
                 tname[3] == '/')) {
                /* self_close は元文字列で判定する (tag[] は
                 * クランプ後のコピーで範囲外になりうる)。 */
                if (tag_len > 0 && s[end - 1] == '/') {
                    pos = end + 1;
                    continue;
                }
                /* 長いテキストは set/add 側で切り詰める。 */
                char text[512];
                size_t tlen = 0;
                size_t after = xml_read_text(s, slen, end + 1, "</a:t>",
                                             text, &tlen, sizeof(text));
                if (after == (size_t)-1) {
                    return -1;
                }
                text[tlen] = '\0';
                if (seen_text == 0) {
                    slides_set_title(out, idx, text);
                } else {
                    slides_add_line(out, idx, text);
                }
                seen_text++;
                pos = after;
                continue;
            }
            pos = end + 1;
        }
    }
    if (out->count == 0) {
        slides_clear(out);
        return -1;
    }
    return out->count;
}
