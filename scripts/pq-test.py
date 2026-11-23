#!/usr/bin/env python3
"""PrismOS 耐量子 (ML-KEM / ML-DSA) の QEMU テスト。

`pq` シェルコマンドを実機 (QEMU) で通しで確かめる:
  * pq keygen → pq encaps と pq decaps のセッション
    フィンガープリントが一致する (鍵交換が成立)
  * pq seal / pq open で文章が往復する
  * 鍵を差し替えた後は復号の認証が失敗する (HMAC)
  * pq signkey → pq sign → pq verify が通り、文章を
    変えると pq verify が INVALID を返す

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
LOG = os.path.join(ROOT, "build", "boot-pq.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-pq.log")

QEMU = "qemu-system-x86_64"


class PqTestFailure(Exception):
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
                raise PqTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise PqTestFailure("%s not seen in %s s (expected %r)"
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

        # --- ML-KEM 鍵交換 ---
        b.send("pq keygen")
        b.wait_for("pq: ML-KEM-768 key pair stored", 30, "keygen")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("pq encaps pqkem.pub ct.bin")
        b.wait_for("pq: ciphertext stored; session key derived.", 30,
                   "encaps")
        b.wait_for("session fingerprint", 20, "encaps fingerprint")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("pq decaps ct.bin")
        b.wait_for("pq: session key derived.", 30, "decaps")
        b.wait_for("session fingerprint", 20, "decaps fingerprint")
        b.wait_for("prismos> ", 20, "shell prompt")
        fps = b.fingerprints()
        if len(fps) < 2 or fps[0] != fps[1]:
            raise PqTestFailure(
                "encaps/decaps fingerprints differ: %r" % (fps,))
        print("ok: ML-KEM key exchange (fingerprint %s)" % fps[0])

        # --- seal / open の往復 ---
        b.send("echo hello pq world > msg.txt")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("pq seal msg.txt msg.sealed")
        b.wait_for("pq: sealed.", 30, "seal")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("pq open msg.sealed msg.out")
        b.wait_for("pq: opened.", 30, "open")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("cat msg.out")
        b.wait_for("hello pq world", 20, "opened text matches")
        print("ok: pq seal/open round trip")
        b.wait_for("prismos> ", 20, "shell prompt")

        # --- 鍵を差し替えると認証が失敗すること ---
        b.send("pq keygen")
        b.wait_for("pq: ML-KEM-768 key pair stored", 30, "re-keygen")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("pq decaps ct.bin")
        b.wait_for("pq: session key derived.", 30, "decaps wrong key")
        b.wait_for("prismos> ", 20, "shell prompt")
        fps = b.fingerprints()
        if len(fps) < 3 or fps[2] == fps[0]:
            raise PqTestFailure(
                "wrong-key decaps fingerprint should differ: %r" % (fps,))
        b.send("pq open msg.sealed msg.bad")
        b.wait_for("pq: authentication failed", 30,
                   "open with wrong key refused")
        print("ok: wrong key cannot open a sealed file")
        b.wait_for("prismos> ", 20, "shell prompt")

        # --- ML-DSA 署名 ---
        b.send("pq signkey")
        b.wait_for("pq: ML-DSA-44 key pair stored", 30, "signkey")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("pq sign msg.txt")
        b.wait_for("pq: signed.", 30, "sign")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("pq verify msg.txt")
        b.wait_for("pq: signature OK.", 30, "verify ok")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("echo changed text > msg.txt")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("pq verify msg.txt")
        b.wait_for("pq: signature INVALID.", 30, "verify tampered")
        print("ok: ML-DSA sign/verify + tamper detection")
        b.wait_for("prismos> ", 20, "shell prompt")
    except PqTestFailure as e:
        print("FAIL: %s" % e)
        print("PQ TEST FAILED")
        return 1
    finally:
        b.stop()
    print("PQ TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
