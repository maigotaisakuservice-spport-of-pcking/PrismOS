#!/usr/bin/env python3
"""PrismOS primary-PIN-change key rewrap test.

Changing the primary (admin) PIN changes the disk master
key. Added users hold the master key wrapped under their
own keys; unless the wraps are re-made during `passwd`,
added users can never log in again (the known issue this
test guards). Flow:

  boot 1: install, set the primary PIN (1111), add alice
          (2222), set a marker (tz +0930), then change the
          primary PIN to 3333 -- the shell must report the
          rewrap for 1 user.
  boot 2: the old primary PIN is rejected, the new one
          logs in.
  boot 3: alice still logs in with 2222 and reads the
          settings encrypted under the NEW master key
          (date shows UTC+09:30).

Usage: python3 scripts/users-rewrap-test.py
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
DISK = os.path.join(ROOT, "build", "prism-rewrap-disk.img")
LOG = os.path.join(ROOT, "build", "users-rewrap-test.log")

QEMU = "qemu-system-x86_64"


class Failure(Exception):
    pass


def make_disk():
    try:
        os.unlink(DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", DISK,
                        "16M"], capture_output=True, text=True)
    if r.returncode != 0:
        raise Failure("qemu-img failed: %s" % r.stderr)


class Boot:
    def __init__(self):
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(LOG, "wb")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        args = [QEMU, "-cdrom", ISO,
                "-drive",
                "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
                "-boot", "order=d",
                "-m", "256M", "-serial", "stdio", "-monitor", "none",
                "-display", "none", "-no-reboot"]
        self.proc = subprocess.Popen(args, stdin=slave_fd,
                                     stdout=slave_fd,
                                     stderr=subprocess.DEVNULL,
                                     cwd=ROOT, close_fds=True)
        os.close(slave_fd)

    def pump(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            r, _, _ = select.select([self.master_fd], [], [],
                                    remaining)
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
                raise Failure("QEMU exited early (code %s)"
                              % self.proc.returncode)
            if time.monotonic() > deadline:
                raise Failure("%s not seen in %s s (expected %r); "
                              "tail=%r"
                              % (what, timeout, needle,
                                 bytes(self.buf[-300:])))
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


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    make_disk()
    try:
        b = Boot()
        try:
            b.wait_for("prismos> ", 180, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 60, "install done")
            b.wait_for("prismos> ", 60, "shell prompt")
            b.send("passwd")
            b.wait_for("New PIN", 30, "new PIN prompt")
            b.send("1111")
            b.wait_for("Confirm PIN", 30, "confirm prompt")
            b.send("1111")
            b.wait_for("Account set", 30, "account set")
            print("ok: primary PIN set")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("user add alice")
            b.wait_for("New PIN", 30, "alice PIN prompt")
            b.send("2222")
            b.wait_for("Confirm PIN", 30, "alice confirm")
            b.send("2222")
            b.wait_for("User added: alice", 30, "alice added")
            print("ok: alice added")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("tz +0930")
            b.wait_for("UTC+09:30", 30, "tz marker set")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("passwd")
            b.wait_for("Current PIN", 30, "current PIN prompt")
            b.send("1111")
            b.wait_for("New PIN", 30, "new PIN prompt")
            b.send("3333")
            b.wait_for("Confirm PIN", 30, "confirm prompt")
            b.send("3333")
            b.wait_for("re-wrapped the disk key for 1 user(s)", 30,
                       "rewrap report")
            print("ok: primary PIN change rewrapped alice's key")
            b.wait_for("Account set", 30, "account set again")
        finally:
            b.stop()

        b = Boot()
        try:
            b.wait_for("User (blank = admin", 180, "user prompt")
            b.send("")
            b.wait_for("PIN: ", 60, "PIN prompt")
            b.send("1111")
            b.wait_for("Wrong PIN.", 60, "old PIN rejected")
            print("ok: old primary PIN rejected")
            b.wait_for("PIN: ", 60, "PIN prompt again")
            b.send("3333")
            b.wait_for("Welcome to PrismOS.", 60, "new PIN login")
            print("ok: new primary PIN accepted")
        finally:
            b.stop()

        b = Boot()
        try:
            b.wait_for("User (blank = admin", 180, "user prompt")
            b.send("alice")
            b.wait_for("PIN: ", 60, "alice PIN prompt")
            b.send("2222")
            b.wait_for("Welcome to PrismOS.", 60, "alice welcome")
            print("ok: alice logged in after the PIN change")
            b.wait_for("prismos> ", 60, "shell prompt")
            b.send("date")
            b.wait_for("UTC+09:30", 60,
                       "settings decrypted under the new key")
            print("ok: alice reads files under the rewrapped key")
        finally:
            b.stop()
    except Failure as e:
        print("USERS REWRAP TEST FAILED:", e)
        return 1
    print("USERS REWRAP TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
