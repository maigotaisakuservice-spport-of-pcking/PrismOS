#!/usr/bin/env python3
"""Generate the PDF fixtures used by tests/test_pdf.c."""
import os
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def make_pdf(path, content, flate):
    if flate:
        stream = zlib.compress(content)
        extra = " /Filter /FlateDecode"
    else:
        stream = content
        extra = ""
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> "
        b">> >>",
        ("<< /Length %d%s >>\nstream\n" % (len(stream), extra)
         ).encode() + stream + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = b"%PDF-1.4\n"
    offs = []
    for i, body in enumerate(objs, 1):
        offs.append(len(out))
        out += ("%d 0 obj\n" % i).encode() + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 6\n0000000000 65535 f \n"
    for o in offs:
        out += ("%010d 00000 n \n" % o).encode()
    out += ("trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n"
            "%d\n%%%%EOF\n" % xref).encode()
    with open(path, "wb") as fh:
        fh.write(out)


def main():
    build = os.path.join(ROOT, "build")
    os.makedirs(build, exist_ok=True)
    c1 = (b"BT /F1 12 Tf 56 756 Td (Hello PrismOS) Tj T* "
          b"(Second line) Tj ET")
    make_pdf(os.path.join(build, "fixture-plain.pdf"), c1, False)
    c2 = b"BT /F1 12 Tf 56 756 Td (Compressed greeting) Tj ET"
    make_pdf(os.path.join(build, "fixture-flate.pdf"), c2, True)


if __name__ == "__main__":
    main()
