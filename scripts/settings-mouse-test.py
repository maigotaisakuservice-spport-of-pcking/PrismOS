#!/usr/bin/env python3
"""設定アプリのマウス操作 (バッチ4 項目10) の QEMU テスト。

見た目ページでアクセント行とタスクバー行をクリックして
変更が効くこと、更新ページで行クリック→再クリックで
確認が実行されることをシリアルと settings.txt で確認。
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
pm = _load("power_menu_test", "power-menu-test.py")


class MouseSettingsFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise MouseSettingsFailure(what)
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
        b.key("f3", 1.2)
        # テーマカードの色チップ (バイオレット = 2 番目)。
        # カード UI では本文は x=401 から、テーマカードの
        # チップ行は y = 269..301、チップ 1 の中心は
        # (477, 285)。
        b.mouse_to(477, 285)
        b.mouse_click()
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        check("accent=#7C5CFF" in out,
              "accent changed by mouse click")
        # タスクバーの位置カードをクリック -> 上へ。
        # (カードは y = 401..469、x は本文側)
        b.key("f3", 1.2)
        b.mouse_to(600, 435)
        b.mouse_click()
        b.wait_for("settings: taskbar top", 20, "taskbar top")
        check(True, "taskbar moved by mouse click")
        # 更新ページ: 確認カードをクリック→再クリックで
        # 実行。(この流れではタスクバーが上でウィンドウが
        # 下がり、確認カードは実測 y = 382..447。x は本文側)
        for _ in range(4):
            b.key("tab", 0.4)
        b.mouse_to(600, 414)
        b.mouse_click()
        b.mouse_click()
        b.wait_for("settings: update check: no server", 30,
                   "update check by mouse")
        check(True, "update check ran from mouse clicks")
    finally:
        b.stop()
    print("SETTINGS MOUSE TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (MouseSettingsFailure, sg.SettingsGuiTestFailure,
            pm.PowerMenuTestFailure) as exc:
        print("SETTINGS MOUSE TEST FAILED:", exc)
        sys.exit(1)
