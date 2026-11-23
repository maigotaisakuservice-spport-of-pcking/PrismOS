#!/usr/bin/env python3
"""PrismOS 32bit GUI デスクトップテスト。

`desktop` コマンドで Bochs VBE フレームバッファの GUI に
入り、モニタ経由で確かめる:

  - 起動バナー "[desktop32] framebuffer 1024x768x32"
  - スクリーンダンプが 1024x768 で、壁紙とタスクバーの
    色が出ている
  - PS/2 キーボードで打った `ver` が端末ウィンドウの
    シェルで実行され、結果がシリアルにも出る
  - マウスクリックが "[desktop32] click x,y" として出る
  - タイトルバーのドラッグでウィンドウが動く
    (ダンプのタイトル色の位置で確認)
  - タスクバーの "Text mode" クリックでテキストに戻る

使い方: python3 scripts/gui32-test.py (make iso32 の後)
成功したら "GUI32 TEST PASSED" と出す。
"""
import os
import pty
import select
import socket
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos-32.iso")
LOG = os.path.join(ROOT, "build", "gui32.log")
MON = os.path.join(ROOT, "build", "qemu-monitor-gui32.sock")
SHOT = os.path.join(ROOT, "build", "shots", "gui32.ppm")

FAILURES = []


def check(label, ok):
    print(("ok: " if ok else "FAIL: ") + label)
    if not ok:
        FAILURES.append(label)


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    rest = data[3:]
    parts = []
    i = 0
    while len(parts) < 3:
        while i < len(rest) and rest[i:i + 1].isspace():
            i += 1
        j = i
        while j < len(rest) and not rest[j:j + 1].isspace():
            j += 1
        parts.append(int(rest[i:j]))
        i = j
    i += 1
    return parts[0], parts[1], rest[i:]


def pixel(raw, w, x, y):
    k = (y * w + x) * 3
    return raw[k], raw[k + 1], raw[k + 2]


def near(px, rgb, tol=6):
    return all(abs(px[i] - rgb[i]) <= tol for i in range(3))


class Session:
    def __init__(self):
        self.buf = b""
        self.pos = 0
        self.log = open(LOG, "wb")
        if os.path.exists(MON):
            os.unlink(MON)
        master, slave = pty.openpty()
        tty.setraw(slave)
        attrs = termios.tcgetattr(slave)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave, termios.TCSANOW, attrs)
        self.master = master
        self.proc = subprocess.Popen(
            [
                "qemu-system-i386",
                "-cdrom", ISO,
                "-boot", "order=d",
                "-m", "256M",
                "-serial", "stdio",
                "-monitor", "unix:%s,server,nowait" % MON,
                "-display", "vnc=:10",
                "-no-reboot",
            ],
            stdin=slave, stdout=slave,
            stderr=subprocess.DEVNULL, cwd=ROOT, close_fds=True)
        os.close(slave)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                self.mon.connect(MON)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        else:
            raise
        self.mon.settimeout(2)
        self.mon.recv(4096)

    def wait_for(self, needle, timeout=40.0):
        needle_b = needle.encode()
        deadline = time.monotonic() + timeout
        while True:
            idx = self.buf.find(needle_b, self.pos)
            if idx >= 0:
                self.pos = idx + len(needle_b)
                return True
            if time.monotonic() > deadline:
                return False
            r, _, _ = select.select([self.master], [], [], 0.5)
            if not r:
                continue
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                return False
            if not chunk:
                return False
            self.buf += chunk
            self.log.write(chunk)
            self.log.flush()

    def send(self, cmd):
        os.write(self.master, (cmd + "\r").encode())

    def hmp(self, cmd):
        self.mon.sendall((cmd + "\n").encode())
        time.sleep(0.4)
        try:
            self.mon.recv(4096)
        except socket.timeout:
            pass

    def shot(self):
        os.makedirs(os.path.dirname(SHOT), exist_ok=True)
        self.hmp("screendump %s" % SHOT)
        time.sleep(0.5)
        return read_ppm(SHOT)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log.close()


def main():
    if not os.path.exists(ISO):
        print("32bit ISO がありません。make iso32 を先に。")
        return 1
    sess = Session()
    try:
        check("shell prompt", sess.wait_for("prismos> ", 60.0))
        sess.send("desktop")
        check("desktop starts",
              sess.wait_for("[desktop32] framebuffer 1024x768x32",
                            30.0))
        time.sleep(1.0)
        w, h, raw = sess.shot()
        check("framebuffer is 1024x768", (w, h) == (1024, 768))
        check("wallpaper colour at top-left",
              near(pixel(raw, w, 10, 10), (0x1D, 0x4E, 0x89)))
        check("taskbar colour at bottom",
              near(pixel(raw, w, 500, 760), (0x1B, 0x1B, 0x1B)))
        check("window title bar present",
              near(pixel(raw, w, 60, 50), (0x24, 0x56, 0xA6)))
        # 端末ウィンドウで ver を実行 (PS/2 キーボード経由)
        for key in ("v", "e", "r"):
            sess.hmp("sendkey %s" % key)
        sess.hmp("sendkey ret")
        check("shell runs inside the GUI terminal",
              sess.wait_for("PrismOS version 27H1", 30.0))
        # マウス: QEMU の mouse_move は相対移動として届く
        # ので、テスト側で現在位置を持って差分を送る。
        mpos = [0, 0]

        def move_to(x, y):
            sess.hmp("mouse_move %d %d" % (x - mpos[0], y - mpos[1]))
            mpos[0], mpos[1] = x, y

        # ウィンドウ内をクリック
        move_to(300, 300)
        sess.hmp("mouse_button 1")
        sess.hmp("mouse_button 0")
        check("mouse click reported",
              sess.wait_for("[desktop32] click 300,300", 20.0))
        # タイトルバーをドラッグして右下へ動かす
        move_to(200, 58)
        sess.hmp("mouse_button 1")
        move_to(260, 98)
        sess.hmp("mouse_button 0")
        time.sleep(1.0)
        w, h, raw = sess.shot()
        check("window dragged: title at new position",
              near(pixel(raw, w, 220, 104), (0x24, 0x56, 0xA6)))
        check("window dragged: old position is wallpaper",
              near(pixel(raw, w, 60, 50), (0x1D, 0x4E, 0x89)))
        # タスクバーの Text mode ボタンで戻る
        move_to(839, 748)
        sess.hmp("mouse_button 1")
        sess.hmp("mouse_button 0")
        check("back to text mode",
              sess.wait_for("[desktop32] back to text mode", 20.0))
        check("text shell prompt again",
              sess.wait_for("prismos> ", 20.0))
    finally:
        sess.stop()
    if FAILURES:
        print("GUI32 TEST FAILED (%d)" % len(FAILURES))
        return 1
    print("GUI32 TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
