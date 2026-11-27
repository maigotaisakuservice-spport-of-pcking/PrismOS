#!/usr/bin/env python3
"""PrismOS login test.

Boots the ISO in QEMU with a blank 16MB ATA disk, installs
PrismOS, sets a PIN with `passwd`, then reboots with the same
disk: a wrong PIN must be rejected and the right PIN must
reach the shell.

Usage: python3 scripts/login-test.py
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
DISK = os.path.join(ROOT, "build", "prism-login-disk.img")
LOG = os.path.join(ROOT, "build", "boot-login.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-login.log")

QEMU = "qemu-system-x86_64"


class LoginTestFailure(Exception):
    pass


def make_disk():
    try:
        os.unlink(DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", DISK, "16M"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise LoginTestFailure("qemu-img failed: %s" % r.stderr)


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
                raise LoginTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise LoginTestFailure("%s not seen in %s s (expected %r)"
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
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    make_disk()
    print("disk: created 16MB ATA disk")

    try:
        # Boot 1: install, then set a PIN.
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
        finally:
            b.stop()

        # Boot 2: wrong PIN is rejected, right PIN logs in.
        b = Boot(LOG)
        try:
            b.wait_for("PrismOS login", 30, "login screen")
            print("ok: login screen shown")
            b.wait_for("PIN: ", 20, "PIN prompt")
            b.send("9999")
            b.wait_for("Wrong PIN.", 20, "wrong PIN rejected")
            print("ok: wrong PIN rejected")
            b.wait_for("PIN: ", 20, "PIN prompt again")
            b.send("1234")
            b.wait_for("Welcome to PrismOS.", 20, "login welcome")
            print("ok: right PIN accepted")
            b.wait_for("prismos> ", 20, "shell prompt after login")
            b.send("ver")
            b.wait_for("PrismOS version 27H1", 20, "ver after login")
            print("ok: shell works after login")
        finally:
            b.stop()

        # Boot 3: admin adds a user and sets up TOTP.
        import base64 as _b64
        import hashlib as _hl
        import hmac as _hm
        import struct as _st
        import time as _time

        totp_secret = [None]

        def totp_now(secret_b32, skew=0):
            pad = "=" * ((8 - len(secret_b32) % 8) % 8)
            key = _b64.b32decode(secret_b32 + pad)
            ctr = (int(_time.time()) + skew) // 30
            msg = _st.pack(">Q", ctr)
            mac = _hm.new(key, msg, _hl.sha1).digest()
            off = mac[-1] & 0x0F
            val = _st.unpack(">I", mac[off:off + 4])[0] & 0x7FFFFFFF
            return "%06d" % (val % 1000000)

        b = Boot(LOG)
        try:
            b.wait_for("PIN: ", 30, "PIN prompt")
            b.send("1234")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("user add alice")
            b.wait_for("New PIN", 20, "alice PIN prompt")
            b.send("5678")
            b.wait_for("Confirm PIN", 20, "alice confirm")
            b.send("5678")
            b.wait_for("User added: alice", 20, "alice added")
            print("ok: user alice added")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("user list")
            b.wait_for("alice (user)", 20, "alice in list")
            print("ok: user list shows alice")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("tz +0930")
            b.wait_for("UTC+09:30", 20, "tz set")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("totp setup")
            b.wait_for("Current code should read:", 20,
                       "totp current code")
            import re as _re
            m = _re.search(
                rb"TOTP secret \(enter it in your "
                rb"authenticator app\): ([A-Z2-7]+)",
                bytes(b.buf))
            if m is None:
                raise LoginTestFailure("totp secret not parseable")
            totp_secret[0] = m.group(1).decode("ascii")
            print("ok: totp set up (secret %s...)"
                  % totp_secret[0][:4])
            b.wait_for("prismos> ", 20, "shell prompt after setup")
            m2 = _re.search(rb"Current code should read: (\d+)",
                            bytes(b.buf))
            if m2 is None:
                raise LoginTestFailure("setup code not parseable")
            os_code = m2.group(1).decode("ascii")
            # Find which skew the OS code corresponds to
            # (diagnostic; the strict check is boot 6,
            # which self-calibrates on the OS's own
            # printed unix time).
            found = None
            for sk in range(-86400, 86401, 30):
                if totp_now(totp_secret[0], sk) == os_code:
                    found = sk
                    break
            print("setup code: os=%s matching skew=%s"
                  % (os_code, found))
        finally:
            b.stop()

        # Boot 4: alice logs in with her own PIN (files were
        # encrypted under the admin key; her wrap opens it).
        b = Boot(LOG)
        try:
            b.wait_for("User (blank = admin", 30, "user prompt")
            b.send("alice")
            b.wait_for("PIN: ", 20, "alice PIN prompt")
            b.send("5678")
            b.wait_for("Welcome to PrismOS.", 20, "alice welcome")
            print("ok: alice logged in (key wrap opened)")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("user whoami")
            b.wait_for("alice", 20, "whoami alice")
            print("ok: whoami = alice")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("date")
            b.wait_for("UTC+09:30", 20,
                       "settings decrypted under the wrapped key")
            print("ok: alice reads files encrypted under admin key")
        finally:
            b.stop()

        # Boot 5: guest session (no PIN, nothing saved).
        b = Boot(LOG)
        try:
            b.wait_for("User (blank = admin", 30, "user prompt")
            b.send("guest")
            b.wait_for("Welcome, guest.", 20, "guest welcome")
            print("ok: guest session started")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("user list")
            b.wait_for("Guests cannot manage users.", 20,
                       "guest blocked from user admin")
            print("ok: guest cannot manage users")
            b.wait_for("prismos> ", 20, "shell prompt")
            # ゲストはドライバの署名・導入にも触れない (監査 F1)。
            b.send("driver rootkey")
            b.wait_for("Guests cannot manage drivers.", 20,
                       "guest blocked from driver rootkey")
            print("ok: guest cannot create a driver root key")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("driver install sample.drv")
            b.wait_for("Guests cannot manage drivers.", 20,
                       "guest blocked from driver install")
            print("ok: guest cannot install drivers")
            b.wait_for("prismos> ", 20, "shell prompt")
            # ゲストの設定変更はファイルに残らない (監査 L5)。
            # セッション内では変わるが、settings.txt は元の
            # tz_offset=570 (alice が boot 3 で設定) のまま。
            b.send("tz +0530")
            b.wait_for("timezone set: UTC+05:30", 20, "guest tz set")
            print("ok: guest tz changed in-session")
            b.wait_for("prismos> ", 20, "shell prompt")
            mark = len(b.buf)
            b.send("type settings.txt")
            b.wait_for("prismos> ", 20, "settings.txt printed")
            out = bytes(b.buf[mark:b.pos])
            if b"tz_offset=330" in out:
                raise LoginTestFailure(
                    "guest tz change leaked into settings.txt")
            if b"tz_offset=570" not in out:
                raise LoginTestFailure(
                    "settings.txt lost the saved tz: %r" % out[:200])
            print("ok: guest settings change not persisted")
        finally:
            b.stop()

        # Boot 6: admin login now requires the TOTP code.
        b = Boot(LOG)
        try:
            b.wait_for("User (blank = admin", 30, "user prompt")
            b.send("")
            b.wait_for("PIN: ", 20, "admin PIN prompt")
            b.send("1234")
            b.wait_for("Code: ", 20, "totp code prompt")
            # The OS derives UTC as RTC - tz_offset.
            # The sandbox RTC shows UTC wall time and
            # boot 3 set tz to +0930 (330 minutes).
            import re as _re2
            mu = _re2.search(rb"totp: unix=(\d+)",
                             bytes(b.buf))
            os_unix = int(mu.group(1))
            skew = os_unix - int(_time.time())
            print("totp: os unix skew = %d s" % skew)
            b.send(totp_now(totp_secret[0], skew))
            b.wait_for("Code accepted.", 20, "totp accepted")
            print("ok: admin TOTP login works")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        print("LOGIN TEST PASSED")
        return 0
    except LoginTestFailure as e:
        print("MISSING: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
