#!/usr/bin/env python3
"""PrismOS security test (phase 14).

Boots the ISO in QEMU and checks the security features end to
end, on real disks:

  * v2 accounts: `passwd` creates a SHA-256 KDF account (10000
    iterations, 16-byte salt); login with the right PIN works.
  * `security status` / `security usb|pin|lockout` (admin PIN
    required) and the audit log (`audit`, `audit verify`).
  * Lockout: 3 wrong PINs -> "Locked for 5 seconds.", then the
    right PIN works again after the wait.
  * USB/media block: after `security usb block` and a reboot the
    boot log says external media is blocked and `install yes`
    is refused.
  * v1 -> v2 migration: a v1 account file (FNV-1a, written by
    this script into the disk image) logs in once and is
    upgraded to v2 ("Account upgraded to the stronger PIN
    format."), and the next boot logs in with the v2 account.
  * Remote lock: `security remote setup` plus a local HTTP
    server serving a lock instruction with the right token
    locks the device at the next boot; the admin PIN unlocks
    it again.

Usage: python3 scripts/security-test.py
  (exit code 0 = passed; standard library only)
"""

import hashlib
import http.server
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import threading
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
DISK = os.path.join(ROOT, "build", "prism-security-disk.img")
DISK2 = os.path.join(ROOT, "build", "prism-security-disk2.img")
DISK3 = os.path.join(ROOT, "build", "prism-security-disk3.img")
LOG = os.path.join(ROOT, "build", "boot-security.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-security.log")
WWW = "/tmp/prism-remote-www"

QEMU = "qemu-system-x86_64"
PIN = "1234"


class SecurityTestFailure(Exception):
    pass


def make_disk(path):
    try:
        os.unlink(path)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", path, "16M"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SecurityTestFailure("qemu-img failed: %s" % r.stderr)


class Boot:
    def __init__(self, disk, log_path):
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
                "-drive", "file=%s,format=raw,if=ide,index=0,media=disk"
                % disk,
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
                raise SecurityTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise SecurityTestFailure("%s not seen in %s s (expected %r)"
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


def fnv1a(data):
    h = 14695981039346656037
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def patch_v1_account(disk_path):
    """Write a v1 .account file into the disk image.

    The disk was installed without a PIN, so all files are
    plaintext. The entry is written in the disk's own PRISMFS
    layout version (read from the superblock): version 2 has
    64 entries and data from sector 10; versions 3/4 have 128
    entries and data from sector 18/20. The start sector must
    be the head of the entry's own 32-sector slot — diskfs
    validates that alignment and refuses entries pointing
    into the middle of another file's slot.
    """
    salt = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88])
    h = fnv1a(salt + PIN.encode("ascii"))
    text = ("v1\nsalt=%s\nhash=%016x\n" % (salt.hex(), h)).encode("ascii")
    with open(disk_path, "r+b") as f:
        f.seek(512)
        sb = f.read(512)
        version = struct.unpack("<I", sb[8:12])[0]
        data_start = struct.unpack("<I", sb[16:20])[0]
        if version == 2:
            max_files, table_bytes = 64, 8 * 512
        else:
            max_files, table_bytes = 128, 16 * 512
        f.seek(2 * 512)
        table = bytearray(f.read(table_bytes))
        free_idx = None
        for i in range(max_files):
            if table[i * 64 + 40] == 0:
                free_idx = i
                break
        if free_idx is None:
            raise SecurityTestFailure("no free diskfs entry")
        start_sector = data_start + free_idx * 32
        entry = bytearray(64)
        entry[0:8] = b".account"
        struct.pack_into("<I", entry, 32, len(text))
        struct.pack_into("<I", entry, 36, start_sector)
        entry[40] = 1
        table[free_idx * 64:free_idx * 64 + 64] = entry
        f.seek(2 * 512)
        f.write(table)
        f.seek(start_sector * 512)
        f.write(text + b"\x00" * (16384 - len(text)))
        # Superblock file count at sector 1, offset 12.
        f.seek(512 + 12)
        count = struct.unpack("<I", f.read(4))[0]
        f.seek(512 + 12)
        f.write(struct.pack("<I", count + 1))
    print("ok: v1 .account patched into disk (slot %d)" % free_idx)


class RemoteServer:
    """Tiny HTTP server on 10.0.2.2:80 (the host side of SLIRP)."""

    def __init__(self, body):
        os.makedirs(WWW, exist_ok=True)
        with open(os.path.join(WWW, "prism-remote.txt"), "w") as f:
            f.write(body)
        handler = http.server.SimpleHTTPRequestHandler
        self.httpd = http.server.ThreadingHTTPServer(
            ("0.0.0.0", 80), handler)
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def _serve(self):
        os.chdir(WWW)
        self.httpd.serve_forever(poll_interval=0.1)

    def start(self):
        self.thread.start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def login(b, pin):
    b.wait_for("PrismOS login", 30, "login screen")
    b.wait_for("PIN: ", 20, "PIN prompt")
    b.send(pin)


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    make_disk(DISK)
    make_disk(DISK2)
    make_disk(DISK3)
    print("disk: created 3 x 16MB ATA disks")

    try:
        # ---- Disk 1: v2 account, security commands, audit ----
        b = Boot(DISK, LOG)
        try:
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            print("ok: installed to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("passwd")
            b.wait_for("New PIN", 20, "new PIN prompt")
            b.send(PIN)
            b.wait_for("Confirm PIN", 20, "confirm PIN prompt")
            b.send(PIN)
            b.wait_for("Account set", 20, "account set")
            print("ok: v2 PIN account set")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("security status")
            b.wait_for("External media: allow", 20, "status media")
            b.wait_for("PIN policy: min 4 chars", 20, "status PIN")
            b.wait_for("Lockout: 3 attempts, 300 seconds", 20,
                       "status lockout")
            b.wait_for("Allowlist: all apps allowed", 20, "status allow")
            b.wait_for("Remote lock/wipe: off", 20, "status remote")
            print("ok: security status shows defaults")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("audit verify")
            b.wait_for("chain OK.", 20, "audit chain OK")
            print("ok: audit chain OK after passwd")
        finally:
            b.stop()

        # ---- Boot 2: login, audit shows login-ok ----
        b = Boot(DISK, LOG)
        try:
            login(b, PIN)
            b.wait_for("Welcome to PrismOS.", 20, "login welcome")
            print("ok: v2 login works")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("audit")
            b.wait_for("login-ok", 20, "audit login-ok entry")
            print("ok: audit log has the login-ok entry")
            b.wait_for("prismos> ", 20, "shell prompt")
            # With the PIN key active, the mail password is
            # stored wrapped (pass_enc), never as plain text.
            b.send("mail setup 10.0.2.2 2525 10.0.2.2 1110 "
                   "prism secret prism@prismos.local")
            b.wait_for("mail: server configured.", 20,
                       "mail setup with PIN")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat mail.conf")
            b.wait_for("pass_enc=", 20, "wrapped password in mail.conf")
            if b"pass=secret" in bytes(b.buf):
                raise SecurityTestFailure(
                    "mail.conf holds the password in plain text")
            print("ok: mail password stored wrapped (pass_enc)")
            b.wait_for("prismos> ", 20, "shell prompt")
            # Lockout: 3 wrong PINs at the next boot lock for 5 s.
            b.send("security lockout 3 5")
            b.wait_for("Admin PIN: ", 20, "admin PIN prompt")
            b.send(PIN)
            b.wait_for("Lockout updated.", 20, "lockout updated")
            # Wait for the prompt: the settings sync to disk runs
            # before the next prompt is printed. Stopping earlier
            # can lose the change.
            b.wait_for("prismos> ", 20, "shell prompt")
            print("ok: lockout set to 3 attempts / 5 seconds")
        finally:
            b.stop()

        # ---- Boot 3: lockout ----
        b = Boot(DISK, LOG)
        try:
            b.wait_for("PrismOS login", 30, "login screen")
            for _ in range(3):
                b.wait_for("PIN: ", 20, "PIN prompt")
                b.send("9999")
                b.wait_for("Wrong PIN.", 20, "wrong PIN rejected")
            print("ok: 3 wrong PINs rejected")
            b.wait_for("Locked for 5 seconds.", 20, "lockout message")
            print("ok: lockout message shown")
            b.wait_for("PIN: ", 20, "PIN prompt after lockout")
            b.send(PIN)
            b.wait_for("Welcome to PrismOS.", 20, "login after lockout")
            print("ok: login works after the lockout wait")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("security lockout 3 300")
            b.wait_for("Admin PIN: ", 20, "admin PIN prompt")
            b.send(PIN)
            b.wait_for("Lockout updated.", 20, "lockout restored")
            # USB block: takes effect at the next boot.
            b.send("security usb block")
            b.wait_for("Admin PIN: ", 20, "admin PIN prompt")
            b.send(PIN)
            b.wait_for("External media: block (takes effect at next boot).",
                       20, "usb blocked")
            b.wait_for("prismos> ", 20, "shell prompt")
            print("ok: external media blocked")
        finally:
            b.stop()

        # ---- Boot 4: blocked media ----
        b = Boot(DISK, LOG)
        try:
            b.wait_for("external media is blocked", 30,
                       "blocked media boot message")
            print("ok: boot reports external media blocked")
            login(b, PIN)
            b.wait_for("Welcome to PrismOS.", 20, "login welcome")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("install yes")
            b.wait_for("Install failed: external media is blocked",
                       20, "install refused while blocked")
            print("ok: install refused while media is blocked")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("security usb allow")
            b.wait_for("Admin PIN: ", 20, "admin PIN prompt")
            b.send(PIN)
            b.wait_for("External media: allow (takes effect at next boot).",
                       20, "usb allowed again")
            b.wait_for("prismos> ", 20, "shell prompt")
            print("ok: external media allowed again")
        finally:
            b.stop()

        # ---- Boot 5: audit chain still OK after all of it ----
        b = Boot(DISK, LOG)
        try:
            login(b, PIN)
            b.wait_for("Welcome to PrismOS.", 20, "login welcome")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("audit verify")
            b.wait_for("chain OK.", 20, "audit chain OK at the end")
            print("ok: audit chain still OK")
        finally:
            b.stop()

        # ---- Disk 2: v1 -> v2 migration ----
        b = Boot(DISK2, LOG)
        try:
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            print("ok: disk 2 installed (no PIN yet)")
        finally:
            b.stop()
        patch_v1_account(DISK2)

        b = Boot(DISK2, LOG)
        try:
            login(b, PIN)
            b.wait_for("Welcome to PrismOS.", 20, "login welcome")
            b.wait_for("Account upgraded to the stronger PIN format.",
                       20, "migration message")
            print("ok: v1 account logged in and migrated to v2")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("audit")
            b.wait_for("account-migrate", 20, "audit migrate entry")
            print("ok: audit log has the account-migrate entry")
        finally:
            b.stop()

        b = Boot(DISK2, LOG)
        try:
            login(b, PIN)
            b.wait_for("Welcome to PrismOS.", 20, "v2 login after migrate")
            print("ok: login works with the migrated v2 account")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("audit verify")
            b.wait_for("chain OK.", 20, "audit chain OK on disk 2")
        finally:
            b.stop()

        # ---- Disk 3: remote lock ----
        token = hashlib.sha256((PIN + "remote-lock").encode("ascii")) \
            .hexdigest()
        server = RemoteServer("action=lock\ntoken=%s\n" % token)
        server.start()
        print("ok: remote instruction server started (lock, token set)")
        try:
            b = Boot(DISK3, LOG)
            try:
                b.wait_for("prismos> ", 30, "shell prompt")
                b.send("install yes")
                b.wait_for("Installed to disk", 30, "install done")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("passwd")
                b.wait_for("New PIN", 20, "new PIN prompt")
                b.send(PIN)
                b.wait_for("Confirm PIN", 20, "confirm PIN prompt")
                b.send(PIN)
                b.wait_for("Account set", 20, "account set")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("security remote setup 10.0.2.2 /prism-remote.txt")
                b.wait_for("Admin PIN: ", 20, "admin PIN prompt")
                b.send(PIN)
                b.wait_for("Remote lock/wipe: on (10.0.2.2/prism-remote.txt).",
                           20, "remote setup done")
                b.wait_for("prismos> ", 20, "shell prompt")
                print("ok: remote lock/wipe set up")
            finally:
                b.stop()

            b = Boot(DISK3, LOG)
            try:
                b.wait_for("This device was locked by the administrator.",
                           60, "remote lock at boot")
                print("ok: device locked by the remote instruction")
            finally:
                b.stop()

            b = Boot(DISK3, LOG)
            try:
                b.wait_for("This device is locked by the administrator.",
                           30, "locked screen")
                b.wait_for("PIN: ", 20, "unlock PIN prompt")
                b.send(PIN)
                b.wait_for("Unlocked.", 20, "unlock message")
                print("ok: admin PIN unlocks the device")
                login(b, PIN)
                b.wait_for("Welcome to PrismOS.", 20, "login after unlock")
                print("ok: login works after unlock")
            finally:
                b.stop()
        finally:
            server.stop()

        print("SECURITY TEST PASSED")
        return 0
    except SecurityTestFailure as e:
        print("MISSING: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
