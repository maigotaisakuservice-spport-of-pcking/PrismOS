#!/usr/bin/env python3
"""PrismOS 後付けドライバ機構の QEMU テスト。

  * 特権操作 (rootkey / sign / install / disable / enable) は
    管理者 PIN の対話確認が必須 (監査 F1 の回帰確認)。
    間違った PIN では rootkey が作られない
  * driver rootkey で信頼の根 (ML-DSA 鍵対) を作る
  * 署名なしのパッケージは導入を拒否される
  * driver sign (開発用署名) の後に driver install が通り、
    ドライバの init が API 経由で driver-hello.txt を書く
  * driver list に載る
  * driver disable の後は導入を拒否され、enable で戻る

使い方: python3 scripts/driver-install-test.py
  (終了コード 0 = 合格。標準ライブラリのみ)
"""

ADMIN_PIN = "1234"

import os
import pty
import select
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
LOG = os.path.join(ROOT, "build", "boot-driver-install.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-driver-install.log")

QEMU = "qemu-system-x86_64"


class DrvTestFailure(Exception):
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
        args = [QEMU, "-cdrom", ISO,
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
        deadline = time.monotonic() + timeout
        target = needle.encode()
        while True:
            idx = self.buf.find(target, self.pos)
            if idx >= 0:
                self.pos = idx + len(target)
                return
            if time.monotonic() > deadline:
                tail = bytes(self.buf[-3000:]).decode(errors="replace")
                raise DrvTestFailure(
                    "MISSING: %s (%r not seen in %d s)\n--- tail ---\n%s"
                    % (what, needle, timeout, tail))
            self.pump(0.25)

    def send(self, line):
        os.write(self.master_fd, line.encode() + b"\r")
        time.sleep(0.3)

    def send_admin(self, line, pin=ADMIN_PIN):
        """特権 driver コマンド: 管理者 PIN の対話確認に応じる。"""
        self.send(line)
        self.wait_for("Admin PIN: ", 20, "admin PIN prompt")
        self.send(pin)

    def stop(self):
        try:
            self.proc.kill()
        except OSError:
            pass
        self.log_file.close()


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    b = Boot(LOG)
    try:
        b.wait_for("prismos> ", 40, "shell prompt")
        print("ok: shell prompt")

        # 特権操作の前提として管理者 PIN を設定する。
        b.send("passwd")
        b.wait_for("New PIN", 20, "new PIN prompt")
        b.send(ADMIN_PIN)
        b.wait_for("Confirm PIN", 20, "confirm PIN prompt")
        b.send(ADMIN_PIN)
        b.wait_for("Account set", 20, "account set")
        print("ok: admin PIN set")
        b.wait_for("prismos> ", 20, "shell prompt")

        # 間違った PIN では rootkey を作れない (F1 回帰)。
        b.send_admin("driver rootkey", pin="9999")
        b.wait_for("Wrong PIN.", 20, "wrong admin PIN rejected")
        print("ok: wrong admin PIN rejected for rootkey")
        b.wait_for("prismos> ", 20, "shell prompt")

        # 署名も鍵も無い状態では導入できない。
        b.send_admin("driver install sample.drv")
        b.wait_for("driver: no root key", 20, "install without root key")
        print("ok: install refused without a root key")

        b.send_admin("driver rootkey")
        b.wait_for("driver: root key created", 30, "root key created")
        print("ok: root key created")

        # 鍵はあるが署名が無いパッケージは拒否される。
        b.send_admin("driver install sample.drv")
        b.wait_for("driver: refused (no signature)", 20,
                   "unsigned package refused")
        print("ok: unsigned package refused")

        # 開発用署名 → 導入が通り、ドライバが動く。
        b.send_admin("driver sign sample.drv")
        b.wait_for("driver: signed", 30, "package signed")
        b.send_admin("driver install sample.drv")
        b.wait_for("driver: loaded sample.drv", 30, "driver loaded")
        b.wait_for("driver: installed.", 20, "install reported")
        print("ok: signed driver installed and loaded")

        b.send("cat driver-hello.txt")
        b.wait_for("Hello from a loadable driver.", 20,
                   "driver wrote its file via the API")
        print("ok: driver init ran and wrote driver-hello.txt")

        b.send("driver list")
        b.wait_for("sample.drv", 20, "driver list shows the driver")
        print("ok: driver list")

        # 無効化すると導入を拒否される (回復手段)。
        b.send_admin("driver disable sample.drv")
        b.wait_for("driver: disabled", 20, "driver disabled")
        b.send_admin("driver install sample.drv")
        b.wait_for("driver: refused (disabled)", 20,
                   "disabled driver refused")
        print("ok: disabled driver refused")

        b.send_admin("driver enable sample.drv")
        b.wait_for("driver: enabled.", 20, "driver enabled")
        print("ok: driver re-enabled")
    except DrvTestFailure as e:
        print(str(e))
        print("DRIVER INSTALL TEST FAILED")
        return 1
    finally:
        b.stop()
    print("DRIVER INSTALL TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
