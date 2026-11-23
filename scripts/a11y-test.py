#!/usr/bin/env python3
"""PrismOS accessibility test.

Boots the ISO in QEMU (VNC display, monitor socket) and checks
the three accessibility features end to end:

  1. Runtime text size: F3 opens Settings, 'b' toggles the
     large text size. The serial line proves the toggle and
     the screenshot must gain ink (bigger glyphs) in the
     window area; 'b' again returns to standard.
  2. Narrator captions: 'n' turns the narrator on. The
     toggle's own notification must come back as a
     "narrator: ..." serial line, and the caption bar
     (black strip with yellow text above the taskbar) must
     be visible in the screenshot.
  3. On-screen keyboard: 'o' shows the keyboard panel
     (dark keys above the taskbar, checked by pixel colour).
     Clicking the v, e, r keys and Enter with the mouse
     types "ver" into the Terminal; the version line on
     serial proves the clicks really entered text.

Usage: python3 scripts/a11y-test.py [--log PATH]
  (exit code 0 = passed; standard library only)
"""

import argparse
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
ISO = os.path.join(ROOT, "build", "prismos.iso")
DEFAULT_LOG = os.path.join(ROOT, "build", "boot-a11y.log")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-a11y.sock")
SHOT_DIR = os.path.join(ROOT, "build", "shots")

QEMU = "qemu-system-x86_64"


class A11yTestFailure(Exception):
    pass


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6\n"):
        raise A11yTestFailure("screenshot %s is not a P6 PPM" % path)
    rest = data[3:]
    parts = []
    i = 0
    while len(parts) < 3:
        while i < len(rest) and rest[i:i + 1].isspace():
            i += 1
        j = i
        while j < len(rest) and not rest[j:j + 1].isspace():
            j += 1
        parts.append(rest[i:j])
        i = j
    width, height, _maxval = int(parts[0]), int(parts[1]), int(parts[2])
    i += 1
    raw = rest[i:i + width * height * 3]
    if len(raw) < width * height * 3:
        raise A11yTestFailure("screenshot %s is truncated" % path)
    return width, height, raw


def px(raw, width, x, y):
    i = (y * width + x) * 3
    return raw[i], raw[i + 1], raw[i + 2]


def count_near(raw, width, height, x0, y0, x1, y1, target, tol=24):
    n = 0
    for y in range(max(0, y0), min(height, y1)):
        for x in range(max(0, x0), min(width, x1)):
            r, g, b = px(raw, width, x, y)
            if (abs(r - target[0]) <= tol and abs(g - target[1]) <= tol
                    and abs(b - target[2]) <= tol):
                n += 1
    return n


def count_whiteish(raw, width, height, x0, y0, x1, y1):
    n = 0
    for y in range(max(0, y0), min(height, y1)):
        for x in range(max(0, x0), min(width, x1)):
            r, g, b = px(raw, width, x, y)
            if r > 200 and g > 200 and b > 200:
                n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=DEFAULT_LOG)
    args = ap.parse_args()

    os.makedirs(SHOT_DIR, exist_ok=True)
    qemu_args = [
        QEMU, "-cdrom", ISO, "-m", "256M",
        "-serial", "stdio",
        "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
        "-display", "vnc=:8",
        "-no-reboot",
    ]
    try:
        os.unlink(MONITOR_SOCK)
    except OSError:
        pass

    log_file = open(args.log, "wb")
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    proc = subprocess.Popen(qemu_args, stdin=slave_fd, stdout=slave_fd,
                            stderr=subprocess.DEVNULL, cwd=ROOT,
                            close_fds=True)
    os.close(slave_fd)
    buf = bytearray()
    pos = 0
    monitor_sock = None

    def pump(timeout):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            r, _, _ = select.select([master_fd], [], [], remaining)
            if not r:
                return
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            buf.extend(chunk)
            log_file.write(chunk)
            log_file.flush()
            return

    def wait_for(needle, timeout, what):
        nonlocal pos
        needle_b = needle.encode("utf-8", "replace")
        deadline = time.monotonic() + timeout
        while True:
            idx = buf.find(needle_b, pos)
            if idx >= 0:
                pos = idx + len(needle_b)
                return
            if proc.poll() is not None:
                raise A11yTestFailure("QEMU exited early (code %s)"
                                      % proc.returncode)
            if time.monotonic() > deadline:
                raise A11yTestFailure(
                    "%s not seen in %s s" % (what, timeout))
            pump(min(0.5, deadline - time.monotonic()))

    def hmp(cmd):
        monitor_sock.sendall((cmd + "\n").encode("ascii"))
        data = b""
        while b"(qemu)" not in data:
            chunk = monitor_sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def screendump(name):
        path = os.path.join(SHOT_DIR, name)
        hmp("screendump %s" % path)
        time.sleep(0.5)
        return read_ppm(path)

    def sendkey(key, delay=0.3):
        hmp("sendkey %s" % key)
        time.sleep(delay)

    def mouse_move(dx, dy):
        hmp("mouse_move %d %d" % (dx, dy))
        time.sleep(0.4)

    def mouse_click():
        hmp("mouse_button 1")
        time.sleep(0.4)
        hmp("mouse_button 0")
        time.sleep(0.4)

    def find_cursor(raw, w, h):
        def white(x, y):
            if x < 0 or y < 0 or x >= w or y >= h:
                return False
            r, g, b = px(raw, w, x, y)
            return r > 200 and g > 200 and b > 200
        for y in range(0, h - 6):
            for x in range(0, w - 6):
                if not white(x, y) or white(x + 1, y):
                    continue
                ok = True
                for r in range(1, 6):
                    for c in range(0, r + 1):
                        if not white(x + c, y + r):
                            ok = False
                            break
                    if not ok or white(x + r + 1, y + r):
                        ok = False
                        break
                if ok:
                    return (x, y)
        return None

    def mouse_to(x, y):
        for _ in range(12):
            _w, _h, raw = screendump("a11y-mouse-find.ppm")
            cur = find_cursor(raw, _w, _h)
            if cur is None:
                mouse_move(8, 8)
                continue
            dx = x - cur[0]
            dy = y - cur[1]
            if abs(dx) <= 3 and abs(dy) <= 3:
                return
            mouse_move(max(-200, min(200, dx)),
                       max(-200, min(200, dy)))
        raise A11yTestFailure("mouse did not reach (%d, %d)" % (x, y))

    try:
        # Connect to the monitor.
        deadline = time.monotonic() + 30
        while monitor_sock is None:
            try:
                monitor_sock = socket.socket(socket.AF_UNIX,
                                             socket.SOCK_STREAM)
                monitor_sock.connect(MONITOR_SOCK)
            except OSError:
                monitor_sock = None
                if time.monotonic() > deadline:
                    raise A11yTestFailure("monitor socket never appeared")
                time.sleep(0.3)
        monitor_sock.recv(4096)  # banner

        wait_for("desktop: ready", 60, "desktop ready")
        wait_for("prismos> ", 30, "shell prompt")

        # Settings at standard text size.
        sendkey("f3")
        time.sleep(0.8)
        w, h, raw = screendump("a11y-01-settings.ppm")
        ink_std = count_whiteish(raw, w, h, 24, 60, w - 24, h - 110)
        print("ok: settings open (ink %d)" % ink_std)

        # 1. Large text size: serial + more ink on screen.
        sendkey("b")
        wait_for("settings: text size large", 20, "text size large")
        time.sleep(0.8)
        w, h, raw = screendump("a11y-02-large.ppm")
        ink_large = count_whiteish(raw, w, h, 24, 60, w - 24, h - 110)
        if ink_large <= ink_std + 100:
            raise A11yTestFailure(
                "large text did not add ink (%d -> %d)"
                % (ink_std, ink_large))
        print("ok: large text size renders bigger (%d -> %d)"
              % (ink_std, ink_large))
        sendkey("b")
        wait_for("settings: text size standard", 20,
                 "text size standard")
        print("ok: text size back to standard")

        # 2. Narrator: the toggle notification is captioned.
        sendkey("n")
        wait_for("settings: narrator on", 20, "narrator on")
        wait_for("narrator: Narrator on", 20, "narrator caption log")
        time.sleep(0.8)
        w, h, raw = screendump("a11y-03-narrator.ppm")
        yellow = count_near(raw, w, h, 0, h - 48 - 40, w, h - 48,
                            (255, 255, 0), 60)
        if yellow < 20:
            raise A11yTestFailure(
                "narrator caption bar not visible (%d yellow px)"
                % yellow)
        print("ok: narrator caption bar visible (%d yellow px)"
              % yellow)

        # 3. On-screen keyboard: panel appears, clicks type.
        sendkey("o")
        wait_for("settings: on-screen keyboard on", 20, "osk on")
        time.sleep(0.8)
        w, h, raw = screendump("a11y-04-osk.ppm")
        taskbar_y = h - 48
        panel_h = 5 * (36 + 4) + 4
        panel_y = taskbar_y - panel_h
        keys_px = count_near(raw, w, h, 0, panel_y, w, taskbar_y,
                             (0x3A, 0x3A, 0x44), 16)
        if keys_px < 5000:
            raise A11yTestFailure(
                "osk panel not drawn (%d key px)" % keys_px)
        print("ok: on-screen keyboard panel drawn (%d key px)"
              % keys_px)

        sendkey("f1")  # back to the Terminal
        time.sleep(0.8)

        def key_center(row_idx, keys, idx):
            n = len(keys)
            kw = (w - 16 - 4 * (n + 1)) // n
            kx = 8 + 4 + idx * (kw + 4)
            ky = panel_y + 4 + row_idx * (36 + 4)
            return kx + kw // 2, ky + 18

        # v (row 3 "zxcvbnm,.", idx 3), e (row 1, idx 2),
        # r (row 1, idx 3), then Enter on the control row.
        for (r, keys, i) in ((3, "zxcvbnm,.", 3),
                             (1, "qwertyuiop", 2),
                             (1, "qwertyuiop", 3)):
            cx, cy = key_center(r, keys, i)
            mouse_to(cx, cy)
            mouse_click()
            wait_for("osk: key pressed", 20, "osk key log")
        unit = (w - 16 - 4 * 7) // 18
        enter_x = 8 + 4 + (2 * unit + 4) + (3 * unit + 4) \
            + (6 * unit + 4) + (3 * unit) // 2
        enter_y = panel_y + 4 + 4 * (36 + 4) + 18
        mouse_to(enter_x, enter_y)
        mouse_click()
        wait_for("PrismOS version 27H1", 30,
                 "ver output typed via the on-screen keyboard")
        print("ok: on-screen keyboard typed 'ver' + Enter")

        print("A11Y TEST PASSED")
        return 0
    except A11yTestFailure as exc:
        print("A11Y TEST FAILED: %s" % exc)
        return 1
    finally:
        try:
            proc.terminate()
        except OSError:
            pass
        log_file.close()


if __name__ == "__main__":
    sys.exit(main())
