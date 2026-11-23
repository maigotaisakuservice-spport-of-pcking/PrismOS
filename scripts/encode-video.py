#!/usr/bin/env python3
"""PrismOS 動画エンコーダ: PV1 形式のフレームファイルを作る。

PV1 は PrismOS の自前動画形式 (1 ファイル = 1 フレーム)。
ファイルの並びは 1 本の動画で、ヘッダのあとに DEFLATE 圧縮
したフレームデータを置く。VFS の 1 ファイル 16384 バイトの
上限に収まる。フレームデータのあとには任意で音声トラック
(8 ビット符号なし PCM、8000 Hz、1 フレーム分 = 2000
サンプル = 0.25 秒) が付く。音声のない古い PV1 ファイルも
そのまま読める。

レイアウト (すべてリトルエンディアン):
    0  magic "PRV1" (4 バイト)
    4  u16 width
    6  u16 height
    8  u8  type: 0 = キーフレーム (全画素), 1 = 差分フレーム
    9  u8  予約 (0)
   10  u32 raw_len  (圧縮前のデータのバイト数)
   14  u32 comp_len (圧縮後のデータのバイト数)
   18  comp_len バイトの raw DEFLATE ストリーム
   18 + comp_len  以降 (任意): 音声トラックの生 PCM

キーフレームのデータ: width*height 個の RGB565 (LE) 画素。
差分フレームのデータ: { u16 skip, u16 count, count 個の
RGB565 画素 } の繰り返しで、skip == 0xFFFF で終わり。
前のフレームの画素列に、skip だけ飛ばして count 個だけ
上書きする。キーフレームは 8 フレームごと。

使い方:
    python3 scripts/encode-video.py --sample --out build/pv1 \
        --header shell/sample_video_pv1.h
        サンプル動画 (320x240、24 フレーム、音声つき) を作る。
    python3 scripts/encode-video.py --bmp frame1.bmp frame2.bmp \
        --out build/pv1 --width 320 --height 240
        BMP (24 ビット) の連番を PV1 に変換する。

色の圧縮は RGB565 (1 画素 2 バイト) に落とすことと、
フレーム間差分と DEFLATE の 3 段で行う。浮動小数点は
カーネルでは使わない (このホスト側エンコーダで正弦波の
音声を作るときだけ使う)。
"""

import math
import os
import struct
import sys
import zlib

MAGIC = b"PRV1"
HEADER_LEN = 18
KEYFRAME_INTERVAL = 8
MAX_FILE_SIZE = 16384

# 音声トラック: 8 ビット符号なし PCM、8000 Hz、1 フレーム
# (4 fps) あたり 2000 サンプル。音は C5 (523.25 Hz) の
# 正弦波で、メロディの最初の音と同じ高さ。
AUDIO_RATE = 8000
AUDIO_SAMPLES_PER_FRAME = AUDIO_RATE // 4
AUDIO_FREQ_HZ = 523.25
AUDIO_AMPLITUDE = 90

# サンプルの色 (RGB565 に落とす)。
BG = (11, 16, 38)       # #0B1026 背景
BALL = (76, 194, 255)    # #4CC2FF ボール
BAND = (124, 92, 255)    # #7C5CFF 帯


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def deflate_raw(data):
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    return comp.compress(data) + comp.flush()


def build_frame(width, height, ftype, raw, audio=b""):
    comp = deflate_raw(raw)
    hdr = MAGIC + struct.pack("<HHBBII", width, height, ftype, 0,
                              len(raw), len(comp))
    assert len(hdr) == HEADER_LEN
    if len(audio) > 4096:
        raise SystemExit(
            "audio too long for one PV1 frame: %d samples (limit %d)"
            % (len(audio), 4096))
    out = hdr + comp + audio
    if len(out) > MAX_FILE_SIZE:
        raise SystemExit(
            "frame too big for one VFS file: %d bytes (limit %d)"
            % (len(out), MAX_FILE_SIZE))
    return out


def encode_keyframe(pixels, width, height, audio=b""):
    raw = b"".join(struct.pack("<H", p) for p in pixels)
    return build_frame(width, height, 0, raw, audio)


def encode_diff(prev, cur, width, height, audio=b""):
    ops = bytearray()
    n = width * height
    pos = 0  # 受け手が今いる画素の位置
    i = 0
    while i < n:
        if prev[i] == cur[i]:
            i += 1
            continue
        start = i
        while i < n and prev[i] != cur[i]:
            i += 1
        # 変わった区間 [start, i) を塊に分けて出す。
        # 1 つの塊は count が u16 なので 65535 画素まで。
        skip = start - pos
        j = start
        while j < i:
            take = min(i - j, 65535)
            ops += struct.pack("<HH", skip, take)
            skip = 0
            for k in range(j, j + take):
                ops += struct.pack("<H", cur[k])
            j += take
        pos = i
    ops += struct.pack("<H", 0xFFFF)
    return build_frame(width, height, 1, bytes(ops), audio)


def sample_audio():
    """1 フレーム分の音声: C5 の正弦波 2000 サンプル。
    8 ビット符号なし PCM (128 が無音)。"""
    out = bytearray()
    for i in range(AUDIO_SAMPLES_PER_FRAME):
        v = int(AUDIO_AMPLITUDE *
                math.sin(2.0 * math.pi * AUDIO_FREQ_HZ * i
                         / AUDIO_RATE))
        out.append(128 + v)
    return bytes(out)


def encode_frames(frames, width, height, audio_per_frame=None):
    """frames: 画素列 (RGB565 の int のリスト) の一覧。
    audio_per_frame: フレームごとの音声 (bytes) の一覧、
    または None (音声なし)。
    戻り値は PV1 ファイル内容のバイト列の一覧。"""
    out = []
    prev = None
    for idx, cur in enumerate(frames):
        audio = (audio_per_frame[idx] if audio_per_frame is not None
                 else b"")
        if idx % KEYFRAME_INTERVAL == 0 or prev is None:
            out.append(encode_keyframe(cur, width, height, audio))
        else:
            out.append(encode_diff(prev, cur, width, height, audio))
        prev = cur
    return out


def sample_ball_pos(width, height, idx):
    """サンプル動画のボールの中心。壁に当たると跳ね返る
    (24 フレームでも画面の中に残る)。整数だけの計算。
    320x240 の場合: 半径 20、(40, 200) から (14, -12)
    ずつ動く。160x120 の場合: 半径 10、(20, 100) から
    (7, -6) ずつ動く (昔の位置とちょうど半分)。"""
    r = max(5, width // 16)
    cx = width // 8
    cy = height * 100 // 120
    vx = width * 7 // 160
    vy = -(height * 6 // 120)
    for _ in range(idx):
        cx += vx
        cy += vy
        if cx - r < 0:
            cx = r
            vx = -vx
        elif cx + r >= width:
            cx = width - 1 - r
            vx = -vx
        if cy - r < 0:
            cy = r
            vy = -vy
        elif cy + r >= height:
            cy = height - 1 - r
            vy = -vy
    return cx, cy, r


def sample_frame(width, height, idx):
    """サンプル動画の 1 フレームを描く。ボールが跳ね、帯が
    横に流れる。整数だけの計算 (浮動小数点なし)。"""
    px = [rgb565(*BG)] * (width * height)
    # 帯: 高さ height//15、幅 width の 60/160、右へ
    # 13*width//160 画素ずつ流れ、右端で回る。
    band_x = (idx * 13 * width // 160) % width
    band_y = height // 6
    band_h = max(4, height // 15)
    band_w = width * 60 // 160
    band_c = rgb565(*BAND)
    for y in range(band_y, band_y + band_h):
        for x in range(band_w):
            xx = (band_x + x) % width
            px[y * width + xx] = band_c
    # ボール: 壁で跳ね返る。
    cx, cy, r = sample_ball_pos(width, height, idx)
    ball_c = rgb565(*BALL)
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            dx = x - cx
            dy = y - cy
            if dx * dx + dy * dy <= r * r:
                if 0 <= x < width and 0 <= y < height:
                    px[y * width + x] = ball_c
    return px


def read_bmp_24(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 54 or data[0:2] != b"BM":
        raise SystemExit("not a BMP file: %s" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise SystemExit("only 24-bit BMP is supported: %s" % path)
    if h < 0:
        h = -h
        top_down = True
    else:
        top_down = False
    stride = ((w * 3 + 3) // 4) * 4
    rows = []
    for y in range(h):
        sy = y if top_down else (h - 1 - y)
        row = data[off + sy * stride: off + sy * stride + w * 3]
        rows.append(row)
    return w, h, rows


def scale_bmp_to_rgb565(w, h, rows, nw, nh):
    px = []
    for y in range(nh):
        sy = y * h // nh
        row = rows[sy]
        for x in range(nw):
            sx = x * w // nw
            b, g, r = row[sx * 3: sx * 3 + 3]
            px.append(rgb565(r, g, b))
    return px


def write_header(frames, path):
    lines = []
    lines.append("/* Generated by scripts/encode-video.py. "
                 "Do not edit by hand; regenerate. */")
    lines.append("#ifndef PRISM_SAMPLE_VIDEO_PV1_H")
    lines.append("#define PRISM_SAMPLE_VIDEO_PV1_H")
    lines.append("")
    lines.append("#include <stddef.h>")
    lines.append("#include <stdint.h>")
    lines.append("")
    for i, data in enumerate(frames, 1):
        lines.append("static const uint8_t prism_sample_vid_pv1_%02d[] = {"
                     % i)
        for j in range(0, len(data), 16):
            chunk = data[j:j + 16]
            lines.append("    " + ", ".join("0x%02x" % b for b in chunk)
                         + ",")
        lines.append("};")
        lines.append("static const size_t prism_sample_vid_pv1_%02d_len = "
                     "%d;" % (i, len(data)))
        lines.append("")
    lines.append("#endif /* PRISM_SAMPLE_VIDEO_PV1_H */")
    lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))


def main(argv):
    width = 320
    height = 240
    frames_n = 12
    out_dir = None
    header_path = None
    bmp_paths = []
    sample = False
    no_audio = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--sample":
            sample = True
        elif a == "--no-audio":
            no_audio = True
        elif a == "--frames" and i + 1 < len(argv):
            i += 1
            frames_n = int(argv[i])
        elif a == "--out" and i + 1 < len(argv):
            i += 1
            out_dir = argv[i]
        elif a == "--header" and i + 1 < len(argv):
            i += 1
            header_path = argv[i]
        elif a == "--width" and i + 1 < len(argv):
            i += 1
            width = int(argv[i])
        elif a == "--height" and i + 1 < len(argv):
            i += 1
            height = int(argv[i])
        elif a.startswith("--"):
            raise SystemExit("unknown option: %s" % a)
        else:
            bmp_paths.append(a)
        i += 1
    if sample and bmp_paths:
        raise SystemExit("--sample and BMP inputs cannot be combined")
    if not sample and not bmp_paths:
        raise SystemExit("give --sample or BMP files")
    if width <= 0 or height <= 0 or width > 320 or height > 240:
        raise SystemExit("bad size: %dx%d" % (width, height))
    if frames_n <= 0 or frames_n > 32:
        raise SystemExit("bad frame count: %d" % frames_n)
    if sample:
        frames = [sample_frame(width, height, k)
                  for k in range(frames_n)]
        audio = None if no_audio else [sample_audio()
                                        for _ in range(frames_n)]
    else:
        frames = []
        for path in bmp_paths:
            w, h, rows = read_bmp_24(path)
            frames.append(scale_bmp_to_rgb565(w, h, rows, width, height))
        audio = None
    encoded = encode_frames(frames, width, height, audio)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        for k, data in enumerate(encoded, 1):
            name = "vid-%02d.pv1" % k
            with open(os.path.join(out_dir, name), "wb") as f:
                f.write(data)
            print("%s: %d bytes" % (name, len(data)))
    if header_path:
        write_header(encoded, header_path)
        print("header: %s (%d frames)" % (header_path, len(encoded)))
    total = sum(len(d) for d in encoded)
    print("total: %d bytes in %d frames (%dx%d)"
          % (total, len(encoded), width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
