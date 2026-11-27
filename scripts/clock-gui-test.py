#!/usr/bin/env python3
"""Clock settings test: the Settings clock page sets the
RTC (Enter applies the prefilled time) and the timezone
(arrows, saved to settings.txt as tz_offset)."""
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


class ClockGuiFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise ClockGuiFailure(what)
    print("ok:", what)


def main():
    os.chdir(os.path.join(HERE, ".."))
    sg.make_disk()
    b = sg.Boot()
    try:
        b.wait_for("prismos> ", 40, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 40, "install done")
        b.send("passwd")
        b.wait_for("New PIN (4-16 chars): ", 20, "new PIN prompt")
        b.send("1234")
        b.wait_for("Confirm PIN: ", 20, "confirm prompt")
        b.send("1234")
        b.wait_for("Account set.", 20, "PIN set")
        b.cmd_output("ver")
        b.cmd_output("sync")
        print("ok: installed with admin PIN")
    finally:
        b.stop()

    b = sg.Boot()
    try:
        b.wait_for("PrismOS login", 40, "login screen")
        b.wait_for("PIN: ", 20, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 30, "admin login")
        b.key("f3", 1.2)
        # 見た目ページで解像度を切り替える (項目8の確認)。
        b.key("r", 1.0)
        b.wait_for("settings: resolution 1024x768", 20,
                   "resolution change")
        check(True, "resolution changed from the GUI")
        b.key("r", 1.0)
        b.wait_for("settings: resolution 800x600", 20,
                   "resolution change 2")
        for _ in range(5):
            b.key("tab", 0.5)  # ページ5 = 時計
        # 日時行で Enter (プリフィル値をそのまま適用)。
        b.key("ret", 0.8)
        b.wait_for("settings: clock set", 20, "clock set")
        check(True, "clock set from the GUI")
        # タイムゾーン行で右へ (540 -> 570)。
        b.key("down", 0.5)
        b.key("right", 0.8)
        b.wait_for("settings: timezone set", 20, "tz set")
        check(True, "timezone changed from the GUI")
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        check("tz_offset=570" in out, "tz_offset=570 in settings.txt")
        # 音量 (項目7の確認): Music で '-' を2回 -> 80。
        b.key("f10", 1.2)
        b.key("minus", 0.7)
        b.wait_for("settings: volume 90", 20, "volume 90")
        b.key("minus", 0.7)
        b.wait_for("settings: volume 80", 20, "volume 80")
        check(True, "volume changed in Music")
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        check("volume=80" in out, "volume=80 in settings.txt")
        b.cmd_output("sync")
    finally:
        b.stop()

    # 再起動後、シェル側にも新しい tz が見える。
    b = sg.Boot()
    try:
        b.wait_for("PrismOS login", 40, "login screen")
        b.wait_for("PIN: ", 20, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 30, "admin login")
        b.key("f1", 1.0)
        out = b.cmd_output("tz")
        check("+09:30" in out,
              "after reboot the shell tz is +09:30: %r" % out[:80])
        # スクリーンショット (項目12)。
        b.key("ctrl-u", 1.0)
        b.wait_for("screenshot: saved shot.bmp", 20, "screenshot")
        check(True, "screenshot saved as shot.bmp")
        out = b.cmd_output("ls")
        check("shot.bmp" in out, "shot.bmp listed by ls")
        # 2 枚目も拡張子つきの名前になる (監査 L4)。
        b.key("ctrl-u", 1.0)
        b.wait_for("screenshot: saved shot1.bmp", 20,
                   "second screenshot")
        check(True, "second screenshot saved as shot1.bmp")
        out = b.cmd_output("ls")
        check("shot1.bmp" in out, "shot1.bmp listed by ls")
        # ネットワークページ: 固定IP設定 (バッチ4 未確認件)。
        b.key("f3", 1.2)
        for _ in range(6):
            b.key("tab", 0.4)
        b.key("right", 0.5)  # 方式: 固定IP
        for text in ("10.0.2.15", "255.255.255.0", "10.0.2.2",
                     "10.0.2.3"):
            b.key("down", 0.4)
            for ch in text:
                b.key("dot" if ch == "." else ch, 0.16)
        b.key("down", 0.4)   # 適用行
        b.key("ret", 0.6)    # PIN ダイアログ
        for ch in "1234":
            b.key(ch, 0.25)
        b.key("ret", 0.8)
        b.wait_for("settings: network static applied", 20,
                   "static applied")
        check(True, "static IP applied from the GUI")
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        check("net_mode=static" in out and
              "net_ip=10.0.2.15" in out,
              "settings.txt has the static config")
        # 入力デバイス設定 (バッチ4 項目26)。
        # 設定はページ6 (ネットワーク) を開いたままなので、
        # Tab 2 回 (管理ページ経由) でページ0 (見た目) に戻る。
        b.key("f3", 1.2)
        b.key("tab", 0.4)
        b.key("tab", 0.4)
        b.key("e", 0.5)
        b.wait_for("settings: mouse speed 150", 20, "mouse speed")
        b.key("i", 0.5)
        b.wait_for("settings: key repeat 2", 20, "key repeat")
        b.key("u", 0.5)
        b.wait_for("settings: dblclick 700", 20, "dblclick")
        check(True, "input device settings changed")
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        check("mouse_speed=150" in out and
              "key_repeat=fast" in out and
              "dblclick_ms=700" in out,
              "settings.txt has the input settings")
    finally:
        b.stop()
    print("CLOCK GUI TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ClockGuiFailure, sg.SettingsGuiTestFailure) as exc:
        print("CLOCK GUI TEST FAILED:", exc)
        sys.exit(1)
