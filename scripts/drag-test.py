#!/usr/bin/env python3
"""PrismOS ドラッグ&ドロップ テスト (バッチ4 項目18)。

Files の一覧で:
  - prism.bmp を docs フォルダ行へドラッグして移動
  - doc.txt を trash フォルダ行へドラッグしてゴミ箱へ
をマウスで実行し、シリアルとシェルの ls で確認する。

使い方: python3 scripts/drag-test.py
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

TEXT_Y = 143  # settings-mouse-test と同じ実測値


class DragFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise DragFailure(what)
    print("ok:", what)


def root_rows(b):
    out = b.cmd_output("ls")
    rows = []
    for line in out.replace("\r", "").split("\n"):
        line = line.strip()
        if line and all(c.isalnum() or c in "._-/" for c in line):
            rows.append(line)
    files = sorted(r for r in rows if not r.endswith("/"))
    it = iter(files)
    return [r if r.endswith("/") else next(it) for r in rows]


def row_y(idx):
    # カーソルのホットスポットが数 px 下に出るため、
    # 行の上寄りを狙う。
    return TEXT_Y + 2 * 16 + idx * 16 + 2


def drag(b, src_idx, dst_idx):
    b.mouse_to(300, row_y(src_idx))
    b.hmp("mouse_button 1")
    time.sleep(0.6)
    b.mouse_to(300, row_y(dst_idx))
    time.sleep(0.4)
    b.hmp("mouse_button 0")
    time.sleep(0.6)


def main():
    os.chdir(os.path.join(HERE, ".."))
    # 電源テスト用ディスク (管理者 PIN 1234) でログインし、
    # 一覧が画面に収まるよう大きなサンプル群を消してから
    # 行の位置を測る (Files は先頭 ~28 行を表示する)。
    b = pm.Gui()
    try:
        b.wait_for("PrismOS login", 40, "login screen")
        try:
            b.wait_for("PIN: ", 8, "PIN prompt")
        except Exception:
            b.send("")
            b.wait_for("PIN: ", 20, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 30, "admin login")
        for i in range(1, 25):
            b.cmd_output("rm vid-%02d.pv1" % i)
        for name in ("loop.elf", "maindyn.elf", "unsup.elf",
                     "wc.elf", "evil.elf", "evil2.elf",
                     "evil3.elf", "libguest.so", "melody.wav",
                     "sample.drv"):
            b.cmd_output("rm " + name)
        b.cmd_output("mkdir docs")
        b.cmd_output("mkdir trash")
        # 前回分の後始末 (あれば戻す。無ければ失敗してよい)。
        b.cmd_output("mv docs/prism.bmp prism.bmp")
        b.cmd_output("mv trash/doc.txt doc.txt")
        rows = root_rows(b)
        bmp = rows.index("prism.bmp")
        docs = rows.index("docs/")
        doc = rows.index("doc.txt")
        trash = rows.index("trash/")

        b.key("f2", 1.2)  # Files
        drag(b, bmp, docs)
        b.wait_for("drag: moved prism.bmp -> docs/prism.bmp", 20,
                   "drag move")
        check(True, "dragged prism.bmp into docs/")
        b.key("f1", 1.0)
        b.cmd_output("cd docs")
        out = b.cmd_output("ls")
        check("prism.bmp" in out, "docs/ contains prism.bmp")
        b.cmd_output("cd ..")

        rows = root_rows(b)
        doc = rows.index("doc.txt")
        trash = rows.index("trash/")
        b.key("f2", 1.2)
        drag(b, doc, trash)
        b.wait_for("drag: trashed doc.txt", 20, "drag trash")
        check(True, "dragged doc.txt onto trash/")
        b.key("f1", 1.0)
        b.cmd_output("cd trash")
        out = b.cmd_output("ls")
        check("doc.txt" in out, "trash/ contains doc.txt")
        b.cmd_output("cd ..")
        b.cmd_output("sync")
    finally:
        b.stop()
    print("DRAG TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (sg.SettingsGuiTestFailure, DragFailure) as e:
        print("DRAG TEST FAILED:", e)
        sys.exit(1)
