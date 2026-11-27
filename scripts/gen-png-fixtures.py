#!/usr/bin/env python3
"""PNG デコーダのホストテスト用フィクスチャを生成する。

tests/png_fixtures.h に、既知のピクセルを持つ小さな PNG の
バイト列を C 配列として書き出す。フィルタは行ごとに違う
種類を使い、復元の全経路を通す。

使い方: python3 scripts/gen-png-fixtures.py
"""
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "png_fixtures.h")


def chunk(typ, data):
    c = struct.pack(">I", len(data)) + typ + data
    c += struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
    return c


def ihdr(w, h, depth, ctype):
    return chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth,
                                      ctype, 0, 0, 0))


def png(w, h, depth, ctype, raw_scanlines, plte=None):
    out = b"\x89PNG\r\n\x1a\n"
    out += ihdr(w, h, depth, ctype)
    if plte is not None:
        out += chunk(b"PLTE", plte)
    out += chunk(b"IDAT", zlib.compress(raw_scanlines))
    out += chunk(b"IEND", b"")
    return out


def filt(ftype, row, prev, bpp):
    """row (bytes) を ftype でフィルタしたバイト列を返す。"""
    out = bytearray()
    for i, v in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i] if prev is not None else 0
        c = prev[i - bpp] if (prev is not None and i >= bpp) else 0
        if ftype == 0:
            pred = 0
        elif ftype == 1:
            pred = a
        elif ftype == 2:
            pred = b
        elif ftype == 3:
            pred = (a + b) // 2
        else:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if (pa <= pb and pa <= pc) else (
                b if pb <= pc else c)
        out.append((v - pred) & 0xFF)
    return bytes([ftype]) + bytes(out)


def main():
    fixtures = []

    # 1. RGB 8bit 4x2, フィルタ 0/1
    rows = [
        bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0]),
        bytes([0, 255, 255, 255, 0, 255, 128, 128, 128, 0, 0, 0]),
    ]
    raw = filt(0, rows[0], None, 3) + filt(1, rows[1], rows[0], 3)
    fixtures.append(("png_rgb8", png(4, 2, 8, 2, raw)))

    # 2. RGBA 8bit 2x2, フィルタ 2/4 (α合成の確認用)
    rows = [
        bytes([255, 0, 0, 255, 0, 255, 0, 128]),
        bytes([0, 0, 255, 0, 10, 20, 30, 255]),
    ]
    raw = filt(2, rows[0], None, 4) + filt(4, rows[1], rows[0], 4)
    fixtures.append(("png_rgba8", png(2, 2, 8, 6, raw)))

    # 3. パレット 8bit 2x2
    plte = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255])
    raw = filt(0, bytes([0, 1]), None, 1) + filt(3, bytes([2, 3]),
                                                  bytes([0, 1]), 1)
    fixtures.append(("png_pal8", png(2, 2, 8, 3, raw, plte)))

    # 4. グレー 8bit 3x1
    raw = filt(0, bytes([0, 127, 255]), None, 1)
    fixtures.append(("png_gray8", png(3, 1, 8, 0, raw)))

    # 5. RGB 16bit 2x1 (上位バイトが値)
    raw = filt(0, bytes([0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
                         0xDE, 0xF0, 0x00, 0x11, 0x22, 0x33]),
               None, 6)
    fixtures.append(("png_rgb16", png(2, 1, 16, 2, raw)))

    # 6. パレット 2bit 4x1 (インデックス 0,1,2,3)
    packed = (0 << 6) | (1 << 4) | (2 << 2) | 3
    raw = filt(0, bytes([packed]), None, 1)
    fixtures.append(("png_pal2", png(4, 1, 2, 3, raw, plte)))

    # 7. RGB 8bit 8x8 単色グラデーション (縮小テスト用):
    #    ピクセル (x,y) = (x*32, y*32, 200)
    raw = b""
    prev = None
    for y in range(8):
        row = bytearray()
        for x in range(8):
            row += bytes([x * 32, y * 32, 200])
        raw += filt(0, bytes(row), prev, 3)
        prev = bytes(row)
    fixtures.append(("png_big8", png(8, 8, 8, 2, raw)))

    with open(OUT, "w") as f:
        f.write("/* 生成物: scripts/gen-png-fixtures.py が作る。"
                "手で編集しないこと。 */\n")
        f.write("#ifndef PRISM_PNG_FIXTURES_H\n")
        f.write("#define PRISM_PNG_FIXTURES_H\n\n")
        f.write("#include <stddef.h>\n#include <stdint.h>\n\n")
        for name, data in fixtures:
            f.write("static const uint8_t %s[] = {" % name)
            for i, b in enumerate(data):
                if i % 12 == 0:
                    f.write("\n    ")
                f.write("0x%02X, " % b)
            f.write("\n};\n")
            f.write("static const size_t %s_len = %d;\n\n"
                    % (name, len(data)))
        f.write("#endif\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
