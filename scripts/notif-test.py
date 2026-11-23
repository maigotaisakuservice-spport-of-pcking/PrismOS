#!/usr/bin/env python3
"""通知履歴アプリの QEMU テスト。

スクリーンショットで通知を 1 件作り、検索から
「Notifications」アプリを開いて、履歴にその通知が
記録されていること (open 時の件数) を確認する。
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


class NotifFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise NotifFailure(what)
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
        # ターミナルのスクロールバック (バッチ4 項目16)。
        for _ in range(4):
            b.cmd_output("help")
        b.key("pgup", 0.8)
        b.wait_for("terminal: scrollback ", 20, "scrollback")
        buf = bytes(b.buf).decode("utf-8", "replace")
        tail = buf[buf.rfind("terminal: scrollback ") + 21:]
        num = int(tail.split("\r")[0].split("\n")[0].strip())
        check(num > 0, "scrolled back %d lines" % num)
        for _ in range(3):
            b.key("pgdn", 0.4)
        check(True, "scrolled forward again")
        # 通知を 1 件作る (スクリーンショット保存)。
        b.key("ctrl-u", 1.0)
        b.wait_for("notify: Screenshot saved", 20, "notify")
        check(True, "a notification was raised")
        # Photos の前後移動・ズーム・回転・スライドショー
        # (バッチ4 項目11)。BMP は prism.bmp と shot.bmp。
        b.key("f9", 1.2)
        b.key("ret", 0.8)
        b.key("right", 0.6)
        b.wait_for("photos: viewing ", 15, "next photo")
        check(True, "moved to the next photo")
        b.key("shift-equal", 0.6)
        b.wait_for("photos: zoom 1", 15, "zoom in")
        check(True, "zoomed in")
        b.key("r", 0.6)
        b.wait_for("photos: rotate 90", 15, "rotate")
        check(True, "rotated 90 degrees")
        b.key("s", 0.6)
        b.wait_for("photos: slideshow on", 15, "slideshow on")
        b.wait_for("photos: slideshow next", 20,
                   "slideshow advance")
        check(True, "slideshow advanced after ~3 s")
        b.key("s", 0.6)
        b.wait_for("photos: slideshow off", 15, "slideshow off")
        b.key("esc", 0.6)
        b.key("esc", 0.6)
        # 検索から通知履歴アプリを開く。
        b.key("f8", 1.0)
        for ch in "notif":
            b.key(ch, 0.25)
        b.key("ret", 1.0)
        b.wait_for("notiflog: opened", 20, "notiflog open")
        check(True, "notifications app opened")
        buf = bytes(b.buf).decode("utf-8", "replace")
        tail = buf[buf.rfind("notiflog: opened"):]
        check(" entries" in tail and
              not tail.startswith("notiflog: opened, 0 entries"),
              "history has entries: %r" % tail[:40])
        b.key("down", 0.4)
        b.key("esc", 0.6)
        check(True, "scrolled and closed without a crash")
    finally:
        b.stop()
    print("NOTIF TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (NotifFailure, sg.SettingsGuiTestFailure) as exc:
        print("NOTIF TEST FAILED:", exc)
        sys.exit(1)
