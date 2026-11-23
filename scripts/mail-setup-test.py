#!/usr/bin/env python3
"""Mail setup form test: opening Mail unconfigured shows
the in-app account form; saving writes mail.conf in the
shell's format (password wrapped, not plain).
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


class MailSetupFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise MailSetupFailure(what)
    print("ok:", what)


KEYMAP = {".": "dot", "-": "minus", "@": "shift-2",
          "_": "shift-minus"}


def type_text(b, text):
    for ch in text:
        b.key(KEYMAP.get(ch, ch), 0.16)


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
        b.key("ctrl-m", 1.2)
        b.wait_for("mail: setup form opened", 20,
                   "setup form auto-open")
        check(True, "setup form opens when unconfigured")
        type_text(b, "me@example.jp")       # 0: From
        b.key("tab", 0.4)
        type_text(b, "smtp.example.jp")     # 1: SMTP host
        b.key("tab", 0.4)
        type_text(b, "587")                 # 2: SMTP port
        b.key("tab", 0.4)
        type_text(b, "pop.example.jp")      # 3: POP3 host
        b.key("tab", 0.4)                   # 4: POP3 port (empty)
        b.key("tab", 0.4)                   # 5: IMAP host (empty)
        b.key("tab", 0.4)                   # 6: IMAP port (empty)
        b.key("tab", 0.4)                   # 7: user
        type_text(b, "me")
        b.key("tab", 0.4)                   # 8: password
        type_text(b, "secretpw")
        b.key("tab", 0.4)                   # 9: SMTP TLS
        b.key("tab", 0.4)                   # 10: POP3 TLS
        b.key("tab", 0.4)                   # 11: IMAP TLS
        b.key("tab", 0.4)                   # 12: [ Save ]
        b.key("ret", 0.8)
        b.wait_for("mail: setup saved", 20, "setup saved")
        check(True, "setup saved from the form")
        b.key("f1", 1.0)
        out = b.cmd_output("type mail.conf")
        check("smtp.example.jp" in out and "pop.example.jp" in out,
              "mail.conf has the servers")
        check("secretpw" not in out and "pass_enc" in out,
              "password stored wrapped, not plain")
        # ストアのサーバー設定フォーム (項目9の確認)。
        b.key("ctrl-k", 1.2)
        b.key("o", 0.8)
        b.wait_for("store-ui: server form opened", 20,
                   "store server form")
        type_text(b, "10.0.2.2")
        b.key("tab", 0.4)
        type_text(b, "8080")
        b.key("tab", 0.4)
        type_text(b, "/")
        b.key("tab", 0.4)
        b.key("ret", 0.8)
        b.wait_for("store-ui: server configured", 20,
                   "store server saved")
        check(True, "store server saved from the app")
        b.key("f1", 1.0)
        out = b.cmd_output("type store.conf")
        check("10.0.2.2 8080 /" in out,
              "store.conf written by the GUI: %r" % out[:60])
        b.cmd_output("sync")
    finally:
        b.stop()
    print("MAIL SETUP TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (MailSetupFailure, sg.SettingsGuiTestFailure) as exc:
        print("MAIL SETUP TEST FAILED:", exc)
        sys.exit(1)
