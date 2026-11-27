#!/usr/bin/env python3
"""PrismOS dual-boot test.

Builds the dual-boot disk (scripts/make-dualboot-disk.py), then
boots QEMU from the disk alone -- no CD attached -- twice:
  1. No key pressed: the menu times out (5 s) and boots the
     default entry, PrismOS. Proves the default side boots.
  2. Down + Enter on the Limine menu: boots the second entry,
     the guest system. Proves the menu really boots the system
     you pick, not the default.

The guest system is only a test stand-in for another OS
(see kernel/guest_main.c); it prints its own banner and stops.

Usage: python3 scripts/dualboot-test.py
  (exit code 0 = passed; standard library only)
"""

import os
import pty
import select
import signal
import socket
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISK = os.path.join(ROOT, "build", "prism-dualboot-disk.img")
LOG = os.path.join(ROOT, "build", "boot-dualboot.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-dualboot.log")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-dualboot.sock")
MENU_CONF = os.path.join(ROOT, "build", "dualboot-limine.conf")

QEMU = "qemu-system-x86_64"


class DualBootTestFailure(Exception):
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
        for path in (MONITOR_SOCK,):
            try:
                os.unlink(path)
            except OSError:
                pass
        args = [QEMU,
                "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
                "-boot", "order=c",
                "-m", "256M", "-serial", "stdio",
                "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
                "-display", "none", "-no-reboot",
                "-d", "cpu_reset,int", "-D", DEBUG_LOG]
        self.proc = subprocess.Popen(args, stdin=slave_fd, stdout=slave_fd,
                                     stderr=subprocess.DEVNULL, cwd=ROOT,
                                     close_fds=True)
        os.close(slave_fd)
        self.monitor_sock = None

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
                raise DualBootTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise DualBootTestFailure("%s not seen in %s s (expected %r)"
                                         % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def send(self, text):
        os.write(self.master_fd, (text + "\r").encode("ascii"))

    def monitor_connect(self, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5)
                s.connect(MONITOR_SOCK)
                self.monitor_sock = s
                self.hmp("")  # read the greeting
                return
            except OSError:
                s.close()
                if time.monotonic() > deadline:
                    raise DualBootTestFailure(
                        "could not connect to the QEMU monitor")
                time.sleep(0.2)

    def hmp(self, cmd):
        self.monitor_sock.sendall((cmd + "\n").encode("ascii"))
        data = b""
        while b"(qemu)" not in data:
            chunk = self.monitor_sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def sendkey(self, key):
        self.hmp("sendkey %s" % key)

    def stop(self):
        if self.monitor_sock is not None:
            try:
                self.monitor_sock.close()
            except OSError:
                pass
            try:
                os.unlink(MONITOR_SOCK)
            except OSError:
                pass
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
                        os.path.join(ROOT, "scripts",
                                     "make-dualboot-disk.py")],
                       capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode != 0:
        print(r.stderr, end="")
        print("MISSING: make-dualboot-disk.py failed")
        return 1

    # The menu must list both systems.
    with open(MENU_CONF, "r") as f:
        menu = f.read()
    if "/PrismOS" not in menu or "/Guest system" not in menu:
        print("MISSING: dual-boot menu does not list both systems")
        return 1
    print("ok: dual-boot menu lists PrismOS and Guest system")

    try:
        # Boot 1: no key pressed, the menu times out and boots
        # the default entry (PrismOS). Then install to the data
        # partition only (`install dual yes`) and write a file.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            print("ok: default entry boots PrismOS (menu timeout)")
            b.wait_for("prismos> ", 30, "shell prompt")
            print("ok: PrismOS shell prompt from the dual-boot disk")
            b.send("install dual yes")
            b.wait_for("Installed to the dual-boot disk's data partition",
                       30, "install dual done")
            print("ok: install dual writes the data partition")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("echo dual boot works > dual.txt")
            # "dual boot works" is 15 chars + the newline echo adds.
            b.wait_for("wrote 16 bytes to dual.txt", 20, "echo write")
            print("ok: file written after install dual")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # Host check: the boot side must be untouched by the
        # install (MBR, boot partition, both kernels).
        import struct as _struct
        with open(DISK, "rb") as f:
            mbr = f.read(512)
            if mbr[510] != 0x55 or mbr[511] != 0xAA:
                raise DualBootTestFailure("MBR signature lost after install dual")
            p1 = mbr[446:462]
            if p1[4] != 0x0E or _struct.unpack("<I", p1[8:12])[0] != 2048:
                raise DualBootTestFailure("boot partition lost after install dual")
            p2 = mbr[462:478]
            if p2[4] != 0x83 or _struct.unpack("<I", p2[8:12])[0] != 34816:
                raise DualBootTestFailure("data partition lost after install dual")
        r = subprocess.run(["mdir", "-i", "%s@@%d" % (DISK, 2048 * 512),
                            "::/boot"], capture_output=True, text=True)
        # mdir shows 8.3 names as "prismos  elf" / "guest    elf".
        if "prismos" not in r.stdout or "guest" not in r.stdout:
            raise DualBootTestFailure(
                "boot partition files lost after install dual")
        print("ok: boot menu and both kernels untouched by install dual")

        # Boot 2: default again, the file must survive.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("cat dual.txt")
            b.wait_for("dual boot works", 20, "persisted file content")
            print("ok: file survived reboot on the dual-boot disk")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # Boot 3: Down + Enter selects the second entry.
        # The menu appears about 2 s after QEMU starts and
        # times out after 5 s, so press the keys at ~3 s.
        b = Boot(LOG)
        try:
            b.monitor_connect()
            time.sleep(3.0)
            b.sendkey("down")
            time.sleep(0.5)
            b.sendkey("ret")
            b.wait_for("GUEST SYSTEM 1.0", 30, "guest banner")
            print("ok: menu selection boots the guest system")
            b.wait_for("Other system stand-in", 20, "guest stand-in line")
            print("ok: guest system is the other system, not PrismOS")
        finally:
            b.stop()

        print("DUAL BOOT TEST PASSED")
        return 0
    except DualBootTestFailure as e:
        print("MISSING: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
