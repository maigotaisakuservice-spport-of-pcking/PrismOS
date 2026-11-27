#!/usr/bin/env python3
"""PrismOS built-in app fault-containment test.

The built-in desktop apps run in kernel space, so the
desktop guards their draw/key/mouse dispatch with appguard:
a fault inside the guarded region is caught by the IDT
handler and bailed back to the dispatch site instead of
halting the kernel. `appguard test` deliberately faults
inside a guarded region (the same role evil.elf plays for
the sandbox) and must report containment, after which the
shell must still answer.

Usage: python3 scripts/appguard-test.py
  (exit code 0 = passed; standard library only)
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
ISO = os.path.join(ROOT, "build", "prismos.iso")
LOG = os.path.join(ROOT, "build", "appguard-test.log")

QEMU = "qemu-system-x86_64"


class Failure(Exception):
    pass


class Boot:
    def __init__(self):
        self.buf = bytearray()
        self.pos = 0
        self.log = open(LOG, "wb")
        m, s = pty.openpty()
        tty.setraw(s)
        a = termios.tcgetattr(s)
        a[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(s, termios.TCSANOW, a)
        self.master = m
        self.proc = subprocess.Popen(
            [QEMU, "-cdrom", ISO, "-boot", "d", "-m", "256",
             "-display", "none", "-serial", "stdio",
             "-monitor", "none", "-no-reboot"],
            stdin=s, stdout=s, stderr=subprocess.DEVNULL,
            close_fds=True)
        os.close(s)

    def pump(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], 0.25)
            if self.master in r:
                try:
                    data = os.read(self.master, 65536)
                except OSError:
                    data = b""
                if data:
                    self.buf += data
                    self.log.write(data)
                    self.log.flush()
                    return True
        return False

    def wait_for(self, needle, timeout=120):
        end = time.time() + timeout
        while time.time() < end:
            idx = self.buf.find(needle.encode(), self.pos)
            if idx >= 0:
                self.pos = idx + len(needle)
                return
            self.pump(1)
        raise Failure("timeout waiting for %r; tail=%r"
                      % (needle, bytes(self.buf[-400:])))

    def cmd(self, line):
        os.write(self.master, (line + "\n").encode())
        time.sleep(0.3)

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main():
    if not os.path.exists(ISO):
        raise Failure("ISO missing; run `make iso` first")
    b = Boot()
    try:
        b.wait_for("prismos>", timeout=300)
        b.cmd("appguard")
        b.wait_for("fault containment is on", timeout=30)
        b.cmd("appguard test")
        b.wait_for("[appguard] fault in a built-in app", timeout=30)
        b.wait_for("self-test: fault contained, desktop alive",
                   timeout=30)
        # シェルがまだ応答すること (カーネルが生きている)。
        b.cmd("appguard")
        b.wait_for("fault containment is on", timeout=30)
        print("APPGUARD TEST PASSED")
        return 0
    finally:
        b.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as e:
        print("APPGUARD TEST FAILED:", e)
        sys.exit(1)
