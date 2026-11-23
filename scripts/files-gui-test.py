#!/usr/bin/env python3
"""Files GUI test: new file/folder, rename, copy, trash view
(restore / empty) driven from the keyboard in QEMU.

Boot 1 creates a PIN account. Boot 2 logs in and drives the
Files app: files created through the shell land last in the
VFS, so pressing Down many times selects them reliably.
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


class FilesTestFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise FilesTestFailure(what)
    print("ok:", what)


def login(b):
    b.wait_for("PrismOS login", 40, "login screen")
    b.wait_for("PIN: ", 20, "PIN prompt")
    b.send("1234")
    b.wait_for("Welcome to PrismOS.", 30, "admin login")


def to_last_row(b):
    for _ in range(60):
        b.key("down", 0.15)


def select_row(b, idx):
    for _ in range(60):
        b.key("up", 0.15)
    for _ in range(idx):
        b.key("down", 0.15)


KEYMAP = {".": "dot", "-": "minus", "_": "shift-minus"}


def type_name(b, name):
    for ch in name:
        b.key(KEYMAP.get(ch, ch), 0.06)


def root_rows(b):
    """GUI の行順を再現する: ls の走査順を土台に、
    ファイル行だけが名前順に並び替わる (フォルダ行は
    走査位置のまま)。"""
    out = b.cmd_output("ls")
    rows = []
    for line in out.replace("\r", "").split("\n"):
        line = line.strip()
        if line and all(c.isalnum() or c in "._-/" for c in line):
            rows.append(line)
    files = sorted(r for r in rows if not r.endswith("/"))
    it = iter(files)
    return [r if r.endswith("/") else next(it) for r in rows]


def clear_and_type(b, old_len, name):
    for _ in range(old_len + 2):
        b.key("backspace", 0.06)
    type_name(b, name)
    b.key("ret", 0.4)


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
        login(b)
        b.key("f1", 1.0)
        b.cmd_output("echo hello > target1.txt")
        rows = root_rows(b)
        check("target1.txt" in rows, "target file listed by ls")
        idx = rows.index("target1.txt")
        b.key("f2", 1.2)  # Files
        for _ in range(idx):
            b.key("down", 0.15)
        # 名前変更 (プリフィルを消してから入力)。
        b.key("r", 0.5)
        clear_and_type(b, len("target1.txt"), "renamed1.txt")
        b.wait_for("files: renamed target1.txt -> renamed1.txt",
                   20, "rename")
        check(True, "renamed from the Files app")
        # コピー (名前順になったので行位置から選ぶ)。
        b.key("f1", 1.0)
        rows = root_rows(b)
        b.key("f2", 1.0)
        select_row(b, rows.index("renamed1.txt"))
        b.key("c", 0.5)
        clear_and_type(b, len("renamed1.txt"), "copy1.txt")
        b.wait_for("files: copied renamed1.txt -> copy1.txt", 20,
                   "copy")
        check(True, "copied from the Files app")
        # 新規ファイルとフォルダ。
        b.key("n", 0.5)
        type_name(b, "fresh1.txt")
        b.key("ret", 0.4)
        b.wait_for("files: created fresh1.txt", 20, "new file")
        b.key("m", 0.5)
        type_name(b, "guifolder")
        b.key("ret", 0.4)
        b.wait_for("files: mkdir guifolder", 20, "new folder")
        check(True, "new file and folder created")
        # copy1.txt をゴミ箱へ (ls から行位置を計算)。
        b.key("f1", 1.0)
        rows = root_rows(b)
        check("copy1.txt" in rows, "copy listed by ls")
        b.key("f2", 1.0)
        select_row(b, rows.index("copy1.txt"))
        b.key("d", 0.5)
        b.wait_for("files: trashed copy1.txt", 20, "trash")
        check(True, "trashed from the Files app")
        # ゴミ箱へ移動 (trash/ の行を選ぶ)。
        b.key("f1", 1.0)
        rows = root_rows(b)
        check("trash/" in rows, "trash dir listed by ls")
        b.key("f2", 1.0)
        select_row(b, rows.index("trash/"))
        b.key("ret", 1.0)
        # 復元。
        b.key("down", 0.3)
        b.key("r", 0.5)
        b.wait_for("files: restored copy1.txt", 20, "restore")
        check(True, "restored from the trash view")
        # シェルで再度捨てて、ゴミ箱を空にする。
        b.key("f1", 1.0)
        b.cmd_output("trash copy1.txt")
        b.key("f2", 1.0)
        b.key("e", 0.5)
        b.key("y", 0.5)
        b.wait_for("files: trash emptied", 20, "empty trash")
        check(True, "trash emptied from the GUI")
        b.key("f1", 1.0)
        out = b.cmd_output("ls")
        check("renamed1.txt" in out and "fresh1.txt" in out and
              "copy1.txt" not in out,
              "shell sees the GUI results")
        b.cmd_output("sync")
    finally:
        b.stop()
    print("FILES GUI TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FilesTestFailure as exc:
        print("FILES GUI TEST FAILED:", exc)
        sys.exit(1)
