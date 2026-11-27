#!/usr/bin/env python3
"""PrismOS 標準アプリ着脱テスト (バッチ4 項目17)。

- Store の端末内アプリビューで Calendar を取り外し、
  settings.txt の hidden_apps に残ること、再導入
  できることを確認する。
- ランチャーの右クリックメニューから Music を
  アンインストールできることを確認する。

使い方: python3 scripts/apps-mgmt-test.py
  (終了コード 0 = 合格。標準ライブラリのみ)
"""

import importlib.util
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def _load(name, fname):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


sg = _load("settings_gui_test", "settings-gui-test.py")
pm = _load("power_menu_test", "power-menu-test.py")


class AppsFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise AppsFailure(what)
    print("ok:", what)


def main():
    os.chdir(os.path.join(HERE, ".."))
    pm.make_disk()
    b = pm.Gui()
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

    b = pm.Gui()
    try:
        b.wait_for("PrismOS login", 40, "login screen")
        b.wait_for("PIN: ", 20, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 30, "admin login")

        # Store の端末内アプリビューで Calendar を着脱。
        b.key("ctrl-k", 1.2)
        b.key("a", 0.8)
        b.wait_for("store-ui: device apps", 20, "device apps view")
        check(True, "device apps view opened")
        for _ in range(5):
            b.key("down", 0.3)
        b.key("ret", 0.8)
        b.wait_for("apps: uninstalled Calendar", 20, "uninstall")
        check(True, "Calendar uninstalled from the Store")
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        check("hidden_apps=32" in out,
              "settings.txt has hidden_apps=32: %r" % out[:200])
        b.key("ctrl-k", 1.2)
        b.key("ret", 0.8)
        b.wait_for("apps: reinstalled Calendar", 20, "reinstall")
        check(True, "Calendar reinstalled from the Store")

        # ランチャーの右クリックから Music を外す。
        b.open_launcher()
        b.mouse_to(478, 358)  # Music のタイル (index 7、col 1, row 1; B2/B8 の新ジオメトリ)
        b.hmp("mouse_button 2")
        time.sleep(1.2)
        b.hmp("mouse_button 0")
        b.wait_for("menu: launcher", 20, "launcher menu")
        check(True, "launcher context menu opened")
        b.mouse_to(566, 400)  # 「アンインストール」の行
        b.mouse_click()
        b.wait_for("apps: uninstalled Music", 20, "rc uninstall")
        check(True, "Music uninstalled from the launcher menu")
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        check("hidden_apps=128" in out,
              "settings.txt has hidden_apps=128: %r" % out[:200])
        # ゲスト拒否の確認用に追加ユーザーを作る (ゲスト
        # ログインは users.txt があるときだけ選べる)。
        b.send("user add bob")
        b.wait_for("New PIN: ", 20, "new user PIN prompt")
        b.send("5678")
        b.wait_for("Confirm PIN: ", 20, "confirm PIN prompt")
        b.send("5678")
        b.wait_for("User added: bob", 20, "user added")
        check(True, "extra user added for the guest check")
        b.cmd_output("sync")
    finally:
        b.stop()

    # ゲストは GUI ストアを使えない (監査 M2: シェルの
    # store_flow と同じ入口ゲート)。
    b = pm.Gui()
    try:
        b.wait_for("User (blank = admin, or guest): ", 40,
                   "user prompt")
        b.send("guest")
        b.wait_for("Welcome, guest.", 30, "guest login")
        b.key("ctrl-k", 1.2)
        b.wait_for("store-ui: opened", 20, "store opened")
        b.key("r", 0.8)
        b.wait_for("store-ui: guest refused", 20,
                   "guest store refusal")
        check(True, "guest refused in the GUI store")
    finally:
        b.stop()
    print("APPS MGMT TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (sg.SettingsGuiTestFailure, AppsFailure) as e:
        print("APPS MGMT TEST FAILED:", e)
        sys.exit(1)
