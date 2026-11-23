#!/usr/bin/env python3
"""PrismOS 32bit ネットワークテスト。

kernel32 に共有 netstack + e1000 を載せたことを、
qemu-system-i386 + user ネットワークで確かめる。

  dhcp    → "dhcp: address 10.0.2.15"
  ping    → "ping: reply from 10.0.2.2"
  netinfo → "net: ip 10.0.2.15 router 10.0.2.2 dns 10.0.2.3"

使い方: python3 scripts/net32-test.py (make iso32 の後)
成功したら "NET32 TEST PASSED" と出す。
"""
import os
import pty
import select
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos-32.iso")
LOG = os.path.join(ROOT, "build", "net32.log")

FAILURES = []


def check(label, ok):
    print(("ok: " if ok else "FAIL: ") + label)
    if not ok:
        FAILURES.append(label)


class Session:
    def __init__(self):
        self.buf = b""
        self.pos = 0
        self.log = open(LOG, "wb")
        master, slave = pty.openpty()
        tty.setraw(slave)
        attrs = termios.tcgetattr(slave)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave, termios.TCSANOW, attrs)
        self.master = master
        self.proc = subprocess.Popen(
            [
                "qemu-system-i386",
                "-cdrom", ISO,
                "-boot", "order=d",
                "-m", "256M",
                "-serial", "stdio",
                "-display", "none",
                "-no-reboot",
                "-nic", "user,model=e1000",
            ],
            stdin=slave, stdout=slave,
            stderr=subprocess.DEVNULL, cwd=ROOT, close_fds=True)
        os.close(slave)

    def wait_for(self, needle, timeout=40.0):
        needle_b = needle.encode()
        deadline = time.monotonic() + timeout
        while True:
            idx = self.buf.find(needle_b, self.pos)
            if idx >= 0:
                self.pos = idx + len(needle_b)
                return True
            if time.monotonic() > deadline:
                return False
            r, _, _ = select.select([self.master], [], [], 0.5)
            if not r:
                continue
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                return False
            if not chunk:
                return False
            self.buf += chunk
            self.log.write(chunk)
            self.log.flush()

    def send(self, cmd):
        os.write(self.master, (cmd + "\r").encode())

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log.close()


def main():
    if not os.path.exists(ISO):
        print("32bit ISO がありません。make iso32 を先に。")
        return 1
    sess = Session()
    try:
        check("shell prompt", sess.wait_for("prismos> ", 60.0))
        sess.send("dhcp")
        check("dhcp gives 10.0.2.15",
              sess.wait_for("dhcp: address 10.0.2.15", 60.0))
        check("prompt after dhcp", sess.wait_for("prismos> ", 20.0))
        sess.send("netinfo")
        check("netinfo shows ip/router/dns",
              sess.wait_for("net: ip 10.0.2.15 router 10.0.2.2 "
                            "dns 10.0.2.3", 20.0))
        check("prompt after netinfo",
              sess.wait_for("prismos> ", 20.0))
        sess.send("ping 10.0.2.2")
        check("ping gets a reply",
              sess.wait_for("ping: reply from 10.0.2.2", 40.0))
    finally:
        sess.stop()
    if FAILURES:
        print("NET32 TEST FAILED (%d)" % len(FAILURES))
        return 1
    print("NET32 TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
