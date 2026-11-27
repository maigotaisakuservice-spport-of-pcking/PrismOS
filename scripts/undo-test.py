#!/usr/bin/env python3
"""Docs / Sheet / Slides の Undo と検索置換の QEMU テスト。

Docs: "zzq" と入力 -> Ctrl+Z x3 で消える -> 保存して
doc.txt に残っていないことを確認。検索置換で
"Welcome" -> "Yokoso" に置換できることも確認。
Sheet: A1 に 5 を入れて確定 -> Ctrl+Z で取り消し ->
保存した sheet.csv に A1 が無いことを確認。
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


class UndoFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise UndoFailure(what)
    print("ok:", what)


def type_text(b, text):
    for ch in text:
        b.key("spc" if ch == " " else ch, 0.22)


def main():
    import os
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
        check(True, "installed with admin PIN")
        b.cmd_output("ver")
        b.cmd_output("sync")
    finally:
        b.stop()

    b = sg.Boot()
    try:
        b.wait_for("PrismOS login", 40, "login screen")
        b.wait_for("PIN: ", 20, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 30, "admin login")

        # --- Docs: 入力 -> Undo ---
        b.key("ctrl-d", 1.2)
        type_text(b, "zzq")
        for _ in range(3):
            b.key("ctrl-z", 0.5)
        b.wait_for("docs: undo", 20, "docs undo")
        check(True, "docs undo applied")
        b.key("f6", 0.8)
        b.wait_for("docs: saved", 20, "docs save")
        b.key("f1", 1.0)
        out = b.cmd_output("type doc.txt")
        check("zzq" not in out, "doc.txt has no zzq after undo")

        # --- Docs: 検索置換 ---
        b.key("ctrl-d", 1.2)
        b.key("ctrl-f", 0.6)
        type_text(b, "bold")
        b.key("ret", 0.5)
        type_text(b, "strong")
        b.key("ret", 0.6)
        b.wait_for("docs: replaced 1", 20, "docs replace")
        check(True, "docs find/replace replaced 1")
        b.key("f6", 0.8)
        b.wait_for("docs: saved", 20, "docs save 2")
        b.key("f1", 1.0)
        out = b.cmd_output("type doc.txt")
        check("strong" in out, "doc.txt contains strong")

        # --- Sheet: セル入力 -> Undo ---
        b.key("ctrl-o", 1.2)
        type_text(b, "5")
        b.key("ret", 0.6)   # A1 = 5 を確定
        b.key("ctrl-z", 0.6)
        b.wait_for("sheet: undo", 20, "sheet undo")
        check(True, "sheet undo applied")
        b.key("s", 0.8)   # Sheet の保存キーは 's'
        b.wait_for("sheet: saved", 20, "sheet save")
        b.key("f1", 1.0)
        out = b.cmd_output("type sheet.csv")
        check("A1=5" not in out,
              "sheet.csv has no A1=5 after undo: %r" % out[:60])

        # --- Sheet: 並べ替えとグラフ (バッチ4 項目24) ---
        b.key("ctrl-o", 1.2)
        type_text(b, "9")
        b.key("ret", 0.6)   # A1 = 9
        b.key("down", 0.4)
        type_text(b, "3")
        b.key("ret", 0.6)   # A2 = 3
        b.key("o", 0.8)     # 列 A を昇順に並べ替え
        b.wait_for("sheet: sorted by column A asc", 20, "sort")
        check(True, "sheet sorted ascending")
        b.key("g", 0.8)
        b.wait_for("sheet: chart on", 20, "chart on")
        check(True, "chart view opened")
        b.key("g", 0.8)
        b.wait_for("sheet: chart off", 20, "chart off")
        b.key("s", 0.8)
        b.wait_for("sheet: saved", 20, "sheet save 2")
        b.key("f1", 1.0)
        out = b.cmd_output("type sheet.csv")
        check("A1=3" in out and "A2=9" in out,
              "sorted order saved: %r" % out[:60])
        b.cmd_output("sync")
    finally:
        b.stop()
    print("UNDO TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (UndoFailure, sg.SettingsGuiTestFailure) as exc:
        print("UNDO TEST FAILED:", exc)
        sys.exit(1)
