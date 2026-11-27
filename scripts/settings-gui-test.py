#!/usr/bin/env python3
"""設定アプリ GUI テスト。

設定アプリの3つの新しいページを実機 (QEMU) で検証する:

  * アカウント: GUI でユーザー追加 (管理者 PIN ダイアログ経由)
    → シェルの `user list` に反映 → GUI で PIN 変更 → GUI で
    TOTP 設定 (users.txt に totp 行) → 再起動して追加ユーザーで
    ログインできる → 管理者で再起動して GUI でユーザー削除 →
    次の起動で単一アカウントのログインに戻る。
  * セキュリティ: USB ブロックを GUI で切り替えて保存 →
    settings.txt に usb_media=1 が出る。許可リストで music を
    拒否 → allowlist.txt から music が消える。
  * 更新: ローカルの署名済みサーバー (update-test と同じ
    www) に対して GUI で更新確認 → チャンネル切り替えが
    settings.txt に残る → 機能更新を GUI で適用 (VERSION が
    0.3.0 になる) → GUI でロールバック (VERSION が戻る)。
  * 一般ユーザー/ゲストではアカウント操作が拒否される
    (シリアルログで確認)。

シリアル (ログインとシェル) と QEMU モニタの sendkey
(デスクトップのキー入力) を組み合わせる。合否はシリアル
ログの実文字列で判定する。
"""

import base64
import hashlib
import hmac
import importlib.util
import os
import pty
import re
import struct
import socket
import subprocess
import sys
import termios
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ISO = os.path.join(ROOT, "build", "prismos.iso")
DISK = os.path.join(ROOT, "build", "test-disk-settings.img")
LOG = os.path.join(ROOT, "build", "boot-settings-gui.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-settings.log")
MON = os.path.join(ROOT, "build", "qemu-monitor-settings.sock")
QEMU = "qemu-system-x86_64"


def _load(name, fname):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rt = _load("recovery_test", "recovery-test.py")
upd = _load("update_test", "update-test.py")


class SettingsGuiTestFailure(Exception):
    pass


class Boot:
    """シリアル pty + モニタソケット付きの1回の起動。"""

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
        if os.path.exists(MON):
            os.unlink(MON)
        args = [QEMU, "-cdrom", ISO,
                "-drive",
                "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
                "-boot", "order=d", "-m", "256M",
                "-serial", "stdio",
                "-monitor", "unix:%s,server,nowait" % MON,
                "-display", "vnc=:9",
                "-no-reboot",
                "-d", "cpu_reset,int", "-D", DEBUG_LOG]
        self.proc = subprocess.Popen(
            args, stdin=slave_fd, stdout=slave_fd,
            stderr=subprocess.DEVNULL, close_fds=True)
        os.close(slave_fd)
        self.mon = None
        for _ in range(50):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5)
                s.connect(MON)
                self.mon = s
                break
            except OSError:
                time.sleep(0.2)
        if self.mon is None:
            raise SettingsGuiTestFailure("monitor socket not available")

    def _pump(self):
        import select
        r, _, _ = select.select([self.master_fd], [], [], 0.2)
        if not r:
            return
        try:
            data = os.read(self.master_fd, 65536)
        except OSError:
            data = b""
        if data:
            self.buf += data
            self.log_file.write(data)
            self.log_file.flush()

    def wait_for(self, needle, timeout, what):
        target = needle.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            idx = self.buf.find(target, self.pos)
            if idx >= 0:
                self.pos = idx + len(target)
                return
            self._pump()
        raise SettingsGuiTestFailure(
            "MISSING: %s not seen in %d s (expected %r); tail: %r" %
            (what, timeout, needle, bytes(self.buf[-400:])))

    def send(self, text):
        os.write(self.master_fd, (text + "\n").encode())
        time.sleep(0.3)

    def cmd_output(self, cmd, timeout=20):
        """シェルコマンドを送り、その出力を返す。

        コマンドのエコーが見えてから次のプロンプトまでを
        出力として切り取る (到着済みプロンプトとの競合回避)。
        呼び出し前にターミナル (F1) へ切り替えておくこと:
        デスクトップのアプリ表示中はシリアルの文字もアプリが
        消費する。"""
        self.send(cmd)
        self.wait_for(cmd, timeout, "echo of %s" % cmd)
        start = self.pos
        self.wait_for("prismos> ", timeout, "prompt after %s" % cmd)
        return bytes(self.buf[start:self.pos - 9]).decode(
            "utf-8", "replace")

    def hmp(self, cmd):
        self.mon.sendall((cmd + "\n").encode("ascii"))
        time.sleep(0.1)
        try:
            self.mon.recv(4096)
        except socket.timeout:
            pass

    def key(self, name, delay=0.35):
        self.hmp("sendkey %s" % name)
        time.sleep(delay)

    def type_gui(self, text):
        for ch in text:
            self.key(ch, 0.18)

    def stop(self):
        try:
            self.mon.close()
        except OSError:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        os.close(self.master_fd)
        self.log_file.close()


def make_disk():
    with open(DISK, "wb") as f:
        f.truncate(64 * 1024 * 1024)


def totp_now(secret_b32, skew=0):
    """login-test と同じ TOTP (RFC 6238, SHA-1, 30秒) 計算。"""
    pad = "=" * ((8 - len(secret_b32) % 8) % 8)
    key = base64.b32decode(secret_b32 + pad)
    ctr = (int(time.time()) + skew) // 30
    msg = struct.pack(">Q", ctr)
    mac = hmac.new(key, msg, hashlib.sha1).digest()
    off = mac[-1] & 0x0F
    val = struct.unpack(">I", mac[off:off + 4])[0] & 0x7FFFFFFF
    return "%06d" % (val % 1000000)


def pin_dialog(b, pin="1234"):
    """管理者 PIN ダイアログに PIN を入れて確定する。"""
    time.sleep(0.6)
    b.type_gui(pin)
    b.key("ret", 0.6)


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    make_disk()
    print("disk: created 64MB ATA disk")
    upd.make_www_030()
    rt.WWW = upd.WWW
    server = rt.HttpServer()
    server.start()
    try:
        # ---- 準備: インストール、更新サーバー、管理者 PIN ----
        b = Boot()
        try:
            b.wait_for("PrismOS 27H1", 40, "boot banner")
            b.wait_for("prismos> ", 40, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 40, "install done")
            print("ok: installed to disk")
            b.send("update server 10.0.2.2 /update-manifest.txt")
            b.wait_for("Update server: 10.0.2.2 /update-manifest.txt",
                       20, "update server set")
            b.send("passwd")
            b.wait_for("New PIN (4-16 chars): ", 20, "new PIN prompt")
            b.send("1234")
            b.wait_for("Confirm PIN: ", 20, "confirm prompt")
            b.send("1234")
            b.wait_for("Account set.", 20, "PIN set")
            print("ok: admin PIN set")
            out = b.cmd_output("type VERSION")
            m = re.search(r"(\d+\.\d+\.\d+[^\r\n]*)", out)
            base_version = m.group(1).strip() if m else ""
            print("ok: baseline VERSION = %r" % base_version)
        finally:
            b.stop()

        # ---- 本編: 管理者でログインして GUI を操作 ----
        b = Boot()
        try:
            b.wait_for("PrismOS login", 40, "login screen")
            b.wait_for("PIN: ", 20, "PIN prompt")
            b.send("1234")
            b.wait_for("Welcome to PrismOS.", 30, "admin login")
            print("ok: admin login")

            # アカウントページ: ユーザー追加。
            b.key("f3", 1.0)
            b.key("tab")
            b.key("tab")  # ページ2 = アカウント
            b.key("down")  # [ Add user ] 行
            b.key("ret", 0.8)
            b.type_gui("alice")
            b.key("tab")
            b.type_gui("5678")
            b.key("tab")
            b.type_gui("5678")
            b.key("tab")  # 役割 (一般のまま)
            b.key("tab")  # [ Add user ] ボタン
            b.key("ret")
            # まず間違った PIN は拒否される (監査 L1: 失敗が
            # 監査に残るようになった)。
            pin_dialog(b, "0000")
            b.wait_for("settings: admin PIN rejected", 20,
                       "wrong PIN rejected in dialog")
            print("ok: wrong admin PIN rejected in the dialog")
            pin_dialog(b)
            b.wait_for("settings: user added alice", 30,
                       "GUI user add")
            print("ok: user added from the GUI")
            b.key("f1", 1.0)  # シェルコマンドはターミナルで
            out = b.cmd_output("user list")
            if "alice (user)" not in out:
                raise SettingsGuiTestFailure(
                    "user list does not show alice: %r" % out)
            print("ok: shell user list shows alice")

            # アカウントページ: TOTP 設定。選択位置は保持され、
            # 追加直後は alice 行 (1) が選ばれている。
            # 設定のページ位置もアプリを離れても保持される。
            b.key("f3", 1.0)
            b.key("t")
            pin_dialog(b)
            b.wait_for("settings: TOTP set for alice", 30,
                       "GUI TOTP set")
            b.key("x")  # 秘密表示を閉じる
            print("ok: TOTP set from the GUI")
            b.key("f1", 1.0)
            out = b.cmd_output("type users.txt")
            if "totp alice " not in out:
                raise SettingsGuiTestFailure(
                    "users.txt has no totp line: %r" % out)
            alice_secret = re.search(
                r"totp alice ([A-Z2-7]+)", out).group(1)
            print("ok: users.txt gained the totp line")

            # アカウントページ: alice の PIN 変更 (選択は alice 行)。
            b.key("f3", 1.0)
            b.key("p", 0.8)
            b.type_gui("9999")
            b.key("tab")
            b.type_gui("9999")
            b.key("tab")  # [ Change PIN ] ボタン
            b.key("ret")
            pin_dialog(b)
            b.wait_for("settings: user PIN changed alice", 30,
                       "GUI PIN change")
            print("ok: PIN changed from the GUI")

            # セキュリティページ: USB ブロック + 保存。
            b.key("tab")  # ページ3 = セキュリティ
            b.key("right")  # USB: 許可 -> ブロック (下書き)
            for _ in range(5):
                b.key("down")
            b.key("ret")  # [ Save ]
            pin_dialog(b)
            b.wait_for("settings: security saved", 30,
                       "GUI security save")
            print("ok: security settings saved from the GUI")
            b.key("f1", 1.0)
            out = b.cmd_output("type settings.txt")
            if "usb_media=block" not in out:
                raise SettingsGuiTestFailure(
                    "settings.txt lacks usb_media=block: %r" % out)
            print("ok: settings.txt has usb_media=block")

            # セキュリティページ: 許可リストで music を拒否。
            # ページと選択位置は保持される (選択は保存行=5)。
            # music は行 6+7=13 (アプリ id 7) なので Down x8。
            b.key("f3", 1.0)
            for _ in range(8):
                b.key("down")
            b.key("spc")
            pin_dialog(b)
            b.wait_for("settings: app music denied", 30,
                       "GUI allowlist deny")
            print("ok: app denied from the GUI")
            b.key("f1", 1.0)
            out = b.cmd_output("type allowlist.txt")
            if "music" in out or "terminal" not in out:
                raise SettingsGuiTestFailure(
                    "allowlist.txt wrong after deny: %r" % out)
            print("ok: allowlist.txt reflects the deny")

            # USB を許可に戻して保存する (往復の確認)。メディア
            # 制限が有効な間は persist_load が利用者ファイルを
            # 再読込しない (シェルの rollback でも同じ既存挙動)
            # ため、更新の検証はメディア許可の状態で行う。
            b.key("f3", 1.0)
            for _ in range(13):
                b.key("up")
            b.key("right")  # USB: ブロック -> 許可 (下書き)
            for _ in range(5):
                b.key("down")
            b.key("ret")  # [ Save ]
            pin_dialog(b)
            b.wait_for("settings: security saved", 30,
                       "GUI security save back")
            b.key("f1", 1.0)
            out = b.cmd_output("type settings.txt")
            if "usb_media=allow" not in out:
                raise SettingsGuiTestFailure(
                    "settings.txt lacks usb_media=allow: %r" % out)
            print("ok: USB allowed again from the GUI")

            # 更新ページ: チャンネル切り替え (往復)。
            b.key("f3", 1.0)
            b.key("tab")  # ページ4 = 更新 (3から1つ)
            b.key("down")
            b.key("down")  # チャンネル行
            b.key("ret")
            pin_dialog(b)
            b.wait_for("settings: update channel beta", 30,
                       "GUI channel beta")
            b.key("f1", 1.0)
            out = b.cmd_output("type settings.txt")
            if "update_channel=beta" not in out:
                raise SettingsGuiTestFailure(
                    "settings.txt lacks update_channel=beta: %r" % out)
            print("ok: channel beta persisted")
            b.key("f3", 1.0)
            b.key("ret")  # 選択はチャンネル行 (2) のまま
            pin_dialog(b)
            b.wait_for("settings: update channel stable", 30,
                       "GUI channel stable")
            print("ok: channel back to stable")

            # 更新ページ: 確認 -> 適用 -> ロールバック。
            b.key("up")
            b.key("up")  # 確認行
            b.key("ret", 1.0)
            b.wait_for("settings: update check: available 0.3.0",
                       90, "GUI update check")
            print("ok: update check from the GUI")
            for _ in range(3):
                b.key("down")
            b.key("ret")  # 機能更新: 1回目で武装
            time.sleep(0.8)
            b.key("ret")  # 2回目で PIN ダイアログ
            pin_dialog(b)
            b.wait_for("settings: update applied", 120,
                       "GUI update apply")
            print("ok: feature update applied from the GUI")
            b.key("f1", 1.0)
            out = b.cmd_output("type VERSION")
            if "0.3.0" not in out:
                raise SettingsGuiTestFailure(
                    "VERSION not 0.3.0 after apply: %r" % out)
            print("ok: VERSION is 0.3.0 after GUI apply")
            b.key("f3", 1.0)
            b.key("down")  # ロールバック行 (選択は適用行=3)
            b.key("ret")  # 武装
            time.sleep(0.8)
            b.key("ret")  # PIN ダイアログ
            pin_dialog(b)
            b.wait_for("settings: rolled back to pre-update-0.3.0",
                       90, "GUI rollback")
            print("ok: rollback from the GUI")
            b.key("f1", 1.0)
            out = b.cmd_output("type VERSION")
            if "0.3.0" in out or base_version not in out:
                raise SettingsGuiTestFailure(
                    "VERSION not restored: %r" % out)
            print("ok: VERSION restored after GUI rollback")
        finally:
            b.stop()

        # ---- 追加ユーザーでログイン (再起動後も残る) ----
        b = Boot()
        try:
            b.wait_for("User (blank = admin, or guest): ", 40,
                       "user prompt")
            b.send("alice")
            b.wait_for("PIN: ", 20, "alice PIN prompt")
            b.send("9999")
            # alice は GUI で TOTP を有効化したのでコードが要る。
            b.wait_for("Code: ", 20, "alice totp prompt")
            mu = re.search(rb"totp: unix=(\d+)", bytes(b.buf))
            skew = int(mu.group(1)) - int(time.time())
            b.send(totp_now(alice_secret, skew))
            b.wait_for("Code accepted.", 20, "alice totp accepted")
            b.wait_for("Welcome to PrismOS.", 30, "alice login")
            print("ok: GUI-added user can log in after reboot")
            # 一般ユーザーはアカウント操作を拒否される。
            b.key("f3", 1.0)
            b.key("tab")
            b.key("tab")
            b.key("p")
            b.wait_for("settings: account action refused", 20,
                       "non-admin refusal")
            print("ok: non-admin refused on the accounts page")
        finally:
            b.stop()

        # ---- ゲストでも拒否される ----
        b = Boot()
        try:
            b.wait_for("User (blank = admin, or guest): ", 40,
                       "user prompt")
            b.send("guest")
            b.wait_for("Welcome, guest.", 30, "guest login")
            b.key("f3", 1.0)
            b.key("tab")
            b.key("tab")
            b.key("p")
            b.wait_for("settings: account action refused", 20,
                       "guest refusal")
            print("ok: guest refused on the accounts page")
        finally:
            b.stop()

        # ---- GUI でユーザー削除 → 単一アカウントに戻る ----
        b = Boot()
        try:
            b.wait_for("User (blank = admin, or guest): ", 40,
                       "user prompt")
            b.send("")  # 空 = 管理者
            b.wait_for("PIN: ", 20, "admin PIN prompt")
            b.send("1234")
            b.wait_for("Welcome to PrismOS.", 30, "admin login")
            b.key("f3", 1.0)
            b.key("tab")
            b.key("tab")
            b.key("down")  # alice 行
            b.key("d", 0.8)  # 削除確認
            b.key("ret")  # 確定 → PIN ダイアログ
            pin_dialog(b)
            b.wait_for("settings: user deleted alice", 30,
                       "GUI user delete")
            print("ok: user deleted from the GUI")
        finally:
            b.stop()

        b = Boot()
        try:
            b.wait_for("PrismOS login", 40, "login screen")
            b.wait_for("PIN: ", 20, "direct PIN prompt")
            b.send("1234")
            b.wait_for("Welcome to PrismOS.", 30, "admin login")
            print("ok: back to single-account login after delete")
            # 鍵のあるセッションでは許可リストが正しく読める
            # (拒否は music の1件だけ)。ゲスト (鍵なし) では
            # 暗号化された allowlist.txt を読めず全拒否に見える
            # のは既存の暗号化仕様。
            b.wait_for("allowlist denies 1 app(s)", 40,
                       "allowlist intact for admin")
            print("ok: allowlist still denies only music")
        finally:
            b.stop()
    finally:
        server.stop()
    print("SETTINGS GUI TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SettingsGuiTestFailure as e:
        print(str(e))
        print("SETTINGS GUI TEST FAILED")
        sys.exit(1)
