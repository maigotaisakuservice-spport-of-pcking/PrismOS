#!/usr/bin/env python3
"""PrismOS 32bit 版のディスク永続化テスト。

qemu-system-i386 で 32bit ISO を起動し、IDE ディスクを
付けて 2 回起動する。1 回目: 未フォーマットを確認し、
ファイルを作って dsync する。2 回目 (同じディスク):
起動時にファイルが自動で読み込まれ、cat で中身が
見えること (diskfs が 32bit でも動く証拠)。

使い方: python3 scripts/disk32-test.py
成功したら "DISK32 TEST PASSED" と出す。
"""
import os
import select
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos-32.iso")
DISK = os.path.join(ROOT, "build", "disk32-test.img")
LOG = os.path.join(ROOT, "build", "disk32.log")

FAILURES = []


def check(label, ok):
    print(("ok: " if ok else "FAIL: ") + label)
    if not ok:
        FAILURES.append(label)


class Session:
    def __init__(self):
        self.buf = b""
        self.proc = subprocess.Popen(
            [
                "qemu-system-i386",
                "-cdrom", ISO,
                "-boot", "order=d",
                "-m", "256M",
                "-drive", f"file={DISK},if=ide,format=raw",
                "-serial", "stdio",
                "-display", "none",
                "-no-reboot",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    def read_until(self, needle, timeout=30.0):
        needle_b = needle.encode()
        deadline = time.time() + timeout
        while needle_b not in self.buf:
            if self.proc.poll() is not None:
                return False
            remain = deadline - time.time()
            if remain <= 0:
                return False
            r, _, _ = select.select([self.proc.stdout], [], [], remain)
            if not r:
                return False
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                return False
            self.buf += chunk
        idx = self.buf.index(needle_b) + len(needle_b)
        self.buf = self.buf[idx:]
        return True

    def send(self, line):
        self.proc.stdin.write(line.encode() + b"\n")
        self.proc.stdin.flush()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


def main():
    if not os.path.exists(ISO):
        print("prismos-32.iso がありません。make iso32 を先に。")
        return 1
    with open(DISK, "wb") as f:
        f.truncate(16 * 1024 * 1024)

    log = open(LOG, "wb")

    # ---- 1 回目の起動 ----
    sess = Session()
    try:
        ok = sess.read_until("PrismOS 27H1 (32-bit)", 60.0)
        check("boot banner (32-bit)", ok)
        ok = sess.read_until(
            "[boot] disk: not formatted (dsync formats it)", 30.0)
        check("disk detected, not formatted", ok)
        ok = sess.read_until("prismos> ", 30.0)
        check("shell prompt (boot 1)", ok)
        if ok:
            sess.send("echo persist-32bit > p.txt")
            ok = sess.read_until("wrote 14 bytes to p.txt", 20.0)
            check("create file in VFS", ok)
            sess.send("dsync")
            ok = sess.read_until("file(s) written to disk", 30.0)
            check("dsync writes files (formats first)", ok)
            sess.send("dls")
            ok = sess.read_until("p.txt", 20.0)
            check("dls lists p.txt", ok)
        log.write(sess.buf)
    finally:
        sess.stop()

    # ---- 2 回目の起動 (同じディスク) ----
    sess = Session()
    try:
        ok = sess.read_until("file(s) loaded from disk", 60.0)
        check("files auto-loaded at boot (boot 2)", ok)
        ok = sess.read_until("prismos> ", 30.0)
        check("shell prompt (boot 2)", ok)
        if ok:
            sess.send("cat p.txt")
            ok = sess.read_until("persist-32bit", 20.0)
            check("file persisted across reboot", ok)
        log.write(sess.buf)
    finally:
        sess.stop()
    log.close()

    if FAILURES:
        print("DISK32 TEST FAILED:", ", ".join(FAILURES))
        print("serial log:", LOG)
        return 1
    print("DISK32 TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
