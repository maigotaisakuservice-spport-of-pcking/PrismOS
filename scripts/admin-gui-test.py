#!/usr/bin/env python3
"""PrismOS 管理ページ テスト (バッチ4 項目22)。

設定アプリの管理ページで:
  - ファイアウォールに遮断ルールを GUI から追加
    (PIN 確認ダイアログ経由) し、シェルの fw list に
    反映されること
  - 復元ポイントを GUI から作成できること
  - FDE を GUI のパスフレーズフォーム + PIN 確認で
    有効化できること (回復キーが発行される)
を確認する。

使い方: python3 scripts/admin-gui-test.py
  (終了コード 0 = 合格。標準ライブラリのみ)
"""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def _load(name, fname):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


sg = _load("settings_gui_test", "settings-gui-test.py")


class AdminFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise AdminFailure(what)
    print("ok:", what)


def main():
    os.chdir(os.path.join(HERE, ".."))
    sg.make_disk()
    b = sg.Boot()
    try:
        b.wait_for("prismos> ", 60, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 60, "install")
        b.send("passwd")
        b.wait_for("New PIN (4-16 chars): ", 20, "new PIN prompt")
        b.send("1234")
        b.wait_for("Confirm PIN: ", 20, "confirm prompt")
        b.send("1234")
        b.wait_for("Account set.", 20, "account set")
        b.cmd_output("ver")
    finally:
        b.stop()

    b = sg.Boot()
    try:
        b.wait_for("PrismOS login", 40, "login screen")
        b.wait_for("PIN: ", 20, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 30, "admin login")
        # 素材: シェルで復元ポイント 1 件と遮断ルール 1 件。
        b.cmd_output("snapshot")
        b.cmd_output("fw add 10.9.9.9")

        # 設定 → 管理ページ (Tab x7)。
        b.key("f3", 1.2)
        for _ in range(7):
            b.key("tab", 0.4)
        # 右 x4 でファイアウォール分類へ。
        for _ in range(4):
            b.key("right", 0.35)
        # 追加行へ (ルール 1 件の下)。
        b.key("down", 0.4)
        for ch in "10.8.8.8":
            b.key("dot" if ch == "." else ch, 0.15)
        b.key("ret", 0.6)  # PIN ダイアログ
        b.type_gui("1234")
        b.key("ret", 0.8)
        b.wait_for("settings: firewall drop added 10.8.8.8", 30,
                   "fw add via GUI")
        check(True, "firewall rule added from the GUI")
        b.key("f1", 1.0)
        out = b.cmd_output("fw list")
        check("10.9.9.9" in out and "10.8.8.8" in out,
              "fw list shows shell + GUI rules: %r" % out[:80])

        # 復元ポイントを GUI から作成。
        b.key("f3", 1.2)
        for _ in range(3):
            b.key("left", 0.35)
        b.key("down", 0.4)  # 作成行 (既存 1 件の下)
        b.key("ret", 0.8)
        b.wait_for("settings: snapshot created ", 30, "snap create")
        check(True, "restore point created from the GUI")
        b.key("f1", 1.0)
        out = b.cmd_output("snapshot list")
        check(out.count("snap-") >= 2,
              "two restore points listed: %r" % out[:120])

        # FDE を GUI から有効化。
        b.key("f3", 1.2)
        b.key("left", 0.4)  # FDE 分類へ
        b.key("ret", 0.8)   # フォームを開く
        b.type_gui("diskpass123")
        b.key("tab", 0.4)
        b.type_gui("diskpass123")
        b.key("ret", 0.6)   # PIN ダイアログ
        b.type_gui("1234")
        b.key("ret", 0.8)
        b.wait_for("settings: fde enabled", 120, "fde enable")
        check(True, "FDE enabled from the GUI")
        # 有効化の過程で再起動が入るため、ここで終了する。
    finally:
        b.stop()
    print("ADMIN GUI TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (sg.SettingsGuiTestFailure, AdminFailure) as e:
        print("ADMIN GUI TEST FAILED:", e)
        sys.exit(1)
