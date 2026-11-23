#!/usr/bin/env python3
"""PrismOS S3 復帰後のマウス テスト (項目7a)。

S3 復帰でファームウェアが 8259 PIC を既定状態に戻す
ため、再初期化しないと IRQ12 (マウス) がベクタ 44 に
届かず、復帰後にマウスのパケットが来なくなる既知の
問題の回帰テスト。スリープ前にカーソルを動かせること、
S3 に入って system_wakeup で復帰した後にもカーソルが
目標位置まで動くこと (mouse_to がスクリーンダンプで
到達を確認する) を見る。

使い方: python3 scripts/s3-mouse-test.py
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


class S3MouseFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise S3MouseFailure(what)
    print("ok:", what)


def main():
    os.chdir(os.path.join(HERE, ".."))
    b = pm.Gui()
    try:
        b.wait_for("PrismOS login", 60, "login screen")
        try:
            b.wait_for("PIN: ", 8, "PIN prompt")
        except Exception:
            b.send("")
            b.wait_for("PIN: ", 30, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 60, "admin login")
        # 基準: スリープ前はマウスが動く。
        b.mouse_to(700, 200)
        check(True, "mouse moves before sleep")
        # シェルの sleep で S3 に入り、モニタから起こす。
        b.send("sleep")
        b.wait_for("power: entering S3", 30, "S3 entry")
        time.sleep(2)
        b.hmp("system_wakeup")
        b.wait_for("power: resumed from S3", 30, "S3 resume")
        check(True, "resumed from S3")
        time.sleep(1)
        # 復帰後もマウスのパケットが届くこと。
        b.mouse_to(300, 500)
        check(True, "mouse still moves after S3 resume "
                    "(IRQ12 packets arrive)")
        out = b.cmd_output("ver")
        check("27H1" in out, "shell alive after resume")
    finally:
        b.stop()
    print("S3 MOUSE TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (sg.SettingsGuiTestFailure, pm.PowerMenuTestFailure,
            S3MouseFailure) as e:
        print("S3 MOUSE TEST FAILED:", e)
        sys.exit(1)
