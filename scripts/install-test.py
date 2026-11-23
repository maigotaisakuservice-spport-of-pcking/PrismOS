#!/usr/bin/env python3
"""PrismOS install / persistence test.

Boots the ISO in QEMU with a blank 16MB ATA disk, installs
PrismOS to it, then reboots with the same disk and checks the
files are still there. Also checks that a deleted file stays
deleted after another reboot, and verifies the MBR and the
filesystem superblock on the host side.

Usage: python3 scripts/install-test.py
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
DISK = os.path.join(ROOT, "build", "prism-disk.img")
LOG = os.path.join(ROOT, "build", "boot-install.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-install.log")

QEMU = "qemu-system-x86_64"


class InstallTestFailure(Exception):
    pass


def make_disk():
    try:
        os.unlink(DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", DISK, "16M"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise InstallTestFailure("qemu-img failed: %s" % r.stderr)


def check_mbr():
    with open(DISK, "rb") as f:
        data = f.read(1024)
    if len(data) < 1024:
        raise InstallTestFailure("disk image too small")
    if data[510] != 0x55 or data[511] != 0xAA:
        raise InstallTestFailure("MBR signature missing")
    if data[446 + 4] != 0x83:
        raise InstallTestFailure("partition type is not 0x83")
    if data[512:520] != b"PRISMFS2":
        raise InstallTestFailure("filesystem superblock magic missing")


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
                raise InstallTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise InstallTestFailure("%s not seen in %s s (expected %r)"
                                         % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def send(self, text):
        os.write(self.master_fd, (text + "\r").encode("ascii"))

    def segment_since(self, pos):
        return bytes(self.buf[pos:])

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
    print("disk: created 16MB ATA disk")

    try:
        # Boot 1: fresh disk, install.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            print("ok: boot banner")
            b.wait_for("[boot] disk: (not installed", 30, "disk not installed")
            print("ok: disk reported as not installed")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("echo hello prism > note.txt")
            b.wait_for("wrote 12 bytes to note.txt", 20, "echo write")
            print("ok: file written to RAM file system")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            print("ok: installed to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()
        check_mbr()
        print("ok: MBR and superblock on disk")

        # Boot 2: same disk, files must survive.
        b = Boot(LOG)
        try:
            b.wait_for("[boot] disk: installed system found", 30,
                       "installed system found")
            print("ok: installed system found on reboot")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("cat note.txt")
            b.wait_for("hello prism", 20, "persisted file content")
            print("ok: file survived reboot")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("rm note.txt")
            b.wait_for("removed note.txt", 20, "rm file")
            print("ok: file removed")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # Boot 3: the deleted file must stay deleted.
        b = Boot(LOG)
        try:
            b.wait_for("prismos> ", 30, "shell prompt")
            mark = b.pos
            b.send("ls")
            b.wait_for("prismos> ", 20, "shell prompt after ls")
            seg = b.segment_since(mark)
            if b"note.txt" in seg:
                raise InstallTestFailure("deleted file came back after reboot")
            print("ok: deleted file stays deleted after reboot")
        finally:
            b.stop()

        print("INSTALL TEST PASSED")
        return 0
    except InstallTestFailure as e:
        print("MISSING: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
