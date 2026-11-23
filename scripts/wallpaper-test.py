#!/usr/bin/env python3
"""壁紙テスト。

写真アプリで画像 (prism.bmp) を表示し、「壁紙に設定」が
できること、デスクトップの壁紙が実際にその画像へ変わる
こと、再起動後も保持されること、設定アプリから標準へ
戻せることを QEMU で検証する。画面の確認はスクリーン
ダンプ上部バンド (壁紙だけが描かれる帯) のピクセル
ハッシュ比較で行う。
"""

import hashlib
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

DISK = os.path.join(ROOT, "build", "test-disk-wallpaper.img")
SHOT = os.path.join(ROOT, "build", "wallpaper-test.ppm")
sg.DISK = DISK
sg.LOG = os.path.join(ROOT, "build", "boot-wallpaper.log")
sg.MON = os.path.join(ROOT, "build", "qemu-monitor-wallpaper.sock")
sg.DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-wallpaper.log")


class WallpaperTestFailure(Exception):
    pass


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 ヘッダ: マジック、幅、高さ、最大値 (空白区切り)。
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1  # 最大値の直後の空白1つ
    w = int(parts[1])
    h = int(parts[2])
    return w, h, data[i:i + w * h * 3]


def band_hash(b, rows=10, xmax=1000):
    """画面上部バンド左側のピクセルハッシュ (壁紙の指紋)。
    右上は通知トーストの表示域なので避ける。"""
    b.hmp("screendump %s" % SHOT)
    time.sleep(0.6)
    w, h, raw = read_ppm(SHOT)
    digest = hashlib.sha256()
    for y in range(rows):
        digest.update(raw[y * w * 3:y * w * 3 + xmax * 3])
    return w, digest.hexdigest(), raw


def count_near(raw, w, x0, y0, x1, y1, rgb, tol):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            o = (y * w + x) * 3
            if (abs(raw[o] - rgb[0]) <= tol and
                    abs(raw[o + 1] - rgb[1]) <= tol and
                    abs(raw[o + 2] - rgb[2]) <= tol):
                n += 1
    return n


def login_admin(b):
    b.wait_for("PrismOS login", 40, "login screen")
    b.wait_for("PIN: ", 20, "PIN prompt")
    b.send("1234")
    b.wait_for("Welcome to PrismOS.", 30, "admin login")
    b.wait_for("desktop: ready", 20, "desktop ready")
    time.sleep(1.5)


def main():
    if not os.path.exists(sg.ISO):
        print("MISSING: ISO not found: %s" % sg.ISO)
        return 1
    with open(DISK, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    print("disk: created 64MB ATA disk")

    b = sg.Boot()
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
        # 次のプロンプト周期で .account がディスクへ同期される
        # のを待ってから止める。
        out = b.cmd_output("ver")
        if "27H1" not in out:
            raise WallpaperTestFailure("ver failed: %r" % out)
        print("ok: installed, admin PIN set")
    finally:
        b.stop()

    # 標準壁紙の指紋を取り、写真から自分の画像に変える。
    b = sg.Boot()
    try:
        login_admin(b)
        w, h_std, raw = band_hash(b)
        n = count_near(raw, w, 1260, 0, 1280, 10, (55, 42, 105), 30)
        if n < 10:
            raise WallpaperTestFailure(
                "standard wallpaper not on screen (%d)" % n)
        print("ok: standard wallpaper on screen")
        b.key("f9", 1.2)  # 写真アプリ
        b.key("ret", 1.0)  # prism.bmp を開く
        b.key("w", 1.0)  # 壁紙に設定
        b.wait_for("photos: wallpaper set to prism.bmp", 20,
                   "wallpaper set from Photos")
        print("ok: wallpaper set from Photos")
        b.key("esc", 0.8)  # 一覧へ
        b.key("esc", 0.8)  # ターミナルへ
        time.sleep(4)  # 通知トーストが消えるのを待つ
        _, h_custom, _ = band_hash(b)
        if h_custom == h_std:
            raise WallpaperTestFailure(
                "wallpaper did not change after setting")
        print("ok: desktop wallpaper changed to the photo")
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        if "wallpaper=custom" not in out or \
                "wallpaper_file=prism.bmp" not in out:
            raise WallpaperTestFailure(
                "settings.txt lacks the custom wallpaper: %r" % out)
        print("ok: settings.txt records the custom wallpaper")
    finally:
        b.stop()

    # 再起動しても自分の壁紙のまま。設定から標準へ戻す。
    b = sg.Boot()
    try:
        login_admin(b)
        _, h_again, _ = band_hash(b)
        if h_again != h_custom:
            raise WallpaperTestFailure(
                "custom wallpaper not kept after reboot")
        print("ok: custom wallpaper kept after reboot")
        b.key("f3", 1.0)  # 設定 (見た目ページ)
        b.key("shift-w", 0.8)  # 標準に戻す
        b.wait_for("settings: wallpaper reset to standard", 20,
                   "wallpaper reset")
        b.key("s", 0.8)  # 保存
        time.sleep(1.0)
        b.key("f1", 1.0)
        out = b.cmd_output("type settings.txt")
        if "wallpaper=image" not in out:
            raise WallpaperTestFailure(
                "settings.txt not back to image: %r" % out)
        print("ok: wallpaper reset to standard in Settings")
    finally:
        b.stop()

    # 再起動すると標準壁紙に戻っている。
    b = sg.Boot()
    try:
        login_admin(b)
        _, h_back, _ = band_hash(b)
        if h_back != h_std:
            raise WallpaperTestFailure(
                "standard wallpaper not restored after reboot")
        print("ok: standard wallpaper restored after reboot")
    finally:
        b.stop()

    print("WALLPAPER TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (WallpaperTestFailure, sg.SettingsGuiTestFailure) as e:
        print(str(e))
        print("WALLPAPER TEST FAILED")
        sys.exit(1)
