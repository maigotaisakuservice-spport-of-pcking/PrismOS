#!/usr/bin/env python3
"""PrismOS kernel update test.

Boots the ISO in QEMU with a bootable disk (32MB FAT16 boot
partition + PRISMFS2 data partition) and a local HTTP server,
and checks the kernel update end to end:
  1. build a second kernel with version 0.3.0 (tree copy, only
     kernel/version.h changed) and serve it as the update;
  2. `update check` finds 0.3.0 and says it includes a kernel
     update;
  3. `update apply yes` streams the new kernel onto the boot
     partition (/boot/prismos.new), verifies its SHA-256 and
     swaps it into /boot/prismos.elf;
  4. reboot from the disk alone: the banner and `ver` say
     0.3.0, and the old kernel is gone (no prismos.new left);
  5. on an old-style disk (12MB boot partition, FAT12) the same
     update fails honestly ("boot partition is not FAT16"),
     the file update rolls back, and the old kernel still boots;
  6. an oversized kernel image (30MB, correct hash) fills the
     boot partition mid-stream, fails honestly ("ran out of
     space"), and the previous kernel still boots.

Usage: python3 scripts/kernel-update-test.py
  (exit code 0 = passed; standard library only)
"""

import hashlib
import importlib.util
import os
import pty
import select
import shutil
import signal
import subprocess
import sys
import termios
import time
import tty

_spec = importlib.util.spec_from_file_location(
    "recovery_test",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "recovery-test.py"))
rt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rt)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
DISK = os.path.join(ROOT, "build", "prism-kernel-update-disk.img")
OLD_DISK = os.path.join(ROOT, "build", "prism-kernel-update-old-disk.img")
LOG = os.path.join(ROOT, "build", "boot-kernel-update.log")
WWW = "/tmp/prism-www-kernel-update"
COPY_TREE = "/tmp/prism-newkernel-tree"

QEMU = "qemu-system-x86_64"


class KernelUpdateTestFailure(Exception):
    pass


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def write_bytes(path, data):
    with open(path, "wb") as f:
        f.write(data)


def write_text(path, data):
    with open(path, "w") as f:
        f.write(data)


def manifest(version, mtype, base, files, kernel_hash):
    lines = ["version=%s" % version, "channel=stable",
             "type=%s" % mtype, "base=%s" % base]
    for name, data in files:
        lines.append("file %s %s" % (name, sha256_hex(data)))
    if kernel_hash is not None:
        lines.append("kernel %s" % kernel_hash)
    return "\n".join(lines) + "\n"


def sign(path):
    """マニフェストにテスト用鍵で署名し、<path>.sig を置く。"""
    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run(
        [sys.executable, os.path.join(here, "pq-sign.py"), path],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise KernelUpdateTestFailure("sign failed for %s: %s"
                                     % (path, r.stderr.strip()))


def build_new_kernel():
    """ツリーをコピーし、version.h だけ 0.3.0 にして新しい
    カーネルをビルドする。元のツリーは触らない。"""
    if os.path.exists(COPY_TREE):
        shutil.rmtree(COPY_TREE)
    shutil.copytree(ROOT, COPY_TREE,
                    ignore=shutil.ignore_patterns(
                        "build", "__pycache__", ".git", "*.log",
                        "*.img", "*.ppm"))
    version_h = os.path.join(COPY_TREE, "kernel", "version.h")
    with open(version_h) as f:
        text = f.read()
    if '"0.2.0"' not in text:
        raise KernelUpdateTestFailure(
            "version.h does not say 0.2.0 in the tree copy")
    with open(version_h, "w") as f:
        # 内部順序バージョン (PRISM_VERSION_ORDER) と表示ラベル
        # (PRISM_VERSION) の両方を 0.3.0 に進めたコピーを作る。
        f.write(text.replace('"0.2.0"', '"0.3.0"')
                    .replace('"27H1"', '"0.3.0"'))
    r = subprocess.run(["make", "kernel"], cwd=COPY_TREE,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise KernelUpdateTestFailure(
            "new kernel build failed: %s%s" % (r.stdout, r.stderr))
    new_kernel = os.path.join(COPY_TREE, "build", "kernel.elf")
    if not os.path.exists(new_kernel):
        raise KernelUpdateTestFailure("new kernel.elf not built")
    with open(new_kernel, "rb") as f:
        data = f.read()
    # "PrismOS version " と "0.3.0" は別のリテラルで、実行時に
    # つながる (core.c の ver コマンド)。バイナリには両方が
    # 別々に含まれる。
    if b"0.3.0" not in data or b"PrismOS version " not in data:
        raise KernelUpdateTestFailure(
            "new kernel does not contain version 0.3.0")
    print("new kernel: %d bytes, version 0.3.0" % len(data))
    return data


def make_disk(disk, boot_sectors=None):
    cmd = [sys.executable, os.path.join(ROOT, "scripts", "make-disk.py"),
           "--disk", disk]
    if boot_sectors is not None:
        cmd += ["--boot-sectors", str(boot_sectors)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode != 0:
        print(r.stderr, end="")
        raise KernelUpdateTestFailure("make-disk.py failed")


class Boot:
    """One QEMU run: serial over a pty. with_cd boots the ISO,
    otherwise the disk alone. with_net attaches the e1000 card
    (user networking, host is 10.0.2.2)."""

    def __init__(self, log_path, disk, with_cd, with_net):
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(log_path, "wb")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        args = [QEMU]
        if with_cd:
            args += ["-cdrom", ISO]
        # ディスクは AHCI 接続にする。IDE (ATA PIO) だと 1 セクタ
        # ごとに 256 回のポート入出力が必要で、エミュレーション下
        # では 4.6MB のカーネル書き込みに 15 分以上かかる。
        # AHCI なら DMA で運ぶ (driver-test.py の AHCI 例と同じ)。
        args += ["-device", "ahci,id=ahci",
                 "-drive",
                 "file=%s,format=raw,if=none,id=d0" % disk,
                 "-device", "ide-hd,drive=d0,bus=ahci.0",
                 "-boot", "order=d" if with_cd else "order=c",
                 "-m", "256M", "-serial", "stdio", "-monitor", "none",
                 "-display", "none", "-no-reboot"]
        if with_net:
            args += ["-netdev", "user,id=net0",
                     "-device", "e1000,netdev=net0"]
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
                raise KernelUpdateTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise KernelUpdateTestFailure(
                    "%s not seen in %s s (expected %r)"
                    % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def wait_for_exit(self, timeout, what):
        deadline = time.monotonic() + timeout
        while True:
            if self.proc.poll() is not None:
                return
            if time.monotonic() > deadline:
                raise KernelUpdateTestFailure(
                    "%s: QEMU did not exit in %s s" % (what, timeout))
            time.sleep(0.2)

    def send(self, text):
        os.write(self.master_fd, (text + "\r").encode("ascii"))

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


def boot_prompt(b, banner, timeout=60):
    # バナーだけ待つ。プロンプトまで待つと、バナーとプロンプトの
    # 間にある行 (ディスクの行など) を見逃す。
    b.wait_for(banner, timeout, "boot banner")


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    new_kernel = build_new_kernel()
    make_disk(DISK)
    print("disk: created bootable disk (32MB boot partition)")

    # Update server content for 0.3.0 (files + kernel).
    os.makedirs(os.path.join(WWW, "updates"), exist_ok=True)
    version_030 = b"0.3.0\n"
    write_bytes(os.path.join(WWW, "updates", "VERSION"), version_030)
    write_bytes(os.path.join(WWW, "updates", "kernel.elf"), new_kernel)
    write_text(os.path.join(WWW, "update-manifest.txt"),
               manifest("0.3.0", "feature", "/updates",
                        [("VERSION", version_030)],
                        sha256_hex(new_kernel)))
    sign(os.path.join(WWW, "update-manifest.txt"))

    rt.WWW = WWW
    server = rt.HttpServer()
    server.start()
    try:
        # --- Boot 1: CD + disk, apply the kernel update. ---
        b = Boot(LOG, DISK, with_cd=True, with_net=True)
        try:
            boot_prompt(b, "PrismOS 27H1")
            print("ok: boot banner (27H1 from ISO)")
            b.wait_for("[boot] disk: data partition at LBA 67584", 30,
                       "data partition found")
            print("ok: data partition at LBA 67584")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            print("ok: installed to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("update server 10.0.2.2 /update-manifest.txt")
            b.wait_for("Update server: 10.0.2.2 /update-manifest.txt", 20,
                       "update server set")
            print("ok: update server configured")
            b.wait_for("prismos> ", 20, "shell prompt")

            b.send("update check")
            b.wait_for("Update available: 0.3.0 (feature, stable)", 60,
                       "update check")
            b.wait_for("This update includes a kernel update", 20,
                       "kernel update note")
            print("ok: update check finds 0.3.0 with a kernel update")
            b.wait_for("prismos> ", 20, "shell prompt")

            b.send("update apply yes")
            b.wait_for("Kernel updated: 0.3.0 (", 300, "kernel updated")
            print("ok: kernel streamed onto the boot partition")
            b.wait_for("Update applied: 0.3.0 (1 downloaded, 0 unchanged).",
                       60, "update applied")
            print("ok: update applied (1 file + kernel)")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat VERSION")
            b.wait_for("0.3.0", 20, "VERSION updated")
            print("ok: VERSION is 0.3.0")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # Host check: the boot partition holds the new kernel and
        # no leftover prismos.new.
        r = subprocess.run(
            ["mdir", "-i", "%s@@%d" % (DISK, 2048 * 512), "::/boot"],
            capture_output=True, text=True)
        if "prismos" not in r.stdout:
            raise KernelUpdateTestFailure(
                "prismos.elf missing from boot partition after update")
        if "new" in r.stdout:
            raise KernelUpdateTestFailure(
                "prismos.new left on boot partition after update")
        print("ok: boot partition holds the new kernel, no .new left")
        print(r.stdout.strip())

        # --- Boot 2: disk alone, the new kernel must boot. ---
        b = Boot(LOG, DISK, with_cd=False, with_net=False)
        try:
            boot_prompt(b, "PrismOS 0.3.0")
            print("ok: new kernel banner (0.3.0 from disk)")
            b.wait_for("[boot] disk: installed system found", 30,
                       "installed system found")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("ver")
            b.wait_for("PrismOS version 0.3.0", 20, "ver is 0.3.0")
            print("ok: ver says 0.3.0 after kernel update")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat VERSION")
            b.wait_for("0.3.0", 20, "VERSION still 0.3.0")
            print("ok: VERSION is 0.3.0 on the new kernel")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # --- Boot 3: old-style disk (12MB boot partition, FAT12).
        # The same update must fail honestly, roll the files back,
        # and leave the old kernel bootable. ---
        make_disk(OLD_DISK, boot_sectors=24576)
        print("disk: created old-style disk (12MB boot partition)")
        b = Boot(LOG, OLD_DISK, with_cd=True, with_net=True)
        try:
            boot_prompt(b, "PrismOS 27H1")
            b.wait_for("prismos> ", 30, "shell prompt")
            # CD 起動時の VFS は空で VERSION は無いので、0.2.0 の
            # VERSION を作ってから install する。こうすると更新前の
            # 状態 (VERSION 0.2.0) がスナップショットに載り、
            # ロールバックでそれが戻ることを確かめられる。
            b.send("echo 0.2.0 > VERSION")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat VERSION")
            b.wait_for("0.2.0", 20, "VERSION 0.2.0 installed")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("update server 10.0.2.2 /update-manifest.txt")
            b.wait_for("Update server: 10.0.2.2 /update-manifest.txt", 20,
                       "update server set")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("update apply yes")
            b.wait_for("Update failed: boot partition is not FAT16",
                       120, "old disk refused honestly")
            print("ok: old disk refused with a reason (not FAT16)")
            b.wait_for("Rolled back to the restore point.", 20,
                       "rolled back")
            print("ok: file update rolled back on the old disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat VERSION")
            b.wait_for("0.2.0", 20, "VERSION still 0.2.0")
            print("ok: VERSION is still 0.2.0 on the old disk")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()
        b = Boot(LOG, OLD_DISK, with_cd=False, with_net=False)
        try:
            boot_prompt(b, "PrismOS 27H1")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("ver")
            b.wait_for("PrismOS version 27H1", 20, "ver still 27H1")
            print("ok: old kernel still boots on the old disk")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # --- Boot 4: oversized kernel on the standard disk. ---
        # The download fills the boot partition mid-stream and
        # must fail honestly; the 0.3.0 kernel must still boot.
        big_kernel = bytes(30 * 1024 * 1024)
        version_040 = b"0.4.0\n"
        write_bytes(os.path.join(WWW, "updates", "VERSION"), version_040)
        write_bytes(os.path.join(WWW, "updates", "kernel.elf"), big_kernel)
        write_text(os.path.join(WWW, "update-manifest.txt"),
                   manifest("0.4.0", "feature", "/updates",
                            [("VERSION", version_040)],
                            sha256_hex(big_kernel)))
        sign(os.path.join(WWW, "update-manifest.txt"))
        print("oversized kernel: 30MB, hash in manifest")
        b = Boot(LOG, DISK, with_cd=True, with_net=True)
        try:
            boot_prompt(b, "PrismOS 27H1")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("update server 10.0.2.2 /update-manifest.txt")
            b.wait_for("Update server: 10.0.2.2 /update-manifest.txt", 20,
                       "update server set")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("update apply yes")
            b.wait_for("Update failed: boot partition ran out of space "
                       "for the new kernel.", 600,
                       "oversized kernel refused honestly")
            print("ok: oversized kernel refused mid-stream (no space)")
            b.wait_for("Rolled back to the restore point.", 20,
                       "rolled back")
            print("ok: file update rolled back after oversized kernel")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat VERSION")
            b.wait_for("0.3.0", 20, "VERSION still 0.3.0")
            print("ok: VERSION is still 0.3.0 after oversized kernel")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()
        b = Boot(LOG, DISK, with_cd=False, with_net=False)
        try:
            boot_prompt(b, "PrismOS 0.3.0")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("ver")
            b.wait_for("PrismOS version 0.3.0", 20, "ver still 0.3.0")
            print("ok: 0.3.0 kernel still boots after oversized update")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        print("KERNEL UPDATE TEST PASSED")
        return 0
    except rt.RecoveryTestFailure as e:
        print("MISSING: %s" % e)
        return 1
    except KernelUpdateTestFailure as e:
        print("MISSING: %s" % e)
        return 1
    finally:
        server.stop()


if __name__ == "__main__":
    sys.exit(main())
