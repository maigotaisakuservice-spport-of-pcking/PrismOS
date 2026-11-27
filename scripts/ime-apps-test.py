#!/usr/bin/env python3
"""IME test: romaji->kana input works in the Editor, Docs,
Mail compose and the search box, not only in Notes.

Boot 1 installs and sets the admin PIN. Boot 2 logs in and
types romaji with the IME on (F9) in each app; committed
text is verified on the serial log ("ime: commit") and, for
the Editor and Docs, in the saved files read back through
the shell.
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


class ImeTestFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise ImeTestFailure(what)
    print("ok:", what)


def type_romaji(b, text):
    for ch in text:
        b.key("spc" if ch == " " else ch, 0.22)


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

        # エディタ: F9 で IME オン、"neko" -> ねこ。
        b.key("ctrl-t", 1.2)
        b.key("f9", 0.8)
        type_romaji(b, "neko")
        b.key("ret", 0.6)
        b.wait_for("ime: commit", 20, "editor IME commit")
        check(True, "IME commit in the Editor")
        b.key("f6", 0.8)
        b.wait_for("editor: saved", 20, "editor save")
        b.key("f1", 1.0)
        out = b.cmd_output("type edit.txt")
        check("ねこ" in out, "edit.txt contains the kana")

        # Docs: IME はオンのまま。"inu" -> いぬ。
        b.key("ctrl-d", 1.2)
        type_romaji(b, "inu")
        b.key("ret", 0.6)
        b.wait_for("ime: commit", 20, "docs IME commit")
        check(True, "IME commit in Docs")
        b.key("f6", 0.8)
        b.wait_for("docs: saved doc.txt", 20, "docs save")
        b.key("f1", 1.0)
        out = b.cmd_output("type doc.txt")
        check("いぬ" in out, "doc.txt contains the kana")

        # メール作成: 初回は設定フォームを埋めてから。
        b.key("ctrl-m", 1.2)
        b.wait_for("mail: setup form opened", 20, "mail setup form")
        for ch in "me@example.jp":
            b.key({"@": "shift-2", ".": "dot"}.get(ch, ch), 0.16)
        b.key("tab", 0.4)
        for ch in "smtp.example.jp":
            b.key({"@": "shift-2", ".": "dot"}.get(ch, ch), 0.16)
        for _ in range(11):
            b.key("tab", 0.3)
        b.key("ret", 0.8)
        b.wait_for("mail: setup saved", 20, "mail setup saved")
        b.key("c", 0.8)      # 作成開始
        b.key("tab", 0.5)    # 件名フィールドへ
        type_romaji(b, "tegami")
        b.key("ret", 0.6)
        b.wait_for("ime: commit", 20, "mail IME commit")
        check(True, "IME commit in Mail compose")
        b.key("esc", 0.6)

        # 検索欄: F8 で開き、F9 状態のまま入力。
        b.key("f8", 1.0)
        type_romaji(b, "neko")
        b.key("ret", 0.6)
        b.wait_for("ime: commit", 20, "search IME commit")
        check(True, "IME commit in the search box")
        b.key("esc", 0.6)

        # 貼り付け (項目10): Notes で行コピー -> エディタへ。
        b.key("f5", 1.2)     # Notes
        b.key("f9", 0.6)     # IME オフに戻す
        type_romaji(b, "paste me")
        b.key("ctrl-c", 0.6)  # 行コピー
        b.key("ctrl-t", 1.2)  # エディタ
        b.key("ctrl-v", 0.8)  # 貼り付け
        b.wait_for("editor: pasted", 20, "editor paste")
        check(True, "paste into the Editor")
        b.key("f6", 0.8)
        b.wait_for("editor: saved", 20, "editor save 2")
        b.key("f1", 1.0)
        out = b.cmd_output("type edit.txt")
        check("paste me" in out, "edit.txt contains the pasted line")
    finally:
        b.stop()
    print("IME APPS TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ImeTestFailure as exc:
        print("IME APPS TEST FAILED:", exc)
        sys.exit(1)
    except sg.SettingsGuiTestFailure as exc:
        print("IME APPS TEST FAILED:", exc)
        sys.exit(1)
