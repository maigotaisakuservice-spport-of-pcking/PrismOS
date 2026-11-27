#!/usr/bin/env python3
"""PrismOS installer test.

Builds the bootable disk image (scripts/make-disk.py), then:
  1. boots the ISO with that disk attached, opens the Installer
     app (Ctrl+I), walks welcome -> disk -> profile -> confirm,
     installs the standard profile, and reboots from the disk;
  2. boots the disk alone and checks the installed files are
     there (prism.bmp present, settings.txt says standard);
  3. repeats the whole flow with a fresh disk and the minimal
     profile, then boots the disk alone and checks only the
     core files are there (no prism.bmp / melody.wav / vid
     frames), settings.txt says minimal, and the desktop
     reports the minimal profile (4 apps).

Usage: python3 scripts/installer-test.py
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
ISO = os.path.join(ROOT, "build", "prismos.iso")
DISK = os.path.join(ROOT, "build", "prism-disk-boot.img")
LOG = os.path.join(ROOT, "build", "boot-installer.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-installer.log")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-installer.sock")
SHOT_DIR = os.path.join(ROOT, "build", "shots")

QEMU = "qemu-system-x86_64"


class InstallerTestFailure(Exception):
    pass


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise InstallerTestFailure("screendump is not a P6 PPM: %s" % path)
    parts = data.split(b"\n", 3)
    w, h = (int(x) for x in parts[1].split())
    maxval = int(parts[2])
    if maxval != 255:
        raise InstallerTestFailure("unexpected PPM maxval %d" % maxval)
    return w, h, parts[3]


def count_whiteish(raw, width, height, x0, y0, x1, y1):
    n = 0
    for y in range(max(0, y0), min(height, y1)):
        for x in range(max(0, x0), min(width, x1)):
            i = (y * width + x) * 3
            if raw[i] > 180 and raw[i + 1] > 180 and raw[i + 2] > 180:
                n += 1
    return n


class Boot:
    """One QEMU run: serial over a pty, monitor over a unix
    socket, VNC display for screendumps."""

    def __init__(self, log_path, with_cd, boot_order):
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
        args += ["-drive",
                 "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
                 "-boot", "order=%s" % boot_order,
                 "-m", "256M", "-serial", "stdio",
                 "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
                 "-display", "vnc=:8",
                 "-no-reboot",
                 "-d", "cpu_reset,int", "-D", DEBUG_LOG]
        try:
            os.unlink(MONITOR_SOCK)
        except OSError:
            pass
        self.proc = subprocess.Popen(args, stdin=slave_fd, stdout=slave_fd,
                                     stderr=subprocess.DEVNULL, cwd=ROOT,
                                     close_fds=True)
        os.close(slave_fd)
        self.monitor_sock = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5)
                s.connect(MONITOR_SOCK)
                self.monitor_sock = s
                break
            except OSError:
                time.sleep(0.2)
        if self.monitor_sock is None:
            raise InstallerTestFailure("monitor socket did not appear")
        self.hmp("")  # consume the initial "(qemu)" banner

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
                raise InstallerTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise InstallerTestFailure("%s not seen in %s s (expected %r)"
                                         % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def wait_exit(self, timeout, what):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return
            time.sleep(0.2)
        raise InstallerTestFailure("%s did not exit in %s s" % (what, timeout))

    def send_serial(self, text):
        os.write(self.master_fd, (text + "\r").encode("ascii"))

    def hmp(self, cmd):
        self.monitor_sock.sendall((cmd + "\n").encode("ascii"))
        data = b""
        while b"(qemu)" not in data:
            chunk = self.monitor_sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def sendkey(self, key, delay=0.3):
        self.hmp("sendkey %s" % key)
        time.sleep(delay)

    def screendump(self, name):
        path = os.path.join(SHOT_DIR, name)
        self.hmp("screendump %s" % path)
        time.sleep(0.5)
        return read_ppm(path)

    def run_command(self, cmd, timeout=20):
        """Send a shell command, return the text it printed."""
        start = len(self.buf)
        self.send_serial(cmd)
        self.wait_for("prismos> ", timeout, "prompt after %r" % cmd)
        return self.buf[start:].decode("utf-8", "replace")

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


def make_disk():
    r = subprocess.run([sys.executable,
                        os.path.join(ROOT, "scripts", "make-disk.py")],
                       capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode != 0:
        print(r.stderr, end="")
        raise InstallerTestFailure("make-disk.py failed")


def install_flow(b, profile_key, profile_name):
    """Walk the Installer UI and install. profile_key is '1'
    (standard) or '2' (minimal). Ends with the Done screen."""
    b.sendkey("ctrl-i")
    b.wait_for("installer: opened", 20, "installer opened")
    print("ok: Installer app opens")
    time.sleep(0.8)
    w, h, raw = b.screendump("36-installer-%s.ppm" % profile_name)
    # The window is centred the same way the kernel draws it.
    win_w = w * 78 // 100
    win_x = (w - win_w) // 2
    avail = h - 48 - 44
    win_h = avail * 72 // 100
    win_y = (avail - win_h) // 2
    title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                             win_x + 300, win_y + 24)
    if title_n < 20:
        raise InstallerTestFailure("Installer title not on screen")
    b.sendkey("ret")
    b.wait_for("installer: step disk", 20, "installer disk step")
    print("ok: Installer disk step (partitions shown)")
    time.sleep(0.5)
    b.sendkey("a")  # automatic target
    b.sendkey("ret")
    b.wait_for("installer: step profile", 20, "installer profile step")
    print("ok: Installer profile step")
    b.sendkey(profile_key)
    time.sleep(0.5)
    b.sendkey("ret")
    b.wait_for("installer: step confirm", 20, "installer confirm step")
    print("ok: Installer confirm step (%s)" % profile_name)
    b.sendkey("y")
    b.wait_for("installer: install started", 30, "install started")
    b.wait_for("installer: done", 90, "install done")
    print("ok: Installer finished (%s)" % profile_name)
    time.sleep(0.5)
    w, h, raw = b.screendump("37-installer-done-%s.ppm" % profile_name)
    done_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                            win_x + 300, win_y + 24)
    if done_n < 20:
        raise InstallerTestFailure("Installer Done title not on screen")
    b.sendkey("r")  # reboot now
    b.wait_exit(20, "reboot after install")
    print("ok: rebooted after install")


def make_blank_disk():
    """A disk with no partitions at all (for the whole-disk test)."""
    try:
        os.unlink(DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", DISK, "16M"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise InstallerTestFailure("qemu-img failed: %s" % r.stderr)


def main():
    os.makedirs(SHOT_DIR, exist_ok=True)
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1

    try:
        # --- Standard install on a fresh bootable disk. ---
        make_disk()
        b = Boot(LOG, with_cd=True, boot_order="d")
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            b.wait_for("[boot] disk: data partition at LBA 67584", 30,
                       "data partition found")
            b.wait_for("prismos> ", 30, "shell prompt")
            install_flow(b, "1", "standard")
        finally:
            b.stop()

        # --- Boot the disk alone: standard install must be there. ---
        b = Boot(LOG, with_cd=False, boot_order="c")
        try:
            b.wait_for("[boot] disk: installed system found", 30,
                       "installed system found")
            b.wait_for("prismos> ", 30, "shell prompt")
            out = b.run_command("ls")
            for name in ("README.txt", "VERSION", "prism.bmp",
                         "melody.wav", "vid-01.pv1", "settings.txt"):
                if name not in out:
                    raise InstallerTestFailure(
                        "standard install missing %s (ls: %r)"
                        % (name, out))
            print("ok: standard install files on disk")
            out = b.run_command("cat settings.txt")
            if "install_profile=standard" not in out:
                raise InstallerTestFailure(
                    "settings.txt lacks install_profile=standard (%r)"
                    % out)
            print("ok: settings.txt says standard")
            out = b.run_command("cat VERSION")
            if "27H1" not in out:
                raise InstallerTestFailure("VERSION is not 27H1 (%r)"
                                           % out)
            print("ok: VERSION is 27H1 on the installed disk")
        finally:
            b.stop()

        # --- Minimal install on a fresh bootable disk. ---
        make_disk()
        b = Boot(LOG, with_cd=True, boot_order="d")
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            b.wait_for("prismos> ", 30, "shell prompt")
            install_flow(b, "2", "minimal")
        finally:
            b.stop()

        # --- Boot the disk alone: only core files must be there. ---
        b = Boot(LOG, with_cd=False, boot_order="c")
        try:
            b.wait_for("[boot] disk: installed system found", 30,
                       "installed system found")
            b.wait_for("[boot] desktop: minimal profile (4 apps)", 30,
                       "minimal profile at boot")
            print("ok: minimal profile active after reboot")
            b.wait_for("prismos> ", 30, "shell prompt")
            out = b.run_command("ls")
            for name in ("README.txt", "VERSION", "theme.toml",
                         "about.txt", "settings.txt"):
                if name not in out:
                    raise InstallerTestFailure(
                        "minimal install missing %s (ls: %r)" % (name, out))
            for name in ("prism.bmp", "melody.wav", "vid-01.pv1",
                         "vid-12.pv1"):
                if name in out:
                    raise InstallerTestFailure(
                        "minimal install must not contain %s (ls: %r)"
                        % (name, out))
            print("ok: minimal install has only core files")
            out = b.run_command("cat settings.txt")
            if "install_profile=minimal" not in out:
                raise InstallerTestFailure(
                    "settings.txt lacks install_profile=minimal (%r)"
                    % out)
            print("ok: settings.txt says minimal")
        finally:
            b.stop()

        # --- Whole-disk choice is refused honestly on a blank disk. ---
        # The installer cannot write the boot files (they live on the
        # install medium, which the OS cannot read), so 'w' must not
        # silently fall through to a data-partition install.
        make_blank_disk()
        b = Boot(LOG, with_cd=True, boot_order="d")
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.sendkey("ctrl-i")
            b.wait_for("installer: opened", 20, "installer opened")
            b.sendkey("ret")
            b.wait_for("installer: step disk", 20, "installer disk step")
            b.sendkey("w")
            b.wait_for("installer: whole-disk not available", 20,
                       "whole-disk refused")
            print("ok: whole-disk refused honestly on a blank disk")
        finally:
            b.stop()

        print("INSTALLER TEST PASSED")
        return 0
    except InstallerTestFailure as e:
        print("MISSING: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
