#!/usr/bin/env python3
"""PrismOS ARM の対話操作テスト (virtio-input + 画面コンソール)。

QEMU virt に virtio-keyboard-device (virtio-mmio) を付けて:
  * 起動ログに virtio キーボードの準備完了が出る
  * モニタの sendkey で打った `ver` がシリアル側のシェルに
    届いて応答が返る (入力が virtio-input 経由)
  * スクリーンダンプで、画面コンソールにプロンプトと
    エコーが描かれている (白い画素) ことを確かめる

使い方: python3 scripts/arm-input-test.py
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
KERNEL = os.path.join(ROOT, "build", "kernel-arm.bin")
LOG = os.path.join(ROOT, "build", "arm-input.log")
SOCK = os.path.join(ROOT, "build", "qemu-monitor-arm-input.sock")
SHOT = os.path.join(ROOT, "build", "shots", "arm-input.ppm")

QEMU = "qemu-system-aarch64"


class ArmInputFailure(Exception):
    pass


def main():
    if not os.path.exists(KERNEL):
        print("MISSING: ARM kernel not found: %s" % KERNEL)
        return 1
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    os.makedirs(os.path.dirname(SHOT), exist_ok=True)
    log_file = open(LOG, "wb")
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    args = [QEMU, "-M", "virt", "-cpu", "cortex-a57", "-m", "256",
            "-device", "ramfb",
            "-device", "virtio-keyboard-device",
            "-serial", "stdio",
            "-monitor", "unix:%s,server,nowait" % SOCK,
            "-display", "vnc=:10",
            "-no-reboot", "-kernel", KERNEL]
    proc = subprocess.Popen(args, stdin=slave_fd, stdout=slave_fd,
                            stderr=subprocess.DEVNULL, cwd=ROOT,
                            close_fds=True)
    os.close(slave_fd)
    buf = bytearray()
    pos = 0

    def pump(timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            r, _, _ = select.select([master_fd], [], [],
                                    deadline - time.monotonic())
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
        nonlocal pos
        target = needle.encode()
        deadline = time.monotonic() + timeout
        while True:
            idx = buf.find(target, pos)
            if idx >= 0:
                pos = idx + len(target)
                return
            if time.monotonic() > deadline:
                tail = bytes(buf[-2000:]).decode(errors="replace")
                raise ArmInputFailure(
                    "MISSING: %s (%r not seen in %d s)\n--- tail ---\n%s"
                    % (what, needle, timeout, tail))
            pump(0.25)

    mon = None

    def hmp(cmd, wait=0.35):
        nonlocal mon
        if mon is None:
            for _ in range(50):
                try:
                    mon = socket.socket(socket.AF_UNIX,
                                        socket.SOCK_STREAM)
                    mon.connect(SOCK)
                    mon.settimeout(2)
                    mon.recv(4096)
                    break
                except OSError:
                    time.sleep(0.2)
        mon.sendall((cmd + "\n").encode())
        time.sleep(wait)
        try:
            mon.recv(4096)
        except socket.timeout:
            pass

    try:
        wait_for("[boot] input: virtio keyboard ready", 40,
                 "virtio keyboard ready")
        print("ok: virtio keyboard initialised")
        wait_for("prismos> ", 20, "shell prompt")

        # Type `ver` + Enter on the virtio keyboard only.
        for key in ("v", "e", "r"):
            hmp("sendkey %s" % key)
        hmp("sendkey ret", 0.6)
        wait_for("PrismOS version 27H1", 20,
                 "ver typed on the virtio keyboard")
        print("ok: virtio keyboard input reaches the shell")
        wait_for("prismos> ", 20, "prompt after ver")

        # The screen console must show text (prompt + echo).
        hmp("screendump %s" % SHOT, 0.8)
        with open(SHOT, "rb") as f:
            data = f.read()
        rest = data[3:]
        parts = []
        i = 0
        while len(parts) < 3:
            while i < len(rest) and rest[i:i + 1].isspace():
                i += 1
            j = i
            while j < len(rest) and not rest[j:j + 1].isspace():
                j += 1
            parts.append(int(rest[i:j]))
            i = j
        i += 1
        w = parts[0]
        raw = rest[i:]
        white = 0
        for y in range(8, 700):
            for x in range(8, 1000):
                k = (y * w + x) * 3
                if raw[k] > 200 and raw[k + 1] > 200 and raw[k + 2] > 200:
                    white += 1
        if white < 200:
            raise ArmInputFailure(
                "screen console shows little text (%d white px)" % white)
        print("ok: screen console shows the session (%d white px)"
              % white)
    except ArmInputFailure as e:
        print(str(e))
        print("ARM INPUT TEST FAILED")
        return 1
    finally:
        try:
            proc.kill()
        except OSError:
            pass
        log_file.close()
    print("ARM INPUT TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
