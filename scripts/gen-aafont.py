#!/usr/bin/env python3
"""PrismOS anti-aliased font generator.

Rasterises the same repertoire as the bitmap Japanese font
(the codepoint list in kernel/jfont_index.h, plus printable
ASCII) from Noto Sans CJK (SIL Open Font License 1.1; see
assets/NotoSansCJK-LICENSE.txt) at several pixel sizes and
emits kernel/aafont_data.c + kernel/aafont_tables.h plus the
glyph blob part files kernel/aafont_blob<N>.c:

  * coverage is stored at 4 bits per pixel (16 levels),
    packed two pixels per byte, each row padded to a whole
    byte, then PackBits-compressed per glyph;
  * every glyph record carries its real advance (1/64 px),
    ink box size and ink offset from the pen origin
    (x = left bearing, y = baseline-relative, negative up),
    so Latin text is proportional and CJK text is full-width;
  * master sizes: Regular 12/16/32, Bold 16/32. Other sizes
    are resampled at draw time by the kernel;
  * each master's compressed blob is split into consecutive
    parts of PRISM_AA_PART_LEN bytes (see aafont_tables.h);
    the parts live in the aafont_blob<N>.c files, grouped so
    every generated file stays small, and aafont_data.c
    carries a pointer table per master. The byte content is
    exactly the unsplit blob, in order.

Usage: python3 scripts/gen-aafont.py [regular-ttc] [bold-ttc]
Defaults are the system Noto Sans CJK TTCs (face index 0 is
the Japanese face). Durable copies of the fonts are kept in
~/workspace/tools/fonts-noto-cjk/.
"""

import re
import sys

from fontTools.ttLib import TTFont
from PIL import ImageFont

DEFAULT_REG = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
DEFAULT_BOLD = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
OUT_C = "kernel/aafont_data.c"
OUT_H = "kernel/aafont_tables.h"
OUT_BLOB_FMT = "kernel/aafont_blob%d.c"

# Blob split: parts of PART_LEN bytes each; blob files are
# flushed before their C text would exceed the budget, so
# every generated file stays well under repository limits.
PART_LEN = 131072  # 2^17, matches PRISM_AA_PART_* in the header
BLOB_FILE_BUDGET = 5600000

# (weight name, sizes) -- weight 0 = Regular, 1 = Bold
PLAN = [("reg", [12, 16, 32]), ("bold", [16, 32])]


def load_codepoints():
    src = open("kernel/jfont_index.h").read()
    cps = sorted(set(int(x, 16) for x in
                     re.findall(r"0x([0-9A-Fa-f]{4})", src)))
    return cps


def packbits(data):
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 128:
            run += 1
        if run >= 3:
            out.append(257 - run)
            out.append(data[i])
            i += run
        else:
            start = i
            lit = 0
            while i < n and lit < 128:
                r = 1
                while (i + r < n and data[i + r] == data[i]
                       and r < 3):
                    r += 1
                if r >= 3 and lit > 0:
                    break
                lit += 1
                i += 1
            out.append(lit - 1)
            out.extend(data[start:i])
    return bytes(out)


def quantise_pack(mask_bytes, w, h):
    """4-bit quantise + nibble-pack (row padded to whole bytes)."""
    packed = bytearray()
    stride = (w + 1) // 2
    for y in range(h):
        row = mask_bytes[y * w:(y + 1) * w]
        for i in range(0, w, 2):
            hi = row[i] >> 4
            lo = (row[i + 1] >> 4) if i + 1 < w else 0
            packed.append((hi << 4) | lo)
    assert len(packed) == stride * h
    return bytes(packed)


def build_table(font_path, size, cps):
    """Return (ascent, descent, ascii_recs, wide_recs, blob).

    A record is (blob_off, adv64, ox, oy, w, h); adv64 == 0
    marks a glyph the font does not have (the kernel falls
    back). Blank-but-real glyphs (spaces) keep their advance
    with w == h == 0 and off == 0xFFFFFFFF.
    """
    tt = TTFont(font_path, fontNumber=0)
    cmap = tt.getBestCmap()
    f = ImageFont.truetype(font_path, size, index=0)
    ascent, descent = f.getmetrics()
    blob = bytearray()

    def one(cp):
        ch = chr(cp)
        if cp not in cmap:
            return (0, 0, 0, 0, 0, 0)
        adv = int(round(f.getlength(ch) * 64))
        if adv <= 0:
            adv = size * 64 if cp >= 0x80 else (size // 2) * 64
        mask, off = f.getmask2(ch, mode="L")
        w, h = mask.size
        if w == 0 or h == 0:
            # Real but blank (space / full-width space).
            return (0xFFFFFFFF, adv, 0, 0, 0, 0)
        ox = off[0]
        oy = off[1] - ascent  # baseline-relative (negative = up)
        data = packbits(quantise_pack(bytes(mask), w, h))
        rec = (len(blob), adv, ox, oy, w, h)
        blob.extend(data)
        return rec

    ascii_recs = [one(cp) for cp in range(32, 127)]
    wide_recs = [one(cp) for cp in cps]
    return ascent, descent, ascii_recs, wide_recs, bytes(blob)


def emit_rec(f, rec):
    off, adv, ox, oy, w, h = rec
    f.write("    { 0x%08X, %d, %d, %d, %d, %d },\n"
            % (off, adv, ox, oy, w, h))


def part_text(tag, idx, data):
    out = ["const uint8_t aa_part_%s_%d[%d] = {\n"
           % (tag, idx, len(data))]
    for i in range(0, len(data), 16):
        row = ", ".join("0x%02X" % b for b in data[i:i + 16])
        out.append("    %s,\n" % row)
    out.append("};\n\n")
    return "".join(out)


BLOB_FILE_HEADER = (
    "/* PrismOS - anti-aliased glyph blob parts.\n"
    " *\n"
    " * Generated by scripts/gen-aafont.py from Noto\n"
    " * Sans CJK Regular / Bold (SIL Open Font License\n"
    " * 1.1; the license text is in\n"
    " * assets/NotoSansCJK-LICENSE.txt). Coverage is\n"
    " * 4 bits per pixel, PackBits-compressed per glyph.\n"
    " * Do not edit by hand; regenerate instead. */\n"
    '#include "aafont.h"\n\n')


class BlobFiles:
    """Writes part arrays into a sequence of .c files,
    starting a new file before the budget is exceeded."""

    def __init__(self):
        self.n = 0
        self.f = None
        self.size = 0

    def _open(self):
        self.n += 1
        self.f = open(OUT_BLOB_FMT % self.n, "w")
        self.f.write(BLOB_FILE_HEADER)
        self.size = len(BLOB_FILE_HEADER)

    def add(self, text):
        if self.f is None or (
                self.size > len(BLOB_FILE_HEADER)
                and self.size + len(text) > BLOB_FILE_BUDGET):
            self.close()
            self._open()
        self.f.write(text)
        self.size += len(text)

    def close(self):
        if self.f is not None:
            self.f.close()
            self.f = None


def main():
    reg_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_REG
    bold_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_BOLD
    paths = {"reg": reg_path, "bold": bold_path}
    cps = load_codepoints()
    print("codepoints: %d wide + 95 ascii" % len(cps))

    tables = []  # (weight_idx, size, ascent, descent, blob_len)
    blob_files = BlobFiles()
    with open(OUT_C, "w") as f:
        f.write("/* PrismOS - anti-aliased glyph data.\n")
        f.write(" *\n")
        f.write(" * Generated by scripts/gen-aafont.py from Noto\n")
        f.write(" * Sans CJK Regular / Bold (SIL Open Font License\n")
        f.write(" * 1.1; the license text is in\n")
        f.write(" * assets/NotoSansCJK-LICENSE.txt). Coverage is\n")
        f.write(" * 4 bits per pixel, PackBits-compressed per glyph.\n")
        f.write(" * The compressed blobs are split into parts of\n")
        f.write(" * PRISM_AA_PART_LEN bytes in the aafont_blob*.c\n")
        f.write(" * files; this file carries the record tables and\n")
        f.write(" * one part-pointer table per master size.\n")
        f.write(" * Do not edit by hand; regenerate instead. */\n")
        f.write('#include "aafont.h"\n\n')
        f.write("const uint16_t prism_aa_cps[PRISM_AA_COUNT] = {\n")
        for i in range(0, len(cps), 8):
            row = ", ".join("0x%04X" % cp for cp in cps[i:i + 8])
            f.write("    %s,\n" % row)
        f.write("};\n\n")
        for wi, (wname, sizes) in enumerate(PLAN):
            for size in sizes:
                asc, desc, arecs, wrecs, blob = build_table(
                    paths[wname], size, cps)
                tag = "%s%d" % (wname, size)
                nparts = (len(blob) + PART_LEN - 1) // PART_LEN
                print("%s: asc %d desc %d blob %.2f MB in %d parts"
                      % (tag, asc, desc, len(blob) / 1048576.0,
                         nparts))
                for pi in range(nparts):
                    chunk = blob[pi * PART_LEN:(pi + 1) * PART_LEN]
                    blob_files.add(part_text(tag, pi, chunk))
                    f.write("extern const uint8_t aa_part_%s_%d[%d];\n"
                            % (tag, pi, len(chunk)))
                f.write("static const uint8_t *const aa_parts_%s[%d]"
                        " = {\n" % (tag, nparts))
                for pi in range(nparts):
                    f.write("    aa_part_%s_%d,\n" % (tag, pi))
                f.write("};\n\n")
                f.write("static const prism_aa_rec aa_ascii_%s[%d] = {\n"
                        % (tag, 96))
                for rec in arecs:
                    emit_rec(f, rec)
                f.write("    { %d, 0, 0, 0, 0, 0 },\n" % len(blob))
                f.write("};\n\n")
                f.write("static const prism_aa_rec aa_wide_%s[%d] = {\n"
                        % (tag, len(wrecs) + 1))
                for rec in wrecs:
                    emit_rec(f, rec)
                f.write("    { %d, 0, 0, 0, 0, 0 },\n" % len(blob))
                f.write("};\n\n")
                tables.append((wi, size, asc, desc, len(blob), tag))
        f.write("const prism_aa_size prism_aa_sizes[] = {\n")
        for wi, size, asc, desc, blen, tag in tables:
            f.write("    { %d, %d, %d, %d, aa_parts_%s, %d,\n"
                    "      aa_ascii_%s, aa_wide_%s },\n"
                    % (wi, size, asc, desc, tag, blen, tag, tag))
        f.write("};\n\n")
        f.write("const int prism_aa_sizes_count = %d;\n" % len(tables))
    blob_files.close()
    print("blob parts written to %d files" % blob_files.n)
    with open(OUT_H, "w") as f:
        f.write("/* PrismOS - anti-aliased font tables.\n")
        f.write(" *\n")
        f.write(" * Generated by scripts/gen-aafont.py from Noto\n")
        f.write(" * Sans CJK (SIL Open Font License 1.1; see\n")
        f.write(" * assets/NotoSansCJK-LICENSE.txt).\n")
        f.write(" * Do not edit by hand; regenerate instead. */\n")
        f.write("#ifndef PRISM_AAFONT_TABLES_H\n")
        f.write("#define PRISM_AAFONT_TABLES_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#define PRISM_AA_COUNT %d\n\n" % len(cps))
        f.write("/* Glyph blobs are split into consecutive parts\n")
        f.write(" * of PRISM_AA_PART_LEN bytes (in the generated\n")
        f.write(" * aafont_blob*.c files); byte i of a blob is\n")
        f.write(" * parts[i >> PRISM_AA_PART_SHIFT]\n")
        f.write(" *      [i & (PRISM_AA_PART_LEN - 1)]. */\n")
        f.write("#define PRISM_AA_PART_LEN %d\n" % PART_LEN)
        f.write("#define PRISM_AA_PART_SHIFT %d\n\n"
                % (PART_LEN.bit_length() - 1))
        f.write("extern const uint16_t prism_aa_cps[PRISM_AA_COUNT];\n")
        f.write("extern const prism_aa_size prism_aa_sizes[];\n")
        f.write("extern const int prism_aa_sizes_count;\n\n")
        f.write("#endif /* PRISM_AAFONT_TABLES_H */\n")
    print("wrote %s + %s" % (OUT_C, OUT_H))


if __name__ == "__main__":
    main()
