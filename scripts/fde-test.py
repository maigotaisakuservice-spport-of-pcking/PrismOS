#!/usr/bin/env python3
"""PrismOS full-disk encryption (FDE) test.

Boots the ISO in QEMU with a blank 16MB ATA disk:
  1. install + set a PIN + write a marker file,
  2. log in and run `fde enable` (sector-level migration),
     then check the raw disk image: the FDE header is at
     sector 0 and neither the diskfs superblock magic, the
     file name, nor the file contents appear in plaintext,
  3. reboot and fail the passphrase 3 times: the disk must
     stay locked and the marker file must be unavailable,
  4. reboot with the right passphrase: unlock, log in, the
     marker file reads back,
  5. reboot with the recovery key instead of the passphrase,
  6. request recovery mode, reboot: unlock + login must
     come before the recovery menu,
  7. in the recovery session, exit to the shell and run
     `fde disable`,
  8. reboot: no passphrase prompt anymore, login works and
     the marker file is back; the raw image shows the
     plaintext superblock magic again.

Usage: python3 scripts/fde-test.py
  (exit code 0 = passed; standard library only)
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
DISK = os.path.join(ROOT, "build", "prism-fde-disk.img")
LOG = os.path.join(ROOT, "build", "boot-fde.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-fde.log")

QEMU = "qemu-system-x86_64"

PASSPHRASE = "prism-test-1"
PIN = "1234"
MARKER = "FDE-MARKER-7Q2X"


class FdeTestFailure(Exception):
    pass


def make_disk():
    try:
        os.unlink(DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", DISK, "16M"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise FdeTestFailure("qemu-img failed: %s" % r.stderr)


def disk_bytes():
    with open(DISK, "rb") as f:
        return f.read()


class Boot:
    def __init__(self, log_path):
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(log_path, "ab")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        args = [QEMU, "-cdrom", ISO,
                "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
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
                raise FdeTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise FdeTestFailure("%s not seen in %s s (expected %r)"
                                     % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def wait_for_re(self, pattern, timeout, what):
        deadline = time.monotonic() + timeout
        while True:
            m = re.search(pattern, bytes(self.buf[self.pos:]))
            if m is not None:
                self.pos += m.end()
                return m
            if self.proc.poll() is not None:
                raise FdeTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise FdeTestFailure("%s not seen in %s s (expected %r)"
                                     % (what, timeout, pattern))
            self.pump(min(0.5, deadline - time.monotonic()))

    def wait_for_exit(self, timeout, what):
        deadline = time.monotonic() + timeout
        while self.proc.poll() is None:
            if time.monotonic() > deadline:
                raise FdeTestFailure("%s: QEMU did not exit" % what)
            time.sleep(0.2)

    def segment_since(self, pos):
        return bytes(self.buf[pos:])

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


def login(b):
    b.wait_for("PrismOS login", 60, "login screen")
    b.wait_for("PIN: ", 20, "PIN prompt")
    b.send(PIN)
    b.wait_for("Welcome to PrismOS.", 20, "login welcome")
    b.wait_for("prismos> ", 20, "shell prompt after login")


def unlock(b, passphrase):
    b.wait_for("Disk passphrase: ", 60, "passphrase prompt")
    b.send(passphrase)
    b.wait_for("Disk unlocked.", 60, "disk unlocked")


def check_marker(b):
    b.send("cat secret.txt")
    b.wait_for(MARKER, 20, "marker file content")
    b.wait_for("prismos> ", 20, "shell prompt")


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    try:
        os.unlink(LOG)
    except OSError:
        pass
    make_disk()
    print("disk: created 16MB ATA disk")

    try:
        # Stage 1: install, set a PIN, write the marker file.
        b = Boot(LOG)
        try:
            b.wait_for("prismos> ", 60, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 60, "install done")
            print("ok: installed to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("passwd")
            b.wait_for("New PIN", 20, "new PIN prompt")
            b.send(PIN)
            b.wait_for("Confirm PIN", 20, "confirm PIN prompt")
            b.send(PIN)
            b.wait_for("Account set", 20, "account set")
            print("ok: PIN set")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("echo %s > secret.txt" % MARKER)
            b.wait_for("wrote %d bytes to secret.txt" % (len(MARKER) + 1),
                       20, "marker written")
            print("ok: marker file written")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # Stage 2: log in, enable FDE, read the file back.
        recovery_key = [None]
        b = Boot(LOG)
        try:
            login(b)
            b.send("fde")
            b.wait_for("Full-disk encryption: off", 20, "fde off")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("fde enable")
            b.wait_for("New disk passphrase", 20, "passphrase prompt")
            b.send(PASSPHRASE)
            b.wait_for("Confirm passphrase: ", 20, "confirm prompt")
            b.send(PASSPHRASE)
            # "enabled." はリカバリキーより前に出るので先に待つ。
            b.wait_for("Full-disk encryption enabled.", 600,
                       "enabled message")
            m = b.wait_for_re(
                rb"Recovery key \(write it down and keep it "
                rb"safe\): ([A-Z2-9]+)", 60, "recovery key shown")
            recovery_key[0] = m.group(1).decode("ascii")
            print("ok: FDE enabled (recovery key %s...)"
                  % recovery_key[0][:4])
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("fde")
            b.wait_for("Full-disk encryption: on (unlocked)", 20,
                       "fde on")
            b.wait_for("prismos> ", 20, "shell prompt")
            check_marker(b)
            print("ok: marker readable in the enabling session")
        finally:
            b.stop()

        # Raw image: header present, nothing else in plaintext.
        data = disk_bytes()
        if data[0:8] != b"PRISMFK1":
            raise FdeTestFailure("FDE header magic not at sector 0")
        if b"PRISMFS2" in data:
            raise FdeTestFailure("diskfs superblock magic in plaintext")
        if b"secret.txt" in data:
            raise FdeTestFailure("file name in plaintext on disk")
        if MARKER.encode() in data:
            raise FdeTestFailure("file contents in plaintext on disk")
        print("ok: raw image is encrypted (header only)")

        # Stage 3: three wrong passphrases; disk stays locked.
        b = Boot(LOG)
        try:
            for i in range(3):
                b.wait_for("Disk passphrase: ", 60,
                           "passphrase prompt %d" % i)
                b.send("wrong-pass-%d" % i)
                b.wait_for("Wrong passphrase.", 20, "wrong rejected")
            b.wait_for("Disk stays locked", 20, "locked message")
            print("ok: wrong passphrases rejected, disk locked")
            b.wait_for("prismos> ", 30, "shell prompt (diskless)")
            mark = b.pos
            b.send("cat secret.txt")
            b.wait_for("prismos> ", 20, "shell prompt after cat")
            seg = b.segment_since(mark)
            if MARKER.encode() in seg:
                raise FdeTestFailure("marker readable while locked")
            if b"No such file" not in seg:
                raise FdeTestFailure(
                    "cat of a disk file did not fail while locked")
            print("ok: disk files unavailable while locked")
        finally:
            b.stop()

        # Stage 4: right passphrase unlocks; login; file reads.
        b = Boot(LOG)
        try:
            unlock(b, PASSPHRASE)
            print("ok: unlocked with the passphrase")
            login(b)
            check_marker(b)
            print("ok: marker readable after unlock + login")
        finally:
            b.stop()

        # Stage 5: the recovery key also unlocks.
        b = Boot(LOG)
        try:
            unlock(b, recovery_key[0])
            print("ok: unlocked with the recovery key")
            login(b)
            check_marker(b)
            print("ok: marker readable after recovery-key unlock")
            # Ask for recovery mode at the next boot.
            b.send("recovery")
            b.wait_for("Rebooting into recovery mode", 20,
                       "recovery reboot")
            b.wait_for_exit(20, "QEMU reset into recovery")
            print("ok: rebooted into recovery mode")
        finally:
            b.stop()

        # Stage 6: unlock + login come before the recovery menu.
        # (login() はプロンプトまで待つので、回復メニューが
        # 先に出るこの段では使えない。)
        b = Boot(LOG)
        try:
            unlock(b, PASSPHRASE)
            b.wait_for("PrismOS login", 60, "login screen")
            b.wait_for("PIN: ", 20, "PIN prompt")
            b.send(PIN)
            b.wait_for("Welcome to PrismOS.", 20, "login welcome")
            b.wait_for("PrismOS Recovery", 30, "recovery menu")
            print("ok: recovery menu after unlock + login")
            b.send("5")
            b.wait_for("prismos> ", 30, "shell prompt after recovery")
            check_marker(b)
            print("ok: marker readable after recovery session")
            # Disable FDE again.
            b.send("fde disable")
            b.wait_for("Disk passphrase: ", 20, "disable confirm")
            b.send(PASSPHRASE)
            b.wait_for("Full-disk encryption disabled.", 600,
                       "disabled message")
            print("ok: FDE disabled")
            b.wait_for("prismos> ", 20, "shell prompt")
            check_marker(b)
            print("ok: marker readable right after disabling")
        finally:
            b.stop()

        # Stage 7: plain disk again — no passphrase prompt.
        b = Boot(LOG)
        try:
            login(b)
            if b"Disk passphrase" in bytes(b.buf):
                raise FdeTestFailure(
                    "passphrase prompt shown after FDE was disabled")
            print("ok: no passphrase prompt after disabling")
            check_marker(b)
            print("ok: marker readable on the plain disk")
        finally:
            b.stop()

        data = disk_bytes()
        if data[0:8] == b"PRISMFK1":
            raise FdeTestFailure("FDE header still present")
        if b"PRISMFS2" not in data:
            raise FdeTestFailure("superblock magic not back in plaintext")
        print("ok: raw image is plaintext again")

        print("FDE TEST PASSED")
        return 0
    except FdeTestFailure as e:
        print("MISSING: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
