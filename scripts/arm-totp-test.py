#!/usr/bin/env python3
"""PrismOS ARM TOTP テスト。

ARM 版に実時刻 (PL031 RTC) と TOTP の門を移植したことを
QEMU の virt 構成 + virtio-blk で確かめる。

  1 回目: pinset で管理者を作り、useradd で alice を追加、
          管理者で `totp setup` する。表示された現在コードが
          ホスト側の計算 (秘密と実時刻) と一致すること。
  2 回目: 管理者ログインで PIN の後に `Code:` を聞かれる。
          門の出す unix が実時刻 (起動からの経過秒ではない)
          であること。間違ったコードは拒否され、正しい
          コードで入れること。
  3 回目: alice でログインして alice の `totp setup`。
  4 回目: alice のログインでも門が働き、正しいコードで
          入れること。

使い方: python3 scripts/arm-totp-test.py
成功したら "ARM TOTP TEST PASSED" と出す。
"""
import base64
import hashlib
import hmac as hmac_mod
import os
import re
import select
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "build", "kernel-arm.bin")
DISK = os.path.join(ROOT, "build", "arm-totp-test.img")
LOG = os.path.join(ROOT, "build", "arm-totp.log")

FAILURES = []


def check(label, ok):
    print(("ok: " if ok else "FAIL: ") + label)
    if not ok:
        FAILURES.append(label)


def totp_at(secret_b32, unix):
    pad = "=" * ((8 - len(secret_b32) % 8) % 8)
    key = base64.b32decode(secret_b32 + pad)
    ctr = int(unix) // 30
    digest = hmac_mod.new(key, struct.pack(">Q", ctr),
                          hashlib.sha1).digest()
    off = digest[-1] & 0x0F
    code = (struct.unpack(">I", digest[off:off + 4])[0] & 0x7FFFFFFF)
    return code % 1000000


def wrong_code(secret_b32, unix):
    valid = {totp_at(secret_b32, unix + d) for d in (-30, 0, 30)}
    cand = 0
    while cand in valid:
        cand += 1
    return cand


class Session:
    def __init__(self):
        self.buf = b""
        self.proc = subprocess.Popen(
            [
                "qemu-system-aarch64",
                "-M", "virt",
                "-cpu", "cortex-a57",
                "-m", os.environ.get("PRISM_ARM_MEM_MB", "256"),
                "-kernel", KERNEL,
                "-device", "ramfb",
                "-drive", f"file={DISK},if=none,format=raw,id=hd0",
                "-device", "virtio-blk-device,drive=hd0",
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

    def read_segment(self, needle, timeout=30.0):
        needle_b = needle.encode()
        deadline = time.time() + timeout
        while needle_b not in self.buf:
            if self.proc.poll() is not None:
                return None
            remain = deadline - time.time()
            if remain <= 0:
                return None
            r, _, _ = select.select([self.proc.stdout], [], [], remain)
            if not r:
                return None
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                return None
            self.buf += chunk
        idx = self.buf.index(needle_b) + len(needle_b)
        seg = self.buf[:idx]
        self.buf = self.buf[idx:]
        return seg.decode("utf-8", "replace")

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


def setup_totp(sess, who):
    sess.send("totp setup")
    seg = sess.read_segment("Current code should read: ", 30.0)
    if seg is None:
        check("%s totp setup prints secret" % who, False)
        return None
    m = re.search(r"TOTP secret \(enter it in your "
                  r"authenticator app\): ([A-Z2-7]+)", seg)
    if not m:
        check("%s totp setup prints secret" % who, False)
        return None
    secret = m.group(1)
    seg2 = sess.read_segment("\n", 10.0)
    shown = None
    if seg2 is not None:
        m2 = re.search(r"(\d{6})", seg2)
        if m2:
            shown = int(m2.group(1))
    now = time.time()
    expect = {totp_at(secret, now + d) for d in (-30, 0, 30)}
    check("%s totp setup: shown current code matches host "
          "computation" % who, shown in expect)
    sess.read_until("prismos> ", 20.0)
    return secret


def gated_login(sess, user, pin, secret, who):
    ok = sess.read_until("User (blank = admin, or guest): ", 30.0)
    check("%s user prompt" % who, ok)
    sess.send(user)
    check("%s PIN prompt" % who, sess.read_until("PIN: ", 20.0))
    sess.send(pin)
    seg = sess.read_segment("Code: ", 30.0)
    check("%s totp gate asks for a code" % who, seg is not None)
    if seg is None:
        return
    m = re.search(r"totp: unix=(\d+)", seg)
    guest_unix = int(m.group(1)) if m else 0
    skew = guest_unix - int(time.time())
    check("%s gate time is real wall-clock (unix=%d, skew=%ds)"
          % (who, guest_unix, skew),
          guest_unix > 1700000000 and abs(skew) <= 12 * 3600)
    sess.send("%06d" % wrong_code(secret, guest_unix))
    check("%s wrong code rejected" % who,
          sess.read_until("Wrong code.", 20.0))
    check("%s code re-prompted" % who,
          sess.read_until("Code: ", 20.0))
    sess.send("%06d" % totp_at(secret, guest_unix))
    check("%s correct code accepted" % who,
          sess.read_until("Code accepted.", 20.0))


def main():
    if not os.path.exists(KERNEL):
        print("kernel-arm.bin がありません。make arm-kernel を先に。")
        return 1
    with open(DISK, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    log = open(LOG, "wb")

    # ---- 1 回目: 管理者作成 → alice 追加 → 管理者の totp setup ----
    sess = Session()
    admin_secret = None
    try:
        check("boot 1 prompt", sess.read_until("prismos> ", 30.0))
        sess.send("pinset")
        check("pinset new PIN", sess.read_until("New PIN: ", 20.0))
        sess.send("1234")
        check("pinset confirm", sess.read_until("Confirm PIN: ", 20.0))
        sess.send("1234")
        check("account created",
              sess.read_until("pinset: account created", 30.0))
        sess.send("useradd alice")
        check("useradd new PIN", sess.read_until("New PIN: ", 20.0))
        sess.send("5678")
        check("useradd confirm",
              sess.read_until("Confirm PIN: ", 20.0))
        sess.send("5678")
        check("alice added",
              sess.read_until("useradd: added alice", 30.0))
        sess.read_until("prismos> ", 20.0)
        admin_secret = setup_totp(sess, "admin")
        log.write(sess.buf)
    finally:
        sess.stop()
    if admin_secret is None:
        print("ARM TOTP TEST FAILED")
        return 1

    # ---- 2 回目: 管理者ログインに門 ----
    sess = Session()
    try:
        gated_login(sess, "", "1234", admin_secret, "admin")
        check("admin welcome after gate",
              sess.read_until("login: welcome", 20.0))
        log.write(sess.buf)
    finally:
        sess.stop()

    # ---- 3 回目: alice で入って alice の totp setup ----
    sess = Session()
    alice_secret = None
    try:
        check("boot 3 user prompt",
              sess.read_until("User (blank = admin, or guest): ",
                              30.0))
        sess.send("alice")
        check("boot 3 PIN prompt", sess.read_until("PIN: ", 20.0))
        sess.send("5678")
        check("alice logs in (no gate yet)",
              sess.read_until("login: welcome alice", 30.0))
        sess.read_until("prismos> ", 20.0)
        alice_secret = setup_totp(sess, "alice")
        log.write(sess.buf)
    finally:
        sess.stop()
    if alice_secret is None:
        print("ARM TOTP TEST FAILED")
        return 1

    # ---- 4 回目: alice のログインにも門 ----
    sess = Session()
    try:
        gated_login(sess, "alice", "5678", alice_secret, "alice")
        check("alice welcome after gate",
              sess.read_until("login: welcome alice", 20.0))
        log.write(sess.buf)
    finally:
        sess.stop()

    log.close()
    if FAILURES:
        print("ARM TOTP TEST FAILED (%d)" % len(FAILURES))
        return 1
    print("ARM TOTP TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
