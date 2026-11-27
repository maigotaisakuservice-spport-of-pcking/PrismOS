#!/usr/bin/env python3
"""PrismOS 管理ページの破壊的操作テスト (項目7g)。

設定アプリの管理ページで、PIN 確認ダイアログ経由で:
  - 復元ポイントへの復元 (復元後にマーカーが消える)
  - 復元ポイントの削除 (一覧から消える)
  - ドライバの有効/無効の切り替え (無効化済みドライバを
    GUI で有効に戻す)
を実行し、シリアル出力とシェル側の状態で確認する。

使い方: python3 scripts/admin-destructive-test.py
  (終了コード 0 = 合格。標準ライブラリのみ)
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


class AdminDestructiveFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise AdminDestructiveFailure(what)
    print("ok:", what)


def pin_dialog(b):
    b.type_gui("1234")
    b.key("ret", 0.8)


def main():
    os.chdir(os.path.join(HERE, ".."))
    sg.make_disk()
    b = sg.Boot()
    try:
        b.wait_for("prismos> ", 90, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 60, "install")
        b.send("passwd")
        b.wait_for("New PIN (4-16 chars): ", 30, "new PIN prompt")
        b.send("1234")
        b.wait_for("Confirm PIN: ", 30, "confirm prompt")
        b.send("1234")
        b.wait_for("Account set.", 30, "account set")
        # プロンプト復帰まで待って永続化を完了させてから止める
        # (Account set. 表示直後に VM を落とすと、ループ末尾の
        # persist 同期が走る前にアカウント書き込みが消える)。
        b.wait_for("prismos> ", 30, "prompt after passwd")
        b.send("ls")
        b.wait_for("prismos> ", 30, "prompt after ls")
    finally:
        b.stop()

    b = sg.Boot()
    try:
        b.wait_for("PrismOS login", 60, "login screen")
        b.wait_for("PIN: ", 30, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 60, "admin login")

        # 復元ポイント A を作り、その後にマーカーを作る。
        b.cmd_output("snapshot")
        out = b.cmd_output("snapshot list")
        check(out.count("snap-") == 1,
              "one restore point exists: %r" % out[:80])
        b.cmd_output("mkdir markerdir")
        out = b.cmd_output("ls")
        check("markerdir" in out, "marker created after snapshot")

        # ドライバ testdrv をシェルで無効化しておく
        # (管理者 PIN の対話確認に応じる)。
        b.send("driver disable testdrv")
        b.wait_for("Admin PIN: ", 30, "driver admin PIN prompt")
        b.send("1234")
        b.wait_for("prismos> ", 30, "prompt after driver disable")

        # 設定 → 管理ページ (Tab x7)。ここからは 1 回の
        # 設定訪問の中でセクションを移動する。
        b.key("f3", 1.2)
        for _ in range(7):
            b.key("tab", 0.4)
        # セクション 0 (FDE) → 1 (復元ポイント)。
        b.key("right", 0.4)
        # 先頭行 = 復元ポイント A。Enter で復元 (PIN 経由)。
        b.key("ret", 0.6)
        pin_dialog(b)
        b.wait_for("settings: restored snapshot ", 30,
                   "snapshot restored via GUI")
        check(True, "restore point restored from the GUI")
        b.key("f1", 1.0)
        out = b.cmd_output("ls")
        check("markerdir" not in out,
              "marker is gone after the restore")

        # 復元ポイント B を作っておく。削除とドライバ切替は
        # 次の起動で行う (管理ページのセクション状態は静的
        # 変数に残るため、再起動で初期状態から組み立てる)。
        b.cmd_output("snapshot")
        out = b.cmd_output("snapshot list")
        check(out.count("snap-") == 2,
              "two restore points exist: %r" % out[:80])
    finally:
        b.stop()

    b = sg.Boot()
    try:
        b.wait_for("PrismOS login", 60, "login screen")
        b.wait_for("PIN: ", 30, "PIN prompt")
        b.send("1234")
        b.wait_for("Welcome to PrismOS.", 60, "admin login")

        # testdrv の無効化は復元で巻き戻っている (復元ポイント
        # A は無効化より前に作った) ため、ここでやり直す。
        b.send("driver disable testdrv")
        b.wait_for("Admin PIN: ", 30, "driver admin PIN prompt")
        b.send("1234")
        b.wait_for("prismos> ", 30, "prompt after driver disable")
        out = b.cmd_output("driver list")
        check("testdrv" in out.split("disabled list:")[-1],
              "testdrv disabled again after the restore rollback")

        # 復元ポイント B を GUI で削除する。
        b.key("f3", 1.2)
        for _ in range(7):
            b.key("tab", 0.4)
        b.key("right", 0.4)     # 復元ポイント
        b.key("down", 0.4)      # 2 件目 (B)
        b.key("d", 0.6)         # 削除 (PIN 経由)
        pin_dialog(b)
        b.wait_for("settings: snapshot deleted ", 30,
                   "snapshot deleted via GUI")
        check(True, "restore point deleted from the GUI")

        # ドライバの切り替え: 設定を開いたままセクション
        # 1 → 3 (ドライバ) へ進み、testdrv を有効に戻す
        # (設定の再入場はフォーカス状態を引き継ぐため、
        # 訪問をまたがない)。キー間隔はスロットリング下の
        # 取りこぼし対策で広めに取る。
        b.key("right", 1.0)
        b.key("right", 1.0)     # ドライバ
        # 行は 読み込み済み → 無効化のみ の順で、testdrv は
        # 最終行。先頭から up 1 回で最終行へ回る。
        b.key("up", 0.8)
        b.key("ret", 1.0)       # 切り替え (PIN 経由)
        pin_dialog(b)
        b.wait_for("settings: driver testdrv enabled", 30,
                   "driver toggled via GUI")
        check(True, "driver re-enabled from the GUI")
        b.key("f1", 1.0)
        out = b.cmd_output("snapshot list")
        check(out.count("snap-") == 1,
              "one restore point remains: %r" % out[:80])
        out = b.cmd_output("driver list")
        check("testdrv" not in out.split("disabled list:")[-1],
              "disabled list no longer holds testdrv")
    finally:
        b.stop()
    print("ADMIN DESTRUCTIVE TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (sg.SettingsGuiTestFailure,
            AdminDestructiveFailure) as e:
        print("ADMIN DESTRUCTIVE TEST FAILED:", e)
        sys.exit(1)
