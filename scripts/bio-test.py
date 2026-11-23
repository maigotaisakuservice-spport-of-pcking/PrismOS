#!/usr/bin/env python3
"""PrismOS 生体認証 (指紋) の QEMU テスト。

`bio` コマンドと `lock` を QEMU で通しで確かめる:
  * 模擬センサーで指紋を登録し、同じ試料で一致、
    違う試料で不一致になること
  * 3 回失敗で生体がロックされ、正しい試料でも
    弾かれること
  * lock の解除: 生体ロック中は PIN が通り、PIN で
    入ると生体ロックが解け、次は生体で開くこと

使い方: python3 scripts/pq-test.py
  (終了コード 0 = 合格。標準ライブラリのみ)
"""

import os
import pty
import re
import select
import signal
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
LOG = os.path.join(ROOT, "build", "boot-bio.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-bio.log")

QEMU = "qemu-system-x86_64"


class BioTestFailure(Exception):
    pass


class Boot:
    def __init__(self, log_path):
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(log_path, "wb")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        args = [QEMU, "-cdrom", ISO,
                "-boot", "order=d",
                "-m", "256M", "-serial", "stdio", "-monitor", "none",
                "-display", "none", "-no-reboot",
                "-d", "cpu_reset,int", "-D", DEBUG_LOG]
        self.proc = subprocess.Popen(args, stdin=slave_fd, stdout=slave_fd,
                                     stderr=subprocess.DEVNULL, cwd=ROOT,
                                     close_fds=True)
        os.close(slave_fd)

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
                raise BioTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise BioTestFailure("%s not seen in %s s (expected %r)"
                                    % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def send(self, text):
        os.write(self.master_fd, (text + "\r").encode("ascii"))

    def fingerprints(self):
        """これまでに出たセッション fingerprint を全部返す。"""
        text = bytes(self.buf).decode("utf-8", "replace")
        return re.findall(r"session fingerprint ([0-9a-f]{8})", text)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        os.close(self.master_fd)
        self.log_file.close()


def main():
    if not os.path.exists(ISO):
        print("missing %s (run `make` first)" % ISO)
        return 1
    b = Boot(LOG)
    try:
        b.wait_for("prismos> ", 30, "shell prompt")
        print("ok: shell prompt")

        # PIN を用意する (lock の PIN 解除用)。
        b.send("passwd")
        b.wait_for("New PIN", 20, "new PIN prompt")
        b.send("1234")
        b.wait_for("Confirm PIN", 20, "confirm PIN prompt")
        b.send("1234")
        b.wait_for("Account set", 20, "account set")
        b.wait_for("prismos> ", 20, "shell prompt")
        print("ok: PIN set")

        # 登録と照合。
        b.send("bio status")
        b.wait_for("bio: finger not enrolled", 20, "not enrolled yet")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("bio mock 7")
        b.wait_for("presenting sample 7", 20, "mock 7")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("bio enroll finger")
        b.wait_for("bio: enrolled.", 20, "enrolled")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("bio verify finger")
        b.wait_for("bio: match.", 20, "same sample matches")
        b.wait_for("prismos> ", 20, "shell prompt")
        print("ok: enroll + match")

        # 違う試料は 3 回で生体ロック。
        b.send("bio mock 9")
        b.wait_for("prismos> ", 20, "shell prompt")
        for i in range(3):
            b.send("bio verify finger")
            b.wait_for("bio: no match.", 20, "wrong sample %d" % i)
            b.wait_for("prismos> ", 20, "shell prompt")
        b.send("bio mock 7")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("bio verify finger")
        b.wait_for("bio: locked (too many failures).", 20,
                   "locked after 3 failures")
        b.wait_for("prismos> ", 20, "shell prompt")
        print("ok: 3 failures lock biometrics")

        # lock: 生体ロック中は PIN で開き、生体も回復する。
        b.send("lock")
        b.wait_for("Locked.", 20, "locked")
        b.send("bio")
        b.wait_for("Biometrics locked. Use the PIN.", 20,
                   "bio refused while locked")
        b.send("9999")
        b.wait_for("Wrong PIN.", 20, "wrong PIN refused")
        b.send("1234")
        b.wait_for("Unlocked.", 20, "PIN unlocks")
        print("ok: lock screen PIN fallback")

        # 回復後は生体で開く。
        b.send("lock")
        b.wait_for("Locked.", 20, "locked again")
        b.send("bio")
        b.wait_for("Unlocked.", 20, "bio unlocks")
        print("ok: lock screen bio unlock")
    except BioTestFailure as e:
        print("FAIL: %s" % e)
        print("BIO TEST FAILED")
        return 1
    finally:
        b.stop()
    print("BIO TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
