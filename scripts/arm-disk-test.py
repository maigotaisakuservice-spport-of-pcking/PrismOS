#!/usr/bin/env python3
"""PrismOS ARM ディスク永続化テスト。

QEMU の virt 構成に virtio-blk-device で生ディスクを付け、
4 回起動する。1 回目: バナーと「未フォーマット」を確認し、
ファイルを作って dsync する。2 回目 (同じディスク):
起動時にファイルが読み込まれ、cat で中身が見えること。
続けて pinset でアカウントを作り、秘密ファイルを暗号化
して dsync する (ディスクイメージ内に平文が無いことも
確認)。3 回目: 間違った PIN ではロックされ、秘密が
暗号文のまま・dsync も拒否されること。4 回目: 正しい
PIN でログインでき、復号されて読めること。

使い方: python3 scripts/arm-disk-test.py
成功したら "ARM DISK TEST PASSED" と出す。
"""
import os
import select
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "build", "kernel-arm.bin")
DISK = os.path.join(ROOT, "build", "arm-disk-test.img")
LOG = os.path.join(ROOT, "build", "arm-disk.log")

FAILURES = []


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
        """needle までの消費分 (needle 含む) を文字列で返す。"""
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


def boot_expect(sess, steps):
    for needle, label, timeout in steps:
        if not sess.read_until(needle, timeout):
            print(f"MISSING: {label} が {timeout} 秒待っても出ませんでした"
                  f" (expected '{needle}')。")
            return False
        print("ok: " + label)
    return True


def main():
    if not os.path.exists(KERNEL):
        print("kernel-arm.bin がありません。make arm-kernel を先に。")
        return 1
    # まっさらな 16MB ディスク
    with open(DISK, "wb") as f:
        f.truncate(16 * 1024 * 1024)

    log = open(LOG, "wb")

    # ---- 1 回目の起動 ----
    sess = Session()
    try:
        ok = boot_expect(sess, [
            ("PrismOS 27H1 (ARM64)", "boot banner (ARM)", 30.0),
            ("[boot] disk: virtio disk present, not formatted",
             "disk detected, not formatted", 30.0),
            ("prismos> ", "shell prompt (boot 1)", 30.0),
        ])
        check("boot 1 startup", ok)
        if ok:
            sess.send("echo arm disk works > d.txt")
            ok = sess.read_until("wrote 15 bytes to d.txt", 20.0)
            check("create file in VFS", ok)
            sess.send("dsync")
            ok = sess.read_until("file(s) written to disk", 30.0)
            check("dsync writes files", ok)
            sess.send("dls")
            ok = sess.read_until("d.txt", 20.0)
            check("dls lists d.txt", ok)
            sess.send("disk")
            ok = sess.read_until("formatted (PRISMFS)", 20.0)
            check("disk reports formatted", ok)
        log.write(sess.buf)
    finally:
        sess.stop()

    # ---- 2 回目の起動 (同じディスク) ----
    sess = Session()
    try:
        ok = boot_expect(sess, [
            ("[boot] disk:", "disk boot line (boot 2)", 30.0),
            ("file(s) loaded from virtio disk",
             "files auto-loaded at boot", 30.0),
            ("prismos> ", "shell prompt (boot 2)", 30.0),
        ])
        check("boot 2 startup", ok)
        if ok:
            sess.send("cat d.txt")
            ok = sess.read_until("arm disk works", 20.0)
            check("file persisted across reboot", ok)
            # アカウントを作り、暗号化を有効にしてから
            # 秘密ファイルを書いて同期する。
            sess.send("pinset")
            ok = sess.read_until("New PIN: ", 20.0)
            check("pinset prompts for a new PIN", ok)
            sess.send("1234")
            ok = sess.read_until("Confirm PIN: ", 20.0)
            check("pinset asks to confirm", ok)
            sess.send("1234")
            ok = sess.read_until(
                "pinset: account created; disk encryption is on",
                30.0)
            check("pinset creates the account", ok)
            sess.send("echo secret-arm-data > s.txt")
            ok = sess.read_until("wrote 16 bytes to s.txt", 20.0)
            check("create secret file", ok)
            sess.send("dsync")
            ok = sess.read_until("file(s) written to disk", 30.0)
            check("dsync encrypts with the PIN key", ok)
        log.write(sess.buf)
    finally:
        sess.stop()

    # ディスクイメージ内に平文の秘密が無いこと (暗号化の証拠)。
    with open(DISK, "rb") as f:
        image = f.read()
    check("secret is NOT plaintext in the disk image",
          b"secret-arm-data" not in image)
    check(".account is stored on disk", b".account" in image)

    # ---- 3 回目の起動: 間違った PIN では復号できない ----
    sess = Session()
    try:
        ok = boot_expect(sess, [
            ("PIN: ", "login prompt (boot 3)", 30.0),
        ])
        check("boot 3 login prompt", ok)
        if ok:
            for _ in range(2):
                sess.send("9999")
                sess.read_until("PIN: ", 20.0)
            sess.send("9999")
            ok = sess.read_until("login: locked", 20.0)
            check("wrong PINs lock the disk", ok)
            ok = sess.read_until("prismos> ", 20.0)
            check("shell prompt while locked", ok)
            sess.send("cat s.txt")
            seg = sess.read_segment("prismos> ", 20.0)
            check("secret stays ciphertext while locked",
                  seg is not None and "secret-arm-data" not in seg)
            sess.send("dsync")
            ok = sess.read_until("dsync: locked", 20.0)
            check("dsync refuses while locked", ok)
        log.write(sess.buf)
    finally:
        sess.stop()

    # ---- 4 回目の起動: 正しい PIN で復号できる ----
    sess = Session()
    try:
        ok = boot_expect(sess, [
            ("PIN: ", "login prompt (boot 4)", 30.0),
        ])
        check("boot 4 login prompt", ok)
        if ok:
            sess.send("1234")
            ok = sess.read_until("login: welcome", 30.0)
            check("correct PIN logs in and decrypts", ok)
            ok = sess.read_until("prismos> ", 20.0)
            check("shell prompt (boot 4)", ok)
            sess.send("cat s.txt")
            ok = sess.read_until("secret-arm-data", 20.0)
            check("secret readable after login", ok)
            sess.send("cat d.txt")
            ok = sess.read_until("arm disk works", 20.0)
            check("older file also decrypted after login", ok)
        log.write(sess.buf)
    finally:
        sess.stop()
    log.close()

    if FAILURES:
        print("ARM DISK TEST FAILED:", ", ".join(FAILURES))
        print("serial log:", LOG)
        return 1
    print("ARM DISK TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
