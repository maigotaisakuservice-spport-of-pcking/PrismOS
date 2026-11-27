#!/usr/bin/env python3
"""PrismOS の Mach-O 実証ゲスト (hello.macho) を作る。

compat/machoguest.s を as/ld で組み立て (入口 0x100000000
に固定)、.text の中身を取り出して、Mach-O 64 ヘッダを
Python で組み立てた実行ファイルにする。生成物は
shell/sample_macho.h に C のバイト列として書き出す
(生成物はコミットする。手で編集しないこと)。

Mach-O の配置:
  ヘッダ 32 バイト + LC_SEGMENT_64 (72) + LC_MAIN (24)
  = 128 バイトの後に __TEXT セグメントの __text を置く。
  dyld は使わない (LC_LOAD_DYLINKER なし) ため、ローダー
  がそのまま載せて走らせる。

使い方: python3 scripts/build-macho-guest.py
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "compat", "machoguest.s")
BUILD_DIR = os.path.join(ROOT, "build", "compat")
HEADER = os.path.join(ROOT, "shell", "sample_macho.h")
TEXT_VA = 0x100000000

MH_MAGIC_64 = 0xFEEDFACF
CPU_X86_64 = 0x01000007
CPU_SUBTYPE_X86_64_ALL = 3
FILE_EXECUTE = 2
LC_SEGMENT_64 = 0x19
LC_MAIN = 0x80000028


def align(v, a):
    return (v + a - 1) // a * a


def c_array(name, data):
    lines = ["static const uint8_t %s[] = {" % name]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk)
                     + ",")
    lines.append("};")
    lines.append("static const size_t %s_len = %d;" % (name, len(data)))
    return "\n".join(lines) + "\n"


def build():
    os.makedirs(BUILD_DIR, exist_ok=True)
    obj = os.path.join(BUILD_DIR, "machoguest.o")
    elf = os.path.join(BUILD_DIR, "machoguest.elf")
    lds = os.path.join(BUILD_DIR, "machoguest.ld")
    with open(lds, "w") as f:
        f.write("SECTIONS {\n"
                "  . = 0x100000000;\n"
                "  .text : { *(.text) }\n"
                "  .rodata : { *(.rodata) }\n"
                "}\n")
    for cmd in (
        ["as", "--64", "-o", obj, SRC],
        ["ld", "-o", elf, "-T", lds, "-e", "_start", "-s", obj],
    ):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAILED: %s" % " ".join(cmd))
            print(r.stdout)
            print(r.stderr, file=sys.stderr)
            return 1
    text_bin = os.path.join(BUILD_DIR, "machoguest.text.bin")
    rodata_bin = os.path.join(BUILD_DIR, "machoguest.rodata.bin")
    for sec, out in ((".text", text_bin), (".rodata", rodata_bin)):
        r = subprocess.run(
            ["objcopy", "-O", "binary", "--only-section=%s" % sec,
             elf, out],
            capture_output=True, text=True)
        if r.returncode != 0:
            print("FAILED: objcopy %s" % sec)
            print(r.stderr, file=sys.stderr)
            return 1
    with open(text_bin, "rb") as f:
        text = f.read()
    with open(rodata_bin, "rb") as f:
        rodata = f.read()
    # __TEXT セグメントの中身: .text の直後に .rodata を置く。
    seg_data = text + rodata
    header_size = 32 + 72 + 24
    fileoff = header_size
    vmsize = align(len(seg_data), 0x1000)
    # ヘッダ
    hdr = bytearray()
    hdr += struct.pack("<IIIIIIII",
                       MH_MAGIC_64, CPU_X86_64,
                       CPU_SUBTYPE_X86_64_ALL, FILE_EXECUTE,
                       2, 72 + 24, 0, 0)
    # LC_SEGMENT_64 (__TEXT)
    segname = b"__TEXT" + b"\x00" * 10
    hdr += struct.pack("<II", LC_SEGMENT_64, 72)
    hdr += segname
    hdr += struct.pack("<QQQQiiII",
                       TEXT_VA, vmsize, fileoff, len(seg_data),
                       7, 5, 0, 0)
    # LC_MAIN (entryoff=0: セグメント先頭が入口)
    hdr += struct.pack("<IIQQ", LC_MAIN, 24, 0, 0)
    assert len(hdr) == header_size
    macho = bytes(hdr) + seg_data
    with open(HEADER, "w") as f:
        f.write("/* PrismOS - Mach-O 実証ゲスト (hello.macho)。\n"
                " * scripts/build-macho-guest.py の生成物。手で編集しないこと。\n"
                " */\n")
        f.write(c_array("compat_guest_hello_macho", macho))
    print("wrote %s (%d bytes)" % (HEADER, len(macho)))
    return 0


if __name__ == "__main__":
    sys.exit(build())
