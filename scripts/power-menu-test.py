#!/usr/bin/env python3
"""電源メニュー テスト。

ランチャーの電源項目 (ロック / ユーザー切替 / スリープ /
再起動 / シャットダウン) を QEMU で検証する:

  * ロック: 画面がロックされ、間違った PIN では開かず、
    正しい PIN で解除できる。
  * ユーザー切替: ログイン画面へ戻り、別のユーザーで
    入れる (user whoami で確認)。
  * スリープ: S3 に入り、system_wakeup で復帰する。
  * 再起動: "Rebooting PrismOS..." が出て QEMU が終わる
    (-no-reboot のため再起動は終了として観測される)。
  * シャットダウン: QEMU が終了する。

マウスは desktop-test と同じ閉ループ方式 (スクリーン
ダンプでカーソルを探して寄せる)。
"""

import importlib.util
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def _load(name, fname):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


sg = _load("settings_gui_test", "settings-gui-test.py")
wp = _load("wallpaper_test", "wallpaper-test.py")

DISK = os.path.join(ROOT, "build", "test-disk-power.img")
SHOT = os.path.join(ROOT, "build", "power-test.ppm")
sg.DISK = DISK
sg.LOG = os.path.join(ROOT, "build", "boot-power-menu.log")
sg.MON = os.path.join(ROOT, "build", "qemu-monitor-power.sock")
sg.DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-power.log")


class PowerMenuTestFailure(Exception):
    pass


def px(raw, w, x, y):
    o = (y * w + x) * 3
    return raw[o], raw[o + 1], raw[o + 2]


def find_cursor(raw, w, h):
    def white(x, y):
        if x < 0 or y < 0 or x >= w or y >= h:
            return False
        r, g, b = px(raw, w, x, y)
        return r > 200 and g > 200 and b > 200

    for y in range(0, h - 6):
        for x in range(0, w - 6):
            if not white(x, y) or white(x + 1, y):
                continue
            ok = True
            for r in range(1, 6):
                for c in range(0, r + 1):
                    if not white(x + c, y + r):
                        ok = False
                        break
                if not ok or white(x + r + 1, y + r):
                    ok = False
                    break
            if ok:
                return (x, y)
    return None


class Gui(sg.Boot):
    def dump(self):
        self.hmp("screendump %s" % SHOT)
        time.sleep(0.5)
        return wp.read_ppm(SHOT)

    def mouse_move(self, dx, dy):
        self.hmp("mouse_move %d %d" % (dx, dy))
        time.sleep(0.3)

    def mouse_click(self):
        # 短い押しは CPU スロットリング下でゲストのポーリングに
        # 取りこぼされることがあるので、長めに押す。
        self.hmp("mouse_button 1")
        time.sleep(1.2)
        self.hmp("mouse_button 0")
        time.sleep(0.6)

    def mouse_to(self, x, y):
        for _ in range(12):
            w, h, raw = self.dump()
            pos = find_cursor(raw, w, h)
            if pos is None:
                self.mouse_move(8, 8)
                continue
            dx = x - pos[0]
            dy = y - pos[1]
            if abs(dx) <= 3 and abs(dy) <= 3:
                return
            self.mouse_move(max(-200, min(200, dx)),
                            max(-200, min(200, dy)))
        raise PowerMenuTestFailure("cursor did not reach (%d,%d)"
                                   % (x, y))

    def open_launcher(self):
        # セッション切替などを挟むとゲスト側のボタン押下
        # エッジ検出がずれて初回のクリックが食われることが
        # あるので、開くまで数回クリックする。
        for _ in range(3):
            self.mouse_to(134, 772)
            self.mouse_click()
            try:
                self.wait_for("launcher: opened", 8, "launcher opens")
                time.sleep(0.8)
                return
            except sg.SettingsGuiTestFailure:
                continue
        raise PowerMenuTestFailure("launcher did not open")

    def launcher_pick_from_end(self, back):
        """末尾から back 個戻った項目を選んで実行する。
        電源項目は末尾5つ (ロック/切替/スリープ/再起動/終了)。"""
        for _ in range(40):
            self.key("right", 0.12)
        for _ in range(back):
            self.key("left", 0.12)
        time.sleep(0.5)
        self.key("ret", 0.8)


def make_disk():
    with open(DISK, "wb") as f:
        f.truncate(64 * 1024 * 1024)


def login_admin(b):
    b.wait_for("User (blank = admin, or guest): ", 40, "user prompt")
    b.send("")
    b.wait_for("PIN: ", 20, "PIN prompt")
    b.send("1234")
    b.wait_for("Welcome to PrismOS.", 30, "admin login")
    b.wait_for("desktop: ready", 20, "desktop ready")
    time.sleep(1.5)


def main():
    if not os.path.exists(sg.ISO):
        print("MISSING: ISO not found: %s" % sg.ISO)
        return 1
    make_disk()

    b = Gui()
    try:
        b.wait_for("PrismOS 27H1", 40, "boot banner")
        b.wait_for("prismos> ", 40, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 40, "install done")
        b.send("passwd")
        b.wait_for("New PIN (4-16 chars): ", 20, "new PIN prompt")
        b.send("1234")
        b.wait_for("Confirm PIN: ", 20, "confirm prompt")
        b.send("1234")
        b.wait_for("Account set.", 20, "PIN set")
        # ロック画面の試行制限 (監査 M3) を確認するため、
        # ロックアウトを 2 回 / 300 秒に設定しておく。
        b.send("security lockout 2 300")
        b.wait_for("Admin PIN: ", 20, "admin PIN prompt")
        b.send("1234")
        b.wait_for("Lockout updated.", 20, "lockout updated")
        print("ok: lockout policy set to 2 attempts / 300 s")
        b.send("user add alice")
        b.wait_for("New PIN", 20, "alice PIN prompt")
        b.send("5678")
        b.wait_for("Confirm PIN", 20, "alice confirm")
        b.send("5678")
        b.wait_for("User added: alice", 20, "alice added")
        print("ok: installed, admin PIN and alice ready")
    finally:
        b.stop()

    # ロック -> 間違った PIN -> 正しい PIN で解除。
    b = Gui()
    try:
        login_admin(b)
        b.open_launcher()
        b.launcher_pick_from_end(4)  # ロック
        b.wait_for("desktop: locked", 20, "screen locked")
        print("ok: locked from the launcher")
        b.type_gui("0000")
        b.key("ret", 0.8)
        b.wait_for("desktop: unlock failed", 20, "wrong PIN refused")
        print("ok: wrong PIN refused on the lock screen")
        b.type_gui("1234")
        b.key("ret", 0.8)
        b.wait_for("desktop: unlocked", 20, "unlocked")
        print("ok: unlocked with the right PIN")

        # ロックアウト (監査 M3): 2 回連続で間違えると
        # ロックアウトし、正しい PIN でも待ち時間中は
        # 照合自体を拒否する。この後このブートはロック
        # されたまま終わる (待ち時間 300 秒)。
        b.open_launcher()
        b.launcher_pick_from_end(4)  # ロック
        b.wait_for("desktop: locked", 20, "screen locked again")
        b.type_gui("0000")
        b.key("ret", 0.8)
        b.wait_for("desktop: unlock failed", 20, "wrong PIN 1")
        b.type_gui("0000")
        b.key("ret", 0.8)
        b.wait_for("desktop: lock screen locked out", 20,
                   "lockout after 2 failures")
        print("ok: lock screen locks out after 2 wrong PINs")
        b.type_gui("1234")
        b.key("ret", 0.8)
        b.wait_for("desktop: unlock refused (lockout)", 20,
                   "correct PIN refused during lockout")
        print("ok: correct PIN refused during lockout")
    finally:
        b.stop()

    # ユーザー切替とスリープ (別ブート)。
    b = Gui()
    try:
        login_admin(b)
        # ユーザー切替 -> alice でログイン。
        b.open_launcher()
        b.launcher_pick_from_end(3)  # ユーザー切替
        b.wait_for("desktop: switch user", 20, "switch requested")
        b.wait_for("User (blank = admin, or guest): ", 40,
                   "back at the login screen")
        print("ok: back at the login screen")
        b.send("alice")
        b.wait_for("PIN: ", 20, "alice PIN prompt")
        b.send("5678")
        b.wait_for("Welcome to PrismOS.", 30, "alice login")
        print("ok: alice logged in after switch")
        out = b.cmd_output("user whoami")
        if "alice" not in out:
            raise PowerMenuTestFailure("whoami not alice: %r" % out)
        print("ok: session user is alice")

        # スリープ -> wakeup で復帰。復帰後はキーボードで
        # シェルが生きていることを確かめる (復帰後のマウス
        # パケット不達は既知の別課題なので、ここではマウスを
        # 使わない)。
        b.open_launcher()
        b.launcher_pick_from_end(2)  # スリープ
        b.wait_for("desktop: entering S3 sleep", 30, "S3 entry")
        time.sleep(2)
        b.hmp("system_wakeup")
        b.wait_for("desktop: resumed from S3", 30, "S3 resume")
        print("ok: slept and resumed from the launcher")
        out = b.cmd_output("ver")
        if "27H1" not in out:
            raise PowerMenuTestFailure("shell dead after resume")
        print("ok: shell alive after resume")
    finally:
        b.stop()

    # 再起動 (-no-reboot なので QEMU 終了として観測)。
    b = Gui()
    try:
        login_admin(b)
        b.open_launcher()
        b.launcher_pick_from_end(1)  # 再起動
        b.wait_for("Rebooting PrismOS...", 20, "reboot started")
        deadline = time.time() + 20
        while time.time() < deadline and b.proc.poll() is None:
            time.sleep(0.5)
        if b.proc.poll() is None:
            raise PowerMenuTestFailure("QEMU did not exit on reboot")
        print("ok: reboot from the launcher (QEMU exited)")
    finally:
        b.stop()

    # シャットダウン。
    b = Gui()
    try:
        login_admin(b)
        b.open_launcher()
        b.launcher_pick_from_end(0)  # シャットダウン
        b.wait_for("desktop: shutdown", 20, "shutdown started")
        deadline = time.time() + 20
        while time.time() < deadline and b.proc.poll() is None:
            time.sleep(0.5)
        if b.proc.poll() is None:
            raise PowerMenuTestFailure("QEMU did not exit on shutdown")
        print("ok: shutdown from the launcher (QEMU exited)")
    finally:
        b.stop()

    print("POWER MENU TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (PowerMenuTestFailure, sg.SettingsGuiTestFailure) as e:
        print(str(e))
        print("POWER MENU TEST FAILED")
        sys.exit(1)
