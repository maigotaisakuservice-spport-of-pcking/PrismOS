#!/usr/bin/env python3
"""PrismOS ARM 複数ユーザーテスト。

x86 版と同じ鍵ラップ方式 (kernel/users.c) が ARM でも
動くことを、QEMU の virt 構成 + virtio-blk で確かめる。

  1 回目: pinset で管理者を作り、useradd で alice を追加、
          共有ファイルを暗号化して dsync する。
          users.txt は平文で載り、共有ファイルの中身は
          イメージ内に平文で無いことを確認する。
  2 回目: alice でログイン (ユーザーを選び、自分の PIN で
          鍵ラップを開く) し、共有ファイルが復号されて
          読めること。userlist に alice が出ること。
  3 回目: alice の間違った PIN ではロックされること。
  4 回目: guest では保存できず、暗号文も復号できないこと。
  5 回目: 空ユーザー = admin で従来どおり入れること。

使い方: python3 scripts/arm-users-test.py
成功したら "ARM USERS TEST PASSED" と出す。
"""
import os
import select
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "build", "kernel-arm.bin")
DISK = os.path.join(ROOT, "build", "arm-users-test.img")
LOG = os.path.join(ROOT, "build", "arm-users.log")

FAILURES = []
MARKER = "shared-arm-secret"


def check(label, ok):
    print(("ok: " if ok else "FAIL: ") + label)
    if not ok:
        FAILURES.append(label)


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

    def send(self, line):
        self.proc.stdin.write(line.encode() + b"\n")
        self.proc.stdin.flush()

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

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


def main():
    if not os.path.exists(KERNEL):
        print("kernel-arm.bin がありません。make arm-kernel を先に。")
        return 1
    with open(DISK, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    log = open(LOG, "wb")

    # ---- 1 回目: 管理者作成 → alice 追加 → 暗号化して保存 ----
    sess = Session()
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
        check("useradd confirm", sess.read_until("Confirm PIN: ", 20.0))
        sess.send("5678")
        check("alice added",
              sess.read_until("useradd: added alice", 30.0))
        check("prompt after useradd",
              sess.read_until("prismos> ", 20.0))
        sess.send("userlist")
        seg = sess.read_segment("prismos> ", 20.0)
        check("userlist shows admin and alice",
              seg is not None and "admin" in seg and "alice" in seg)
        sess.send("echo %s > shared.txt" % MARKER)
        check("create shared file",
              sess.read_until("wrote %d bytes to shared.txt"
                              % (len(MARKER) + 1), 20.0))
        sess.send("dsync")
        check("dsync writes files",
              sess.read_until("file(s) written to disk", 30.0))
        log.write(sess.buf)
    finally:
        sess.stop()

    with open(DISK, "rb") as f:
        image = f.read()
    check("shared secret is NOT plaintext in the disk image",
          MARKER.encode() not in image)
    check("users.txt stored (alice record in plaintext)",
          b"alice" in image and b"users.txt" in image)

    # ---- 2 回目: alice でログインして復号できる ----
    sess = Session()
    try:
        check("user prompt (boot 2)",
              sess.read_until("User (blank = admin, or guest): ", 30.0))
        sess.send("alice")
        check("alice PIN prompt", sess.read_until("PIN: ", 20.0))
        sess.send("5678")
        check("alice logs in via key wrap",
              sess.read_until("login: welcome alice", 30.0))
        check("prompt (boot 2)", sess.read_until("prismos> ", 20.0))
        sess.send("cat shared.txt")
        check("alice decrypts the shared file",
              sess.read_until(MARKER, 20.0))
        log.write(sess.buf)
    finally:
        sess.stop()

    # ---- 3 回目: alice の間違った PIN ではロック ----
    sess = Session()
    try:
        check("user prompt (boot 3)",
              sess.read_until("User (blank = admin, or guest): ", 30.0))
        sess.send("alice")
        for _ in range(2):
            sess.send("0000")
            sess.read_until("PIN: ", 20.0)
        sess.send("0000")
        check("wrong PINs lock", sess.read_until("login: locked", 20.0))
        check("prompt (boot 3)", sess.read_until("prismos> ", 20.0))
        sess.send("cat shared.txt")
        seg = sess.read_segment("prismos> ", 20.0)
        check("secret stays ciphertext for wrong PIN",
              seg is not None and MARKER not in seg)
        log.write(sess.buf)
    finally:
        sess.stop()

    # ---- 4 回目: guest は保存も復号もできない ----
    sess = Session()
    try:
        check("user prompt (boot 4)",
              sess.read_until("User (blank = admin, or guest): ", 30.0))
        sess.send("guest")
        check("guest welcomed",
              sess.read_until("welcome guest", 20.0))
        check("prompt (boot 4)", sess.read_until("prismos> ", 20.0))
        sess.send("dsync")
        check("guest dsync refused",
              sess.read_until("dsync: guest session is not saved",
                              20.0))
        sess.send("cat shared.txt")
        seg = sess.read_segment("prismos> ", 20.0)
        check("guest cannot decrypt the shared file",
              seg is not None and MARKER not in seg)
        sess.send("useradd mallory")
        check("guest cannot add users",
              sess.read_until("useradd: guests cannot manage users",
                              20.0))
        log.write(sess.buf)
    finally:
        sess.stop()

    # ---- 5 回目: 空ユーザー = admin で従来どおり ----
    sess = Session()
    try:
        check("user prompt (boot 5)",
              sess.read_until("User (blank = admin, or guest): ", 30.0))
        sess.send("")
        check("admin PIN prompt", sess.read_until("PIN: ", 20.0))
        sess.send("1234")
        check("admin logs in", sess.read_until("login: welcome", 30.0))
        check("prompt (boot 5)", sess.read_until("prismos> ", 20.0))
        sess.send("cat shared.txt")
        check("admin decrypts the shared file",
              sess.read_until(MARKER, 20.0))
        log.write(sess.buf)
    finally:
        sess.stop()
    log.close()

    if FAILURES:
        print("ARM USERS TEST FAILED:", ", ".join(FAILURES))
        print("serial log:", LOG)
        return 1
    print("ARM USERS TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
