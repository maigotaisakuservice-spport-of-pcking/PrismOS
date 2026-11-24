#!/usr/bin/env python3
"""Screenshot test: デスクトップで Ctrl+U を押し、shot.bmp が
原寸 (1280x800 24 ビット BMP、3072054 バイト) で保存される
ことを確かめる。QEMU で撮ってから、ホスト側の検査ツール
(build/check_shot) でディスクイメージの中身を直接読む。
旧 144x90 形式なら寸法で失敗する。"""
import importlib.util
import os
import subprocess
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

DISK = os.path.join(HERE, "..", "build", "test-disk-shot.img")
CHECK = os.path.join(HERE, "..", "build", "check_shot")


class ScreenshotFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise ScreenshotFailure(what)
    print("ok:", what)


def main():
    os.chdir(os.path.join(HERE, ".."))
    sg.DISK = DISK
    sg.LOG = os.path.join(HERE, "..", "build", "boot-shot.log")
    sg.MON = os.path.join(HERE, "..", "build", "qemu-monitor-shot.sock")
    sg.DEBUG_LOG = os.path.join(HERE, "..", "build",
                                "qemu-debug-shot.log")
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
        # デスクトップでスクリーンショットを撮る。
        b.key("ctrl-u", 1.0)
        b.wait_for("screenshot: saved shot.bmp", 20, "screenshot")
        check(True, "screenshot saved as shot.bmp")
        b.key("f1", 1.0)
        b.cmd_output("sync")
        print("ok: synced to disk")
    finally:
        b.stop()

    # ホスト側でディスクイメージの中身を直接検査する。
    r = subprocess.run([CHECK, DISK], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    check(r.returncode == 0, "shot.bmp is a full-size BMP")
    print("SCREENSHOT TEST PASSED")


if __name__ == "__main__":
    main()
