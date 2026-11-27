#!/usr/bin/env python3
"""PrismOS 公式ページ用の実スクリーンショット撮影。

現行ビルドの ISO を QEMU で起動し、モニタの screendump で
実際の画面を PPM で撮って docs/images/ に PNG で保存する。
撮影対象: 起動アニメーション、デスクトップ、ターミナル
(ver で 27H1 表示)、Files、設定。

Usage: python3 scripts/capture-screenshots.py
  (exit code 0 = 全枚撮影できた; 標準ライブラリ + PIL)
"""

import os
import pty
import select
import socket
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-shots.sock")
OUT_DIR = os.path.join(ROOT, "docs", "images")
TMP_DIR = os.path.join(ROOT, "build", "shots-official")
LOG = os.path.join(ROOT, "build", "capture-screenshots.log")
QEMU = "qemu-system-x86_64"


class ShotFailure(Exception):
    pass


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(TMP_DIR, exist_ok=True)
    try:
        os.unlink(MONITOR_SOCK)
    except OSError:
        pass

    qemu_args = [
        QEMU, "-cdrom", ISO, "-m", "256M",
        "-serial", "stdio",
        "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
        "-display", "vnc=:7",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        "-device", "sb16,audiodev=snd0",
        "-audiodev", "none,id=snd0",
        "-no-reboot",
    ]
    log_file = open(LOG, "wb")
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    proc = subprocess.Popen(qemu_args, stdin=slave_fd, stdout=slave_fd,
                            stderr=subprocess.DEVNULL, cwd=ROOT,
                            close_fds=True)
    os.close(slave_fd)
    buf = bytearray()
    pos = [0]
    monitor_sock = [None]

    def pump(timeout):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            r, _, _ = select.select([master_fd], [], [], remaining)
            if not r:
                return
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            buf.extend(chunk)
            log_file.write(chunk)
            log_file.flush()
            return

    def wait_for(needle, timeout, what):
        needle_b = needle.encode("utf-8", "replace")
        deadline = time.monotonic() + timeout
        while True:
            idx = buf.find(needle_b, pos[0])
            if idx >= 0:
                pos[0] = idx + len(needle_b)
                return
            if proc.poll() is not None:
                raise ShotFailure("QEMU exited early (code %s)"
                                  % proc.returncode)
            if time.monotonic() > deadline:
                raise ShotFailure("%s not seen in %s s" % (what, timeout))
            pump(min(0.5, deadline - time.monotonic()))

    def hmp(cmd):
        monitor_sock[0].sendall((cmd + "\n").encode("ascii"))
        data = b""
        while b"(qemu)" not in data:
            chunk = monitor_sock[0].recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def connect_monitor():
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(MONITOR_SOCK)
        monitor_sock[0] = s
        hmp("")

    def screendump(name):
        ppm = os.path.join(TMP_DIR, name + ".ppm")
        hmp("screendump %s" % ppm)
        time.sleep(0.6)
        from PIL import Image
        img = Image.open(ppm)
        img.load()
        w, h = img.size
        if w > 960:
            img = img.resize((960, h * 960 // w), Image.LANCZOS)
        dest = os.path.join(OUT_DIR, name + ".png")
        img.save(dest)
        print("shot: %s (%dx%d -> %s, %d bytes)"
              % (name, w, h, dest, os.path.getsize(dest)))
        return dest

    def sendkey(key, delay=0.25):
        hmp("sendkey %s" % key)
        time.sleep(delay)

    def type_text(text):
        keymap = {".": "dot", "/": "slash", ":": "shift-semicolon"}
        for ch in text:
            if ch in keymap:
                sendkey(keymap[ch], 0.15)
            elif ch == " ":
                sendkey("spc", 0.15)
            elif ch == "\n":
                sendkey("ret", 0.3)
            elif ch.isupper():
                sendkey("shift-" + ch.lower(), 0.15)
            else:
                sendkey(ch, 0.15)

    try:
        # 起動アニメーション (フレーム表示中に撮る)。
        wait_for("[boot] anim frame 8", 60, "boot animation")
        connect_monitor()
        try:
            screendump("shot-boot")
        except Exception as e:
            print("warn: boot shot failed (%s); continuing" % e)

        # バナー ("PrismOS 27H1") はアニメーションより前に出る
        # ため、ここでは探さない。
        wait_for("desktop: ready", 60, "desktop ready")
        wait_for("prismos> ", 30, "shell prompt")
        time.sleep(2.0)
        screendump("shot-desktop")

        # ターミナルで ver (27H1 が画面に出る)。
        type_text("ver\n")
        wait_for("PrismOS version 27H1", 30, "ver output")
        time.sleep(0.6)
        screendump("shot-terminal")

        # Files (F2)。
        sendkey("f2")
        time.sleep(1.0)
        screendump("shot-files")
        sendkey("esc")
        time.sleep(0.6)

        # 設定 (F3)。
        sendkey("f3")
        time.sleep(1.0)
        screendump("shot-settings")
        print("SCREENSHOTS DONE")
        return 0
    except ShotFailure as e:
        print("FAILED: %s" % e)
        return 1
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        os.close(master_fd)
        log_file.close()


if __name__ == "__main__":
    sys.exit(main())
