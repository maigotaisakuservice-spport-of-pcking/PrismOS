#!/usr/bin/env python3
"""PrismOS disk-alone boot test.

Builds the bootable disk image (scripts/make-disk.py), then boots
QEMU from the disk alone -- no CD attached -- and checks:
  1. Limine boots the kernel from the disk's boot partition,
  2. the kernel finds the installed system on the data partition,
  3. a file written to disk survives a reboot from disk.

Usage: python3 scripts/disk-boot-test.py
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
DISK = os.path.join(ROOT, "build", "prism-disk-boot.img")
LOG = os.path.join(ROOT, "build", "boot-disk.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-disk.log")

QEMU = "qemu-system-x86_64"


class DiskBootTestFailure(Exception):
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
        args = [QEMU,
                "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
                "-boot", "order=c",
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
                raise DiskBootTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise DiskBootTestFailure("%s not seen in %s s (expected %r)"
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


def main():
    r = subprocess.run([sys.executable,
                        os.path.join(ROOT, "scripts", "make-disk.py")],
                       capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode != 0:
        print(r.stderr, end="")
        print("MISSING: make-disk.py failed")
        return 1

    try:
        # Boot 1: disk alone, write a file.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            print("ok: boot banner (from disk)")
            # Boot order in the log: disk lines come before the
            # bootloader line, so wait in that order.
            b.wait_for("[boot] disk: data partition at LBA 67584", 30,
                       "data partition found")
            print("ok: data partition found")
            b.wait_for("[boot] disk: installed system found", 30,
                       "installed system found")
            print("ok: installed system found on disk")
            b.wait_for("bootloader: Limine", 30, "bootloader line")
            print("ok: booted by Limine from disk")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("echo disk boot works > boot.txt")
            b.wait_for("wrote 16 bytes to boot.txt", 20, "echo write")
            print("ok: file written to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # Boot 2: same disk, the file must survive.
        b = Boot(LOG)
        try:
            b.wait_for("[boot] disk: installed system found", 30,
                       "installed system found")
            print("ok: installed system found on reboot")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("cat boot.txt")
            b.wait_for("disk boot works", 20, "persisted file content")
            print("ok: file survived reboot from disk")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        print("DISK BOOT TEST PASSED")
        return 0
    except DiskBootTestFailure as e:
        print("MISSING: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
