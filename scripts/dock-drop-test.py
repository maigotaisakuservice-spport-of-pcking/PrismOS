#!/usr/bin/env python3
"""PrismOS Dock ドロップ テスト (項目7g)。

Files で prism.bmp をドラッグし、Dock の Photos アイコン
へ落とすと、そのファイルが Photos で開くこと
(シリアルに "drag: opened prism.bmp") を確認する。
Dock は既定で非表示なので、テスト冒頭で設定 (外観ページ
の 'k') から表示し、settings.txt の dock=show で確認して
から進める。Dock のボタン位置は、設定 (settings.txt の
hidden_apps / dock_size など) と実画面サイズから
desktop.c の dock_layout と同じ式で計算する (横置き
タスクバー + 下 Dock 以外は失敗として報告する)。

使い方: python3 scripts/dock-drop-test.py
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
dt = _load("drag_test", "drag-test.py")

APPS = ["Terminal", "Files", "Settings", "Browser", "Notes",
        "Calendar", "Photos", "Music", "Cards", "Shooter",
        "Sheet", "Video", "Installer", "Mail", "Docs",
        "Slides", "Contacts", "Editor", "Paint", "Store",
        "Notifications"]
PHOTOS = 6


class DockFailure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise DockFailure(what)
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
        for i in range(1, 25):
            b.cmd_output("rm vid-%02d.pv1" % i)
        for name in ("loop.elf", "maindyn.elf", "unsup.elf",
                     "wc.elf", "evil.elf", "evil2.elf",
                     "evil3.elf", "libguest.so", "melody.wav",
                     "sample.drv"):
            b.cmd_output("rm " + name)
        b.cmd_output("mv docs/prism.bmp prism.bmp")
        b.cmd_output("mv trash/prism.bmp prism.bmp")

        # Dock は既定で非表示。設定の外観ページ (初回は
        # ページ0) で 'k' を押して表示する。
        b.key("f3", 1.0)  # Settings
        b.key("k", 0.8)   # Dock 表示トグル
        b.key("f1", 1.0)  # Terminal へ戻る

        # 設定を読んで Dock の形状を決める。
        st = {}
        for line in b.cmd_output("cat settings.txt").split("\n"):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                st[k] = v
        def iget(k, d=0):
            try:
                return int(st.get(k, d))
            except ValueError:
                return d
        check(iget("dock_pos") == 0 and iget("taskbar_side") == 0
              and iget("taskbar_top") == 0
              and iget("dock_autohide") == 0,
              "dock layout is the default bottom strip")
        check(st.get("dock") == "show",
              "dock enabled in Settings (dock=show): %r"
              % st.get("dock"))
        hidden = iget("hidden_apps")
        visible = [i for i in range(len(APPS))
                   if i < 3 or not (hidden >> i) & 1]
        count = len(visible)
        vi = visible.index(PHOTOS)
        dock_size = iget("dock_size")
        bw_want = 92 if dock_size else 76
        bh = 40 if dock_size else 32
        strip_h = 56 if dock_size else 44

        w, h, _raw = b.dump()
        fit = (w - 16 - (count - 1) * 4) // count
        bw = bw_want if fit >= bw_want else max(fit, 40)
        total = count * bw + (count - 1) * 4
        start_x = (w - total) // 2
        dock_y = h - 48 - 44
        cx = start_x + vi * (bw + 4) + bw // 2
        cy = dock_y + (strip_h - bh) // 2 + bh // 2
        print("dock: screen %dx%d count=%d photos idx=%d "
              "center=(%d,%d)" % (w, h, count, vi, cx, cy))

        rows = dt.root_rows(b)
        bmp = rows.index("prism.bmp")
        b.key("f2", 1.2)  # Files
        b.mouse_to(300, dt.row_y(bmp))
        b.hmp("mouse_button 1")
        time.sleep(0.6)
        b.mouse_to(cx, cy)
        time.sleep(0.4)
        b.hmp("mouse_button 0")
        time.sleep(0.6)
        b.wait_for("drag: opened prism.bmp", 30, "dock drop opens")
        check(True, "dropped prism.bmp on the Dock Photos icon "
                    "and it opened in Photos")
    finally:
        b.stop()
    print("DOCK DROP TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (sg.SettingsGuiTestFailure, DockFailure,
            dt.DragFailure) as e:
        print("DOCK DROP TEST FAILED:", e)
        sys.exit(1)
