#!/usr/bin/env python3
"""PrismOS Live ブート (USB) 版のテスト。

build/prismos-live-usb.img (make live-usb で作る。ハイブリッド
ISO と同じ中身で、USB メモリへそのまま書き込めるイメージ) を
QEMU に USB ストレージとして付けて起動し、確認する:
  1. CD を付けずに USB から Limine が起動し、カーネルのバナーと
     シェルプロンプトが出る
  2. シェルコマンド (ver / help / echo > / cat) が動く
  3. 画面 (VNC + screendump) にデスクトップ (壁紙とタスクバー)
     が描かれている

チェックリスト項目9で確認した方式 (ISO を USB ストレージとして
起動) を、正式なビルド成果物に対して行うテスト。

使い方: python3 scripts/live-usb-test.py
  (終了コード 0 = 合格。標準ライブラリのみ)
"""

import os
import pty
import select
import signal
import socket
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
IMG = os.path.join(BUILD, "prismos-live-usb.img")
ISO = os.path.join(BUILD, "prismos.iso")
LOG = os.path.join(BUILD, "live-usb.log")
DEBUG_LOG = os.path.join(BUILD, "qemu-debug-live-usb.log")
STDERR_LOG = os.path.join(BUILD, "qemu-stderr-live-usb.log")
MONITOR_SOCK = os.path.join(BUILD, "qemu-monitor-live-usb.sock")
SHOT = os.path.join(BUILD, "shots", "live-usb-desktop.ppm")

QEMU = "qemu-system-x86_64"


class LiveUsbTestFailure(Exception):
    pass


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", "not a P6 ppm: %s" % path
    parts = data.split(b"\n", 3)
    dims = parts[1].split()
    return int(dims[0]), int(dims[1]), parts[3]


class Boot:
    def __init__(self):
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(LOG, "wb")
        self.stderr_file = open(STDERR_LOG, "wb")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        try:
            os.unlink(MONITOR_SOCK)
        except OSError:
            pass
        args = [QEMU,
                "-usb",
                "-drive", "file=%s,format=raw,if=none,id=usb" % IMG,
                "-device", "usb-storage,drive=usb",
                "-boot", "order=c",
                "-m", "256M", "-serial", "stdio",
                "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
                "-display", "vnc=:12",
                "-no-reboot",
                "-d", "cpu_reset,int", "-D", DEBUG_LOG]
        self.proc = subprocess.Popen(args, stdin=slave_fd, stdout=slave_fd,
                                     stderr=self.stderr_file, cwd=ROOT,
                                     close_fds=True)
        os.close(slave_fd)
        self.monitor_sock = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5)
                s.connect(MONITOR_SOCK)
                self.monitor_sock = s
                break
            except OSError:
                time.sleep(0.2)
        if self.monitor_sock is None:
            raise LiveUsbTestFailure("monitor socket did not appear")
        self.hmp("")

    def pump(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            r, _, _ = select.select([self.master_fd], [], [], remaining)
            if not r:
                return
            try:
                chunk = os.read(self.master_fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            self.buf.extend(chunk)
            self.log_file.write(chunk)
            self.log_file.flush()
            return

    def wait_for(self, needle, timeout, what):
        needle_b = needle.encode("utf-8", "replace")
        deadline = time.monotonic() + timeout
        while True:
            idx = self.buf.find(needle_b, self.pos)
            if idx >= 0:
                self.pos = idx + len(needle_b)
                return
            if self.proc.poll() is not None:
                raise LiveUsbTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise LiveUsbTestFailure(
                    "%s not seen in %s s (expected %r)" % (what, timeout,
                                                           needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def send(self, text):
        os.write(self.master_fd, (text + "\r").encode("ascii"))

    def hmp(self, cmd):
        self.monitor_sock.sendall((cmd + "\n").encode("ascii"))
        data = b""
        while b"(qemu)" not in data:
            chunk = self.monitor_sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def screendump(self):
        os.makedirs(os.path.dirname(SHOT), exist_ok=True)
        self.hmp("screendump %s" % SHOT)
        time.sleep(0.5)
        return read_ppm(SHOT)

    def stop(self):
        if self.monitor_sock is not None:
            try:
                self.monitor_sock.close()
            except OSError:
                pass
            try:
                os.unlink(MONITOR_SOCK)
            except OSError:
                pass
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        try:
            os.close(self.master_fd)
        except OSError:
            pass
        self.stderr_file.close()
        self.log_file.close()


def main():
    if not os.path.exists(IMG):
        print("MISSING: Live USB イメージがありません: %s "
              "(先に make live-usb してください)" % IMG)
        return 1
    # 成果物はハイブリッド ISO と同じ中身 (USB へそのまま書ける)。
    if os.path.getsize(IMG) != os.path.getsize(ISO):
        print("MISSING: Live USB イメージの大きさが ISO と違います")
        return 1
    print("ok: live USB image present (%d bytes)" % os.path.getsize(IMG))

    try:
        b = Boot()
        try:
            # USB からの読み込みは BIOS の USB エミュレーション経由で
            # 遅い (実測でバナーまで 30 秒を超える)。待ち時間は
            # CD 起動より長く取る。
            b.wait_for("PrismOS 27H1", 120, "boot banner from USB")
            print("ok: boot banner (from USB storage)")
            b.wait_for("prismos> ", 60, "shell prompt from USB")
            print("ok: shell prompt (from USB storage)")
            for cmd, needle, label in [
                ("ver", "PrismOS version 27H1", "ver output"),
                ("help", "PrismOS shell", "help output"),
                ("echo live usb works > usb.txt",
                 "wrote 15 bytes to usb.txt", "echo write"),
                ("cat usb.txt", "live usb works", "cat created file"),
            ]:
                b.send(cmd)
                b.wait_for(needle, 20, label)
                print("ok: %s" % label)
                b.wait_for("prismos> ", 20, "shell prompt after %s" % cmd)
            # 画面: デスクトップが描かれていること。
            w, h, raw = b.screendump()
            if (w, h) != (1280, 800):
                raise LiveUsbTestFailure(
                    "unexpected screen size %dx%d" % (w, h))
            # 壁紙領域 (中央) に黒でない画素があること。
            colored = 0
            for y in range(200, 600, 7):
                for x in range(200, 1080, 7):
                    i = (y * w + x) * 3
                    if raw[i] + raw[i + 1] + raw[i + 2] > 60:
                        colored += 1
            if colored < 100:
                raise LiveUsbTestFailure(
                    "wallpaper looks blank (colored samples=%d)" % colored)
            print("ok: desktop drawn (wallpaper samples=%d)" % colored)
            # タスクバー帯 (下端) に明るい文字画素があること。
            white = 0
            for y in range(h - 46, h - 4):
                for x in range(0, w):
                    i = (y * w + x) * 3
                    if raw[i] > 180 and raw[i + 1] > 180 and raw[i + 2] > 180:
                        white += 1
            if white < 50:
                raise LiveUsbTestFailure(
                    "taskbar text not visible (white=%d)" % white)
            print("ok: taskbar drawn (white pixels=%d)" % white)
        finally:
            b.stop()
        print("LIVE USB TEST PASSED")
        return 0
    except LiveUsbTestFailure as e:
        print("MISSING: %s" % e)
        print("--- boot log (%s) ---" % LOG)
        try:
            with open(LOG, "rb") as f:
                sys.stdout.write(f.read().decode("utf-8", "replace"))
        except OSError:
            pass
        print("LIVE USB TEST FAILED")
        return 1


if __name__ == "__main__":
    sys.exit(main())
