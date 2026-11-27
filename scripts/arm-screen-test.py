#!/usr/bin/env python3
"""PrismOS の ARM 画面テスト (QEMU virt + ramfb)。

ramfb に描いた壁紙とタスクバーを、QEMU モニタの screendump で
PPM として取り出し、画素の色で確かめる。

チェック:
  * 画面サイズ 1024x768
  * 壁紙がグラデーション (上の青から下の紫へ、黒一色でない)
  * 中央にひし形 (プリズム色の画素が一定数ある)
  * 下 48 ピクセルがタスクバー (濃い色 + 上端に青い線)
  * タスクバーに白い文字 (PrismOS 27H1) の画素がある

使い方: python3 scripts/arm-screen-test.py
  (終了コード 0 = 合格)
標準ライブラリのみを使う。
"""

import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "build", "kernel-arm.bin")
SHOT = os.path.join(ROOT, "build", "arm-screen.ppm")
LOG = os.path.join(ROOT, "build", "arm-screen.log")
MON_SOCK = "/tmp/prism-arm-screen-mon.sock"

QEMU = "qemu-system-aarch64"
W, H = 1024, 768


class ScreenTestFailure(Exception):
    pass


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 バイナリ: "P6\n<幅> <高さ>\n<最大値>\n" + 画素 (RGB 各 1 バイト)
    if not data.startswith(b"P6"):
        raise ScreenTestFailure("screendump が P6 PPM ではありません")
    # ヘッダを 3 行読む (コメントなしを想定する)。
    parts = data.split(b"\n", 3)
    if len(parts) < 4:
        raise ScreenTestFailure("PPM ヘッダが読めません")
    w, h = (int(x) for x in parts[1].split())
    maxval = int(parts[2])
    px = parts[3]
    if (w, h) != (W, H):
        raise ScreenTestFailure(
            "画面サイズが %dx%d ではありません (実際 %dx%d)"
            % (W, H, w, h)
        )
    if maxval != 255 or len(px) < w * h * 3:
        raise ScreenTestFailure("PPM の画素データが足りません")
    return px


def pixel(px, x, y):
    i = (y * W + x) * 3
    return px[i], px[i + 1], px[i + 2]


def count_color(px, x0, y0, x1, y1, target, tol=24):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = pixel(px, x, y)
            if (abs(r - target[0]) <= tol and abs(g - target[1]) <= tol
                    and abs(b - target[2]) <= tol):
                n += 1
    return n


def main() -> int:
    if not os.path.exists(KERNEL):
        print("MISSING: ARM カーネルがありません: %s" % KERNEL)
        return 1
    if os.path.exists(MON_SOCK):
        os.unlink(MON_SOCK)
    log_file = open(LOG, "wb")
    proc = subprocess.Popen(
        [QEMU, "-M", "virt", "-cpu", "cortex-a57", "-m", os.environ.get("PRISM_ARM_MEM_MB", "256"),
         "-device", "ramfb",
         "-serial", "file:%s" % LOG,
         "-monitor", "unix:%s,server,nowait" % MON_SOCK,
         "-display", "none", "-no-reboot",
         "-kernel", KERNEL],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd=ROOT,
        close_fds=True,
    )
    try:
        # シリアルログに fb の行が出るまで待つ (描画はその前に済む)。
        deadline = time.monotonic() + 30.0
        fb_ok = False
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise ScreenTestFailure(
                    "QEMU が途中で終了しました (exit code %s)"
                    % proc.returncode
                )
            log_file.flush()
            with open(LOG, "rb") as f:
                if b"[boot] fb: ramfb 1024x768" in f.read():
                    fb_ok = True
                    break
            time.sleep(0.5)
        if not fb_ok:
            raise ScreenTestFailure(
                "30 秒待っても ramfb の起動行が出ませんでした"
            )
        print("ok: ramfb 1024x768")

        # モニタに繋いで screendump を取る。
        mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon.settimeout(10.0)
        mon.connect(MON_SOCK)
        mon.recv(4096)  # あいさつ
        mon.sendall(("screendump %s\n" % SHOT).encode("ascii"))
        time.sleep(1.0)
        try:
            mon.recv(4096)
        except socket.timeout:
            pass
        mon.close()

        if not os.path.exists(SHOT):
            raise ScreenTestFailure("screendump が作られませんでした")
        px = read_ppm(SHOT)
        print("ok: screendump 1024x768")

        # 壁紙: 上は濃い青、下は紫。黒一色でないこと。
        top = pixel(px, 512, 20)
        mid = pixel(px, 512, 360)
        bot = pixel(px, 512, 700)
        print("ok: wallpaper top=%s mid=%s bottom=%s" % (top, mid, bot))
        if top[2] < 30 or bot[0] < 60 or bot[2] < 150:
            raise ScreenTestFailure(
                "壁紙のグラデーションが想定と違います "
                "(top=%s bottom=%s)" % (top, bot)
            )
        # ひし形: プリズム色 (76,194,255) の画素が中央にある。
        n_prism = count_color(px, 300, 190, 730, 530, (76, 194, 255))
        print("ok: prism pixels=%d" % n_prism)
        if n_prism < 500:
            raise ScreenTestFailure(
                "ひし形のプリズム色画素が少なすぎます (%d)" % n_prism
            )
        # タスクバー: 下 48 ピクセルが濃い色で、上端に青い線。
        task = pixel(px, 512, 750)
        edge = pixel(px, 512, 721)
        print("ok: taskbar=%s edge=%s" % (task, edge))
        if task[0] > 40 or task[1] > 40 or task[2] > 80:
            raise ScreenTestFailure(
                "タスクバーの色が想定と違います (%s)" % (task,)
            )
        if edge[2] < 150 or edge[0] > 120:
            raise ScreenTestFailure(
                "タスクバー上端の青い線が見えません (%s)" % (edge,)
            )
        # タスクバーの文字: 白い画素が一定数ある。
        n_white = count_color(px, 16, 730, 320, 760, (255, 255, 255),
                              tol=40)
        print("ok: taskbar text white pixels=%d" % n_white)
        if n_white < 100:
            raise ScreenTestFailure(
                "タスクバーの文字が見えません (白画素 %d)" % n_white
            )
        print("ARM SCREEN TEST PASSED")
        return 0
    except ScreenTestFailure as e:
        print("MISSING: %s" % e)
    except OSError as e:
        print("MISSING: 通信でエラー: %s" % e)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        log_file.close()
    return 1


if __name__ == "__main__":
    sys.exit(main())
