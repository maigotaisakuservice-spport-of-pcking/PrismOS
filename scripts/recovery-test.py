#!/usr/bin/env python3
"""PrismOS recovery mode test.

Boots the ISO in QEMU with a blank 16MB ATA disk and checks the
recovery mode end to end:
  1. install, make a user file, take a snapshot, corrupt a built-in
     file, delete the user file, then `recovery` to reboot;
  2. recovery menu option 1 (repair keeps data) fixes the built-in
     file only; option 2 (restore) brings the user file back;
  3. option 3 (initialize) erases everything but the built-ins;
  4. option 4 downloads the previous version's manifest and
     repairs only the damaged built-in file (local HTTP server);
  5. pressing R during boot also enters recovery mode.

Usage: python3 scripts/recovery-test.py
  (exit code 0 = passed; standard library only)
"""

import hashlib
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
DISK = os.path.join(ROOT, "build", "prism-recovery-disk.img")
LOG = os.path.join(ROOT, "build", "boot-recovery.log")
WWW = "/tmp/prism-www-recovery"

QEMU = "qemu-system-x86_64"

README_ORIGINAL = (
    "PrismOS 27H1\n"
    "A customizable OS: modern centered UI, Dock and quick search,\n"
    "and a shell with two command styles.\n"
)
THEME_ORIGINAL = (
    "[theme]\n"
    "accent = \"#4CC2FF\"\n"
    "wallpaper = \"prism-dusk.png\"\n"
    "taskbar_position = \"bottom\"\n"
)


class RecoveryTestFailure(Exception):
    pass


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def make_disk():
    try:
        os.unlink(DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", DISK, "16M"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RecoveryTestFailure("qemu-img failed: %s" % r.stderr)


def make_www():
    os.makedirs(os.path.join(WWW, "prev"), exist_ok=True)
    # Previous version's files (the originals).
    with open(os.path.join(WWW, "prev", "theme.toml"), "w") as f:
        f.write(THEME_ORIGINAL)
    with open(os.path.join(WWW, "prev", "README.txt"), "w") as f:
        f.write(README_ORIGINAL)
    manifest = (
        "version=0.2.0\n"
        "channel=stable\n"
        "type=security\n"
        "base=/prev\n"
        "file README.txt %s\n"
        "file theme.toml %s\n"
        % (sha256_hex(README_ORIGINAL.encode()),
           sha256_hex(THEME_ORIGINAL.encode()))
    )
    with open(os.path.join(WWW, "update-manifest.txt.prev"), "w") as f:
        f.write(manifest)
    # 前バージョンマニフェストにも署名する (recovery.c が検証する)。
    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run(
        [sys.executable, os.path.join(here, "pq-sign.py"),
         os.path.join(WWW, "update-manifest.txt.prev")],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise RecoveryTestFailure("sign failed for .prev: %s" %
                                  r.stderr.strip())


class HttpServer:
    def __init__(self):
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "http.server", "80"],
            cwd=WWW, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None


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
                "-boot", "order=d",
                "-m", "256M", "-serial", "stdio", "-monitor", "none",
                "-display", "none", "-no-reboot",
                "-netdev", "user,id=net0",
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
                raise RecoveryTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise RecoveryTestFailure("%s not seen in %s s (expected %r)"
                                         % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def wait_for_re(self, pattern, timeout, what):
        # バイト列のまま探す。self.pos はバイト位置であり、
        # デコードした文字列の文字位置と混ぜると、日本語など
        # マルチバイト文字が出た途端にずれてしまう。
        rx = re.compile(pattern.encode("ascii"))
        deadline = time.monotonic() + timeout
        while True:
            m = rx.search(bytes(self.buf), self.pos)
            if m:
                self.pos = m.end()
                # 呼び出し側は group(1) を文字列として使う。
                class _M:
                    pass
                wrapped = _M()
                wrapped._m = m
                wrapped.group = lambda i: m.group(i).decode("ascii")
                return wrapped
            if self.proc.poll() is not None:
                raise RecoveryTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise RecoveryTestFailure("%s not seen in %s s (expected %r)"
                                         % (what, timeout, pattern))
            self.pump(min(0.5, deadline - time.monotonic()))

    def wait_for_exit(self, timeout, what):
        deadline = time.monotonic() + timeout
        while True:
            if self.proc.poll() is not None:
                return
            if time.monotonic() > deadline:
                raise RecoveryTestFailure("%s: QEMU did not exit in %s s"
                                         % (what, timeout))
            time.sleep(0.2)

    def send(self, text):
        os.write(self.master_fd, (text + "\r").encode("ascii"))

    def send_raw(self, text):
        os.write(self.master_fd, text.encode("ascii"))

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
    make_www()
    server = HttpServer()
    server.start()

    try:
        # Boot 1: install, snapshot, corrupt, delete, `recovery`.
        b = Boot(LOG)
        snap_name = None
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            print("ok: boot banner")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            print("ok: installed to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("echo my data > data.txt")
            b.wait_for("wrote 8 bytes to data.txt", 20, "echo write")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("snapshot")
            # 名前の全形 (snap-YYYYMMDDHHMMSS = 14 桁) を要求する。
            # 途中まで届いた文字列に先に一致するのを防ぐ。
            m = b.wait_for_re(r"Restore point created: (snap-\d{14})",
                              20, "snapshot created")
            snap_name = m.group(1)
            print("ok: snapshot created (%s)" % snap_name)
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("echo broken > README.txt")
            b.wait_for("wrote 7 bytes to README.txt", 20, "corrupt README")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("rm data.txt")
            b.wait_for("removed data.txt", 20, "rm file")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("recovery")
            b.wait_for("Rebooting into recovery mode", 20, "recovery reboot")
            b.wait_for_exit(20, "QEMU reset into recovery")
            print("ok: rebooted into recovery mode")
        finally:
            b.stop()

        # Boot 2: recovery menu — repair, then restore, then continue.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS Recovery", 30, "recovery menu")
            print("ok: recovery menu shown")
            b.send("1")
            b.wait_for("Repaired 1 file(s).", 20, "repair count")
            print("ok: repair fixed the built-in file")
            b.wait_for("Choice: ", 20, "menu again")
            b.send("2")
            b.wait_for(snap_name, 20, "snapshot listed")
            print("ok: restore point listed")
            b.wait_for("Restore which one", 20, "restore prompt")
            b.send("1")
            b.wait_for("Restored snapshot '%s'." % snap_name, 20, "restored")
            print("ok: restored from the restore point")
            b.wait_for("Choice: ", 20, "menu again")
            b.send("5")
            b.wait_for("prismos> ", 30, "shell prompt after recovery")
            b.send("cat data.txt")
            b.wait_for("my data", 20, "user file back")
            print("ok: user file is back after restore")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat README.txt")
            b.wait_for("PrismOS 27H1", 20, "built-in file repaired")
            print("ok: built-in file is correct")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("recovery")
            b.wait_for("Rebooting into recovery mode", 20, "recovery reboot")
            b.wait_for_exit(20, "QEMU reset into recovery")
            print("ok: rebooted into recovery mode again")
        finally:
            b.stop()

        # Boot 3: initialize, then corrupt theme.toml for option 4.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS Recovery", 30, "recovery menu")
            b.send("3")
            b.wait_for("Type YES to erase everything: ", 20, "init confirm")
            b.send("YES")
            b.wait_for("Initialized. Only built-in files remain.", 30,
                       "initialized")
            print("ok: initialized")
            b.wait_for("Choice: ", 20, "menu again")
            b.send("5")
            b.wait_for("prismos> ", 30, "shell prompt after initialize")
            mark = b.pos
            b.send("ls")
            b.wait_for("prismos> ", 20, "shell prompt after ls")
            seg = b.segment_since(mark)
            if b"data.txt" in seg:
                raise RecoveryTestFailure("user file survived initialize")
            if b"README.txt" not in seg:
                raise RecoveryTestFailure("built-in file missing after init")
            print("ok: only built-in files remain after initialize")
            b.send("echo garbage > theme.toml")
            b.wait_for("wrote 8 bytes to theme.toml", 20, "corrupt theme")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("update server 10.0.2.2 /update-manifest.txt")
            b.wait_for("Update server: 10.0.2.2 /update-manifest.txt", 20,
                       "update server set")
            print("ok: update server configured")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("recovery")
            b.wait_for("Rebooting into recovery mode", 20, "recovery reboot")
            b.wait_for_exit(20, "QEMU reset into recovery")
        finally:
            b.stop()

        # Boot 4: option 4 repairs only the damaged built-in file.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS Recovery", 30, "recovery menu")
            b.send("4")
            b.wait_for("Previous version: 0.2.0", 60, "prev manifest")
            b.wait_for("Repaired 1 damaged file(s) (2 checked).", 60,
                       "prev repair count")
            print("ok: previous version repaired only the damaged file")
            b.wait_for("Choice: ", 20, "menu again")
            b.send("5")
            b.wait_for("prismos> ", 30, "shell prompt after prev repair")
            b.send("cat theme.toml")
            b.wait_for('accent = "#4CC2FF"', 20, "theme.toml repaired")
            print("ok: theme.toml is back to the original")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # Boot 5: pressing R during boot enters recovery mode.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            b.send_raw("r")
            b.wait_for("PrismOS Recovery", 30, "recovery menu via R key")
            print("ok: R key enters recovery mode")
            b.send("5")
            b.wait_for("prismos> ", 30, "shell prompt after R-key recovery")
            print("ok: continued to normal boot")
        finally:
            b.stop()

        print("RECOVERY TEST PASSED")
        return 0
    except RecoveryTestFailure as e:
        print("MISSING: %s" % e)
        return 1
    finally:
        server.stop()


if __name__ == "__main__":
    sys.exit(main())
