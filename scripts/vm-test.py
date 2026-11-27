#!/usr/bin/env python3
"""PrismOS 仮想環境版のテスト。

build/prismos-vm.qcow2 (make vm-image で作る。PrismOS が
インストール済みの VM 用ディスクイメージ) を QEMU の
ハードディスクとして起動し、CD なしで確認する:
  1. Limine がディスクからカーネルを起動し、バナーとプロンプト
     が出る
  2. インストール済みシステム (データパーティション) が見つかる
  3. シェルコマンドが動き、書いたファイルが再起動後も残る
  4. 画面 (VNC + screendump) にデスクトップが描かれている

使い方: python3 scripts/vm-test.py
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
IMG = os.path.join(BUILD, "prismos-vm.qcow2")
LOG = os.path.join(BUILD, "vm.log")
DEBUG_LOG = os.path.join(BUILD, "qemu-debug-vm.log")
STDERR_LOG = os.path.join(BUILD, "qemu-stderr-vm.log")
MONITOR_SOCK = os.path.join(BUILD, "qemu-monitor-vm.sock")
SHOT = os.path.join(BUILD, "shots", "vm-desktop.ppm")

QEMU = "qemu-system-x86_64"


class VmTestFailure(Exception):
    pass


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", "not a P6 ppm: %s" % path
    parts = data.split(b"\n", 3)
    dims = parts[1].split()
    return int(dims[0]), int(dims[1]), parts[3]


class Boot:
    def __init__(self, img_path):
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
                "-drive",
                "file=%s,format=qcow2,if=ide,index=0,media=disk" % img_path,
                "-boot", "order=c",
                "-m", "256M", "-serial", "stdio",
                "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
                "-display", "vnc=:13",
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
            raise VmTestFailure("monitor socket did not appear")
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
                raise VmTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise VmTestFailure(
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


def boot_and_check(img_path, expect_file):
    b = Boot(img_path)
    try:
        b.wait_for("PrismOS 27H1", 30, "boot banner (from VM disk)")
        print("ok: boot banner (from VM disk)")
        b.wait_for("[boot] disk: installed system found", 30,
                   "installed system found")
        print("ok: installed system found on VM disk")
        b.wait_for("prismos> ", 30, "shell prompt")
        if expect_file:
            b.send("cat vm.txt")
            b.wait_for("vm image works", 20, "persisted file content")
            print("ok: file survived reboot on VM disk")
            b.wait_for("prismos> ", 20, "shell prompt")
        else:
            b.send("ver")
            b.wait_for("PrismOS version 27H1", 20, "ver output")
            print("ok: ver output")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("echo vm image works > vm.txt")
            b.wait_for("wrote 15 bytes to vm.txt", 20, "echo write")
            print("ok: file written to VM disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            # 画面: デスクトップが描かれていること。
            w, h, raw = b.screendump()
            if (w, h) != (1280, 800):
                raise VmTestFailure("unexpected screen size %dx%d" % (w, h))
            colored = 0
            for y in range(200, 600, 7):
                for x in range(200, 1080, 7):
                    i = (y * w + x) * 3
                    if raw[i] + raw[i + 1] + raw[i + 2] > 60:
                        colored += 1
            if colored < 100:
                raise VmTestFailure(
                    "wallpaper looks blank (colored samples=%d)" % colored)
            print("ok: desktop drawn (wallpaper samples=%d)" % colored)
    finally:
        b.stop()


def main():
    if not os.path.exists(IMG):
        print("MISSING: VM イメージがありません: %s "
              "(先に make vm-image してください)" % IMG)
        return 1
    print("ok: VM image present (%d bytes)" % os.path.getsize(IMG))
    # 成果物そのものは汚さない: テストは作業コピーを起動する。
    work = os.path.join(BUILD, "vm-test-work.qcow2")
    import shutil
    shutil.copyfile(IMG, work)
    try:
        boot_and_check(work, expect_file=False)
        boot_and_check(work, expect_file=True)
        print("VM TEST PASSED")
        return 0
    except VmTestFailure as e:
        print("MISSING: %s" % e)
        print("--- boot log (%s) ---" % LOG)
        try:
            with open(LOG, "rb") as f:
                sys.stdout.write(f.read().decode("utf-8", "replace"))
        except OSError:
            pass
        print("VM TEST FAILED")
        return 1


if __name__ == "__main__":
    sys.exit(main())
