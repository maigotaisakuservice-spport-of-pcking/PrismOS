#!/usr/bin/env python3
"""Wi-Fi UX GUI テスト。

QEMU には Wi-Fi デバイスが無いため、実接続ではなく
「ハードウェアなしの正直な表示」と操作系を検証する:

  * 設定 > ネットワーク: Wi-Fi トグルのオン/オフが
    シリアルログと wifi.txt (PRISMWIFI1 ヘッダ) に残る。
  * 「その他のネットワーク」からダイアログが開き、
    SSID/パスワードを入れて接続すると、ハードウェア
    なしの理由で失敗する (rc=-1) — 接続できたフリを
    しないことの確認。
  * タスクバーのトレイアイコン (実測 x=1152) で
    クイックパネルが開閉し、Esc でも閉じる。
  * パネルのフッターから設定のネットワークページへ
    遷移する (遷移先でトグル操作が効くことで確認)。

状態機械そのもの (スキャン/接続/認証失敗/保存往復) は
ホストテスト tests/test_wifi.c が担う。
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


pm = _load("power_menu_test", "power-menu-test.py")
sg = pm.sg

DISK = os.path.join(ROOT, "build", "test-disk-wifi.img")
sg.DISK = DISK
sg.LOG = os.path.join(ROOT, "build", "boot-wifi-gui.log")
sg.MON = os.path.join(ROOT, "build", "qemu-monitor-wifi.sock")
sg.DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-wifi.log")
pm.SHOT = os.path.join(ROOT, "build", "wifi-gui-test.ppm")

Fail = sg.SettingsGuiTestFailure

# トレイアイコンの中心 (1280x800 実測: バー左端 1141、
# ボタン 1132..1172 の中心)。
ICON_X = 1152
ICON_Y = 776
# パネルのフッター行 (ネットワーク設定を開く) の中心。
# パネルは x=912..1272、h=238 (メッセージなし時) で
# y=506..744、最終行は y=692..732。
FOOTER_X = 1092
FOOTER_Y = 712


class Gui(pm.Gui):
    def type_gui(self, text):
        for ch in text:
            if ch.isupper():
                self.key("shift-" + ch.lower(), 0.18)
            else:
                self.key(ch, 0.18)


def make_disk():
    with open(DISK, "wb") as f:
        f.truncate(64 * 1024 * 1024)


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
        b.cmd_output("type VERSION")
        time.sleep(1.0)
        print("ok: installed + PIN set")
    finally:
        b.stop()

    b = Gui()
    try:
        b.wait_for("PrismOS login", 40, "login screen")
        b.wait_for("PIN: ", 20, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 30, "admin login")
        b.wait_for("desktop: ready", 20, "desktop ready")
        print("ok: admin login")

        # ---- 設定: ネットワークページの Wi-Fi トグル ----
        b.key("f3", 1.0)
        for _ in range(6):
            b.key("tab")
        time.sleep(1.0)
        for _ in range(8):
            b.key("down")
        b.key("right")
        b.wait_for("wifi: disabled", 20, "toggle off logged")
        print("ok: Wi-Fi toggled off from settings")
        b.key("right")
        b.wait_for("wifi: enabled", 20, "toggle on logged")
        print("ok: Wi-Fi toggled on from settings")

        # ---- その他のネットワーク → ダイアログ → 失敗 ----
        b.key("down")  # その他のネットワーク行
        b.key("ret", 0.8)
        b.wait_for("wifi: connect dialog opened", 20, "dialog opens")
        b.type_gui("HomeNet")
        b.key("tab")
        b.type_gui("himitsu123")
        b.key("ret", 0.8)
        b.wait_for("wifi: connect failed rc=-1", 20,
                   "no-hardware failure reported")
        print("ok: connect honestly fails with no hardware")
        b.key("esc", 0.8)

        # ---- 永続化: wifi.txt に有効状態が残る ----
        b.key("f1", 1.0)
        out = b.cmd_output("type wifi.txt")
        if "PRISMWIFI1 1" not in out:
            raise Fail("wifi.txt lacks enabled header: %r" % out)
        if "HomeNet" in out:
            raise Fail("failed network must not be saved: %r" % out)
        print("ok: wifi.txt persists the enabled flag only")

        # ---- クイックパネル: 開閉 (アイコン再クリック) ----
        b.mouse_to(ICON_X, ICON_Y)
        b.mouse_click()
        b.wait_for("wifi: quick panel opened", 20, "panel opens")
        print("ok: quick panel opened from the tray icon")
        b.mouse_to(ICON_X, ICON_Y)
        b.mouse_click()
        b.wait_for("wifi: quick panel closed", 20, "panel closes")
        print("ok: quick panel closed by re-clicking the icon")

        # ---- クイックパネル: Esc で閉じる ----
        b.mouse_to(ICON_X, ICON_Y)
        b.mouse_click()
        b.wait_for("wifi: quick panel opened", 20, "panel reopens")
        b.key("esc", 0.8)
        b.wait_for("wifi: quick panel closed", 20, "panel Esc close")
        print("ok: quick panel closed with Esc")

        # ---- パネル → 設定 (ネットワークページ) へ遷移 ----
        b.mouse_to(ICON_X, ICON_Y)
        b.mouse_click()
        b.wait_for("wifi: quick panel opened", 20, "panel opens 3rd")
        jumped = False
        for _ in range(3):
            b.mouse_to(FOOTER_X, FOOTER_Y)
            b.mouse_click()
            try:
                b.wait_for("wifi: open network settings", 8,
                           "footer jump logged")
                jumped = True
                break
            except Fail:
                continue
        if not jumped:
            raise Fail("panel footer did not jump to settings")
        print("ok: panel footer jumps to Settings > Network")
        time.sleep(1.0)
        # 遷移先がネットワークページなら、行移動 + トグルが
        # 効く。net_row は 9 (その他のネットワーク) のはず
        # なので Down x9 で行 8 (トグル) へ回る。
        for _ in range(9):
            b.key("down")
        b.key("right")
        b.wait_for("wifi: disabled", 20, "toggle works after jump")
        print("ok: network page is active after the jump")
        b.key("right")
        b.wait_for("wifi: enabled", 20, "toggle back on")
    finally:
        b.stop()
    print("WIFI GUI TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print(str(e))
        print("WIFI GUI TEST FAILED")
        sys.exit(1)
