#!/usr/bin/env python3
"""PrismOS disk encryption test.

Boots the ISO in QEMU with a blank 16MB ATA disk, installs
PrismOS, sets a PIN with `passwd`, creates a secret file, then
reboots: after login the file must read back, but the disk
image itself must NOT contain the plaintext anywhere.

Usage: python3 scripts/crypto-test.py
  (exit code 0 = passed; standard library only)
"""

import os
import pty
import select
import signal
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
DISK = os.path.join(ROOT, "build", "prism-crypto-disk.img")
LOG = os.path.join(ROOT, "build", "boot-crypto.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-crypto.log")

QEMU = "qemu-system-x86_64"


class CryptoTestFailure(Exception):
    pass


def make_disk():
    try:
        os.unlink(DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", DISK, "16M"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise CryptoTestFailure("qemu-img failed: %s" % r.stderr)


def check_disk_has_no_plaintext(needle):
    with open(DISK, "rb") as f:
        data = f.read()
    if needle in data:
        raise CryptoTestFailure(
            "disk image contains plaintext %r (not encrypted!)" % needle)


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
                "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
                # CD first: the disk has an MBR signature after install,
                # so without this SeaBIOS tries to boot the disk (which
                # has no bootloader yet) instead of the CD.
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
                raise CryptoTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise CryptoTestFailure("%s not seen in %s s (expected %r)"
                                       % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

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

    def login(self):
        self.wait_for("PrismOS login", 30, "login screen")
        self.wait_for("PIN: ", 20, "PIN prompt")
        self.send("1234")
        self.wait_for("Welcome to PrismOS.", 20, "login welcome")
        print("ok: login with PIN 1234")
        # NOTE: do not wait for the prompt here: the decrypt
        # message prints after "Welcome" and before the prompt,
        # and wait_for only searches ahead of the last match.


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    make_disk()
    print("disk: created 16MB ATA disk")

    try:
        # Boot 1: install, set a PIN, create a secret file.
        b = Boot(LOG)
        try:
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            print("ok: installed to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("passwd")
            b.wait_for("New PIN", 20, "new PIN prompt")
            b.send("1234")
            b.wait_for("Confirm PIN", 20, "confirm PIN prompt")
            b.send("1234")
            b.wait_for("Account set", 20, "account set")
            print("ok: PIN set")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("echo my secret text > secret.txt")
            b.wait_for("wrote 15 bytes to secret.txt", 20,
                       "secret file written")
            print("ok: secret file written")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat secret.txt")
            b.wait_for("my secret text", 20, "secret readable")
            print("ok: secret readable in the same boot")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # The disk image must not contain the plaintext.
        check_disk_has_no_plaintext(b"my secret text")
        print("ok: disk image has no plaintext of the secret")

        # Boot 2: login, the secret must decrypt and read back.
        # Create a second secret while the key is active.
        b = Boot(LOG)
        try:
            b.login()
            # The exact count grows with the built-in file set;
            # what matters is that decryption ran at login.
            b.wait_for(" encrypted files", 20, "decrypt message")
            print("ok: encrypted files decrypted at login")
            b.wait_for("prismos> ", 20, "shell prompt after login")
            b.send("cat secret.txt")
            b.wait_for("my secret text", 20, "secret after reboot")
            print("ok: secret readable after reboot + login")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("echo second secret > second.txt")
            b.wait_for("wrote 14 bytes to second.txt", 20,
                       "second file written")
            print("ok: second secret written")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        check_disk_has_no_plaintext(b"my secret text")
        check_disk_has_no_plaintext(b"second secret")
        print("ok: disk image has no plaintext of either secret")

        # Boot 3: login again, both secrets must read back.
        b = Boot(LOG)
        try:
            b.login()
            b.wait_for(" encrypted files", 20,
                       "decrypt message for second boot")
            print("ok: encrypted files decrypted at 2nd login")
            b.wait_for("prismos> ", 20, "shell prompt after login")
            b.send("cat secret.txt")
            b.wait_for("my secret text", 20, "first secret after 2nd reboot")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat second.txt")
            b.wait_for("second secret", 20, "second secret after 2nd reboot")
            print("ok: both secrets readable after 2nd reboot + login")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        print("CRYPTO TEST PASSED")
        return 0
    except CryptoTestFailure as e:
        print("MISSING: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
