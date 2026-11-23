#!/usr/bin/env python3
"""PrismOS ウィジェットテスト (バッチ4 項目21)。

ホスト側にフィード用 HTTP サーバーを立て、設定の
ネットワークページで天気・ニュースのフィード URL を
登録し、ランチャーからウィジェットパネルを開いて
取得できることをシリアルで確認する。カレンダーは
RTC のローカルデータで描かれる (クラッシュしない
ことだけ確認)。

使い方: python3 scripts/widgets-test.py
  (終了コード 0 = 合格。標準ライブラリのみ)
"""

import functools
import http.server
import importlib.util
import os
import sys
import threading

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

PORT = 8393
FEED_DIR = "/tmp/prism-widgets"

failures = []


def check(cond, label):
    if cond:
        print("ok:", label)
    else:
        failures.append(label)
        print("FAIL:", label)


def type_url(b, text):
    names = {":": "shift-semicolon", "/": "slash", ".": "dot",
             "-": "minus", "_": "shift-minus"}
    for ch in text:
        b.key(names.get(ch, ch), 0.15)


def serve():
    os.makedirs(FEED_DIR, exist_ok=True)
    with open(os.path.join(FEED_DIR, "weather.txt"), "w") as f:
        f.write("Tokyo\nSunny, 24 degrees\n")
    with open(os.path.join(FEED_DIR, "news.txt"), "w") as f:
        f.write("PrismOS widgets work\nSecond headline here\n")
    handler = functools.partial(
        http.server.SimpleHTTPRequestHandler, directory=FEED_DIR)
    httpd = http.server.HTTPServer(("0.0.0.0", PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def main():
    pm.make_disk()
    httpd = serve()
    b = pm.Gui()
    try:
        b.wait_for("prismos> ", 60, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 60, "install done")
        b.send("passwd")
        b.wait_for("New PIN (4-16 chars): ", 20, "new PIN")
        b.send("1234")
        b.wait_for("Confirm PIN: ", 20, "confirm PIN")
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

        # 設定 → ネットワークページでフィード URL を登録。
        b.key("f3", 1.2)
        for _ in range(6):
            b.key("tab", 0.4)
        for _ in range(6):
            b.key("down", 0.3)
        type_url(b, "10.0.2.2:%d/weather.txt" % PORT)
        b.key("ret", 0.6)
        sg.pin_dialog(b)  # 保存は PIN 確認後 (監査 L2)
        b.wait_for("settings: feed url saved", 20, "weather save")
        check(True, "weather feed URL saved")
        b.key("down", 0.4)
        type_url(b, "10.0.2.2:%d/news.txt" % PORT)
        b.key("ret", 0.6)
        sg.pin_dialog(b)  # 保存は PIN 確認後 (監査 L2)
        b.wait_for("settings: feed url saved", 20, "news save")
        check(True, "news feed URL saved")
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        check("feed_weather=10.0.2.2:%d/weather.txt" % PORT in out,
              "settings.txt has the weather feed URL")
        check("feed_news=10.0.2.2:%d/news.txt" % PORT in out,
              "settings.txt has the news feed URL")

        # ランチャーからウィジェットを開く。
        # 項目位置: アプリ21 + 窓3 の次 (index 24) =
        # 4 列グリッドの col 0, row 6。
        b.open_launcher()
        b.mouse_to(400, 554)
        b.mouse_click()
        b.wait_for("widgets: weather ok", 30, "weather fetch")
        check(True, "weather feed fetched")
        b.wait_for("widgets: news ok", 20, "news fetch")
        check(True, "news feed fetched")
        b.wait_for("widgets: opened", 20, "widgets open")
        check(True, "widgets panel opened")
        b.key("esc", 0.6)
        b.wait_for("widgets: closed", 20, "widgets close")
        check(True, "widgets panel closed")
        b.cmd_output("sync")
    finally:
        b.stop()
        httpd.shutdown()
    if failures:
        print("WIDGETS TEST FAILED:", failures)
        return 1
    print("WIDGETS TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except sg.SettingsGuiTestFailure as e:
        print("WIDGETS TEST FAILED:", e)
        sys.exit(1)
