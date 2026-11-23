#!/usr/bin/env python3
"""PrismOS desktop screen test.

Boots the ISO in QEMU with a VNC display (no window needed) and
checks the screen with the monitor's `screendump` command:
  1. the desktop is drawn (wallpaper gradient, taskbar, Dock),
  2. typing in the Terminal shows on screen,
  3. F2 opens the Files app, F3 opens Settings, F1 goes back,
  4. changing the accent in Settings really changes the screen,
  5. Browser fetches a page, Notes edits and saves, Calendar
     shows the month, the mouse clicks a Dock button,
  6. F8 search opens Calendar, F9 Photos shows a BMP,
     F10 Music plays a WAV, F11 Cards plays blackjack.
  7. F12 opens Shooter: the 3D view is drawn, Space shoots
     the enemy straight ahead (serial confirms the hit),
     and walking forward makes the enemy bigger on screen.
  8. Ctrl+O opens Sheet: 10 in A1, 20 in A2, =SUM(A1:A2) in
     A3 shows the computed 30, and saving to sheet.csv and
     loading it back keeps the values.
  9. Ctrl+V opens Video: the 24 vid-*.pv1 frames (320x240,
     compressed, each with a 2000-sample audio track) are
     listed, Space plays them at 4 fps (the ball moves),
     'r' returns to frame 1 and 'n' steps to frame 2.
 10. Ctrl+I opens the Installer: with no disk attached it
     honestly says no disk was found.
 11. Ctrl+M opens Mail: with no server configured it says
     so honestly, and the compose form takes typed text.

Verification is by pixels, not by looking: the script reads the
PPM screenshots and counts pixels of the expected colours in the
expected regions. QEMU's serial output is still checked for the
banner and the "desktop: ready" line.

Usage: python3 scripts/desktop-test.py [--log PATH]
  (exit code 0 = passed; standard library only)
"""

import argparse
import os
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
DEFAULT_LOG = os.path.join(ROOT, "build", "boot-desktop.log")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-desktop.sock")
SHOT_DIR = os.path.join(ROOT, "build", "shots")

QEMU = "qemu-system-x86_64"


class DesktopTestFailure(Exception):
    pass


def read_ppm(path):
    """Read a P6 PPM screenshot into (width, height, pixels)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6\n"):
        raise DesktopTestFailure("screenshot %s is not a P6 PPM" % path)
    rest = data[3:]
    # Header: width, height, maxval, then one whitespace byte.
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
    width, height, maxval = int(parts[0]), int(parts[1]), int(parts[2])
    i += 1  # the single whitespace byte after maxval
    raw = rest[i:i + width * height * 3]
    if len(raw) < width * height * 3:
        raise DesktopTestFailure("screenshot %s is truncated" % path)
    return width, height, raw


def px(raw, width, x, y):
    i = (y * width + x) * 3
    return raw[i], raw[i + 1], raw[i + 2]


def count_near(raw, width, height, x0, y0, x1, y1, target, tol=24):
    """Count pixels near target RGB in a rectangle."""
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
    parser = argparse.ArgumentParser(description="PrismOS desktop test")
    parser.add_argument("--log", default=DEFAULT_LOG)
    args = parser.parse_args()
    os.makedirs(SHOT_DIR, exist_ok=True)
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1

    # Local web server for the Browser steps: the sandbox
    # blocks external DNS/egress, so the pages come from the
    # host at 10.0.2.2 (same approach as browser-test.py).
    def _serve_pages():
        pages = {
            "/one.html": ("<html><head><title>Local One</title>"
                          "</head><body><h1>Local test page</h1>"
                          "<p>This page was fetched from the "
                          "host test server.</p><p>It has several "
                          "lines of text so the screen visibly "
                          "fills with content.</p><p>Line three "
                          "of the local page.</p></body></html>"),
            "/two.html": ("<html><head><title>Local Two</title>"
                          "</head><body><h1>Second local page</h1>"
                          "<p>Typing a different address fetched "
                          "this other page from the same local "
                          "server.</p><p>More text follows here "
                          "to fill the window.</p></body></html>"),
        }
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", 8380))
        srv.listen(8)
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            try:
                req = b""
                while b"\r\n\r\n" not in req:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    req += chunk
                path = req.split(b" ")[1].decode("latin1")
                body = pages.get(path, "not found").encode()
                conn.sendall(
                    ("HTTP/1.1 200 OK\r\nContent-Type: "
                     "text/html\r\nContent-Length: %d\r\n"
                     "Connection: close\r\n\r\n"
                     % len(body)).encode() + body)
            except OSError:
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    threading.Thread(target=_serve_pages, daemon=True).start()

    qemu_args = [
        QEMU, "-cdrom", ISO, "-m", "256M",
        "-serial", "stdio",
        "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
        "-display", "vnc=:7",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        # SB16 for the Music app (sound goes to the null backend;
        # the real capture test is scripts/music-test.py).
        "-device", "sb16,audiodev=snd0",
        "-audiodev", "none,id=snd0",
        "-no-reboot",
        "-d", "cpu_reset,int",
        "-D", os.path.join(ROOT, "build", "qemu-debug-desktop.log"),
    ]
    try:
        os.unlink(MONITOR_SOCK)
    except OSError:
        pass

    log_file = open(args.log, "wb")
    stderr_file = open(os.path.join(ROOT, "build", "qemu-stderr-desktop.log"), "wb")
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    proc = subprocess.Popen(qemu_args, stdin=slave_fd, stdout=slave_fd,
                            stderr=stderr_file, cwd=ROOT, close_fds=True)
    os.close(slave_fd)
    buf = bytearray()
    pos = 0
    monitor_sock = None
    failures = []

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
                raise DesktopTestFailure("QEMU exited early (code %s)"
                                         % proc.returncode)
            if time.monotonic() > deadline:
                raise DesktopTestFailure("%s not seen in %s s" % (what, timeout))
            pump(min(0.5, deadline - time.monotonic()))

    def wait_for_any(needles, timeout, what):
        nonlocal pos
        needles_b = [n.encode("utf-8", "replace") for n in needles]
        deadline = time.monotonic() + timeout
        while True:
            best = -1
            best_len = 0
            for nb in needles_b:
                idx = buf.find(nb, pos)
                if idx >= 0 and (best < 0 or idx < best):
                    best = idx
                    best_len = len(nb)
            if best >= 0:
                pos = best + best_len
                return
            if proc.poll() is not None:
                raise DesktopTestFailure("QEMU exited early (code %s)"
                                         % proc.returncode)
            if time.monotonic() > deadline:
                raise DesktopTestFailure("%s not seen in %s s" % (what, timeout))
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

    def sendkey(key, delay=0.25):
        hmp("sendkey %s" % key)
        time.sleep(delay)

    def mouse_move(dx, dy):
        hmp("mouse_move %d %d" % (dx, dy))
        time.sleep(0.5)

    def mouse_click():
        hmp("mouse_button 1")
        time.sleep(0.5)
        hmp("mouse_button 0")
        time.sleep(0.5)

    def find_cursor(raw, w, h):
        # The cursor is a white staircase arrow: row r starts
        # with r+1 white pixels and a non-white pixel after.
        # Text glyphs do not match that signature.
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
        # Closed-loop absolute move: large relative moves can
        # lose PS/2 packets in the guest, so locate the cursor
        # on screen and correct until it arrives.
        for _ in range(10):
            _w, _h, raw = screendump("mouse-find.ppm")
            pos = find_cursor(raw, _w, _h)
            if pos is None:
                mouse_move(8, 8)  # nudge so the cursor redraws
                continue
            dx = x - pos[0]
            dy = y - pos[1]
            if abs(dx) <= 3 and abs(dy) <= 3:
                return
            mouse_move(max(-200, min(200, dx)),
                       max(-200, min(200, dy)))
        raise DesktopTestFailure(
            "mouse did not reach (%d, %d)" % (x, y))

    def type_text(text):
        # Type via the QEMU monitor: each char as a sendkey.
        keymap = {
            ".": "dot", "/": "slash", ":": "shift-semicolon",
        }
        for ch in text:
            if ch in keymap:
                sendkey(keymap[ch], 0.15)
            elif ch == " ":
                sendkey("spc", 0.15)
            elif ch == "\n":
                sendkey("ret", 0.3)
            elif ch.isupper():
                sendkey("shift-" + ch.lower(), 0.15)
            else:
                sendkey(ch, 0.15)

    def type_sheet_text(text):
        # Like type_text, plus the punctuation a formula needs
        # ("=", "(", ")", ":", "/", "-", "+", ".", ",").
        keymap = {
            "=": "equal", "(": "shift-9", ")": "shift-0",
            ":": "shift-semicolon", "/": "slash", "-": "minus",
            "+": "shift-equal", ".": "dot", ",": "comma",
        }
        for ch in text:
            if ch in keymap:
                sendkey(keymap[ch], 0.15)
            elif ch == " ":
                sendkey("spc", 0.15)
            elif ch == "\n":
                sendkey("ret", 0.3)
            elif ch.isupper():
                sendkey("shift-" + ch.lower(), 0.15)
            else:
                sendkey(ch, 0.15)

    try:
        wait_for("PrismOS 27H1", 30, "boot banner")
        print("ok: boot banner")
        wait_for("desktop: ready", 30, "desktop ready")
        print("ok: desktop ready")
        wait_for("prismos> ", 30, "shell prompt")
        print("ok: shell prompt")
        time.sleep(1.0)

        monitor_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor_sock.settimeout(5)
        monitor_sock.connect(MONITOR_SOCK)
        hmp("")

        # 1. The desktop is drawn: the wallpaper image (top strip
        #    is dark dusk blue), the taskbar strip at the bottom is
        #    darker still. The top-right pixel of the real image is
        #    (55, 42, 105); the drawn gradient would be (11, 16, 38)
        #    everywhere, so this tells the two apart.
        w, h, raw = screendump("01-desktop.ppm")
        print("screen: %dx%d" % (w, h))
        top_n = count_near(raw, w, h, 0, 0, w, 10, (11, 16, 38), 30)
        if top_n < w * 5:
            raise DesktopTestFailure(
                "wallpaper top not dusk blue (%d pixels)" % top_n)
        print("ok: wallpaper drawn (%d dusk pixels on top row band)" % top_n)
        img_n = count_near(raw, w, h, 1260, 0, 1280, 10, (55, 42, 105), 30)
        if img_n < 10:
            raise DesktopTestFailure(
                "wallpaper image not on screen (%d pixels near (55,42,105))"
                % img_n)
        print("ok: wallpaper image on screen (%d pixels)" % img_n)
        taskbar_n = count_near(raw, w, h, 0, h - 10, w, h, (8, 12, 30), 30)
        if taskbar_n < w * 5:
            raise DesktopTestFailure(
                "taskbar strip not found (%d pixels)" % taskbar_n)
        print("ok: taskbar drawn")
        # The Dock strip sits just above the taskbar.
        dock_n = count_near(raw, w, h, 0, h - 48 - 44, w, h - 48,
                            (16, 22, 48), 30)
        if dock_n < 100:
            raise DesktopTestFailure("dock strip not found (%d)" % dock_n)
        print("ok: dock drawn")

        # 2. Typing in the Terminal shows on screen: white text
        #    pixels in the terminal window grow after typing.
        before = count_whiteish(raw, w, h, 24, 60, w - 24, h - 110)
        type_text("ver\n")
        wait_for("PrismOS version 27H1", 20, "ver output on serial")
        time.sleep(0.5)
        w, h, raw = screendump("02-terminal.ppm")
        after = count_whiteish(raw, w, h, 24, 60, w - 24, h - 110)
        if after <= before + 20:
            raise DesktopTestFailure(
                "typed text not visible on screen (%d -> %d white pixels)"
                % (before, after))
        print("ok: terminal text on screen (%d -> %d white pixels)"
              % (before, after))

        # 2b. Japanese text renders on screen: write a file
        #     with Japanese content over the serial line (the
        #     PS/2 keyboard cannot type it), clear the console
        #     and cat the file. Only the prompt (ASCII) and
        #     the Japanese line can be on screen afterwards,
        #     so white pixels far above the prompt's own ink
        #     prove the Japanese glyphs were drawn.
        os.write(master_fd,
                 "echo \u65e5\u672c\u8a9e\u306e\u30c6\u30b9\u30c8"
                 "\u3067\u3059 > jp.txt\r".encode("utf-8"))
        wait_for("prismos> ", 20, "prompt after writing jp.txt")
        os.write(master_fd, b"clear\r")
        wait_for("prismos> ", 20, "prompt after clear")
        os.write(master_fd, b"cat jp.txt\r")
        wait_for("\u65e5\u672c\u8a9e\u306e\u30c6\u30b9\u30c8"
                 "\u3067\u3059", 20, "jp.txt content on serial")
        wait_for("prismos> ", 20, "prompt after cat jp.txt")
        time.sleep(0.5)
        w, h, raw = screendump("02b-japanese.ppm")
        jp_n = count_whiteish(raw, w, h, 24, 60, w - 24, h - 110)
        if jp_n < 300:
            raise DesktopTestFailure(
                "Japanese text not rendered on screen "
                "(%d white pixels)" % jp_n)
        print("ok: Japanese text rendered on screen (%d white pixels)"
              % jp_n)

        # 3. F2 opens Files, F3 opens Settings, F1 goes back.
        #    The window is centred: recompute its title bar from
        #    the screen size the same way the kernel does.
        win_w = w * 78 // 100
        win_x = (w - win_w) // 2
        avail = h - 48 - 44
        win_h = avail * 72 // 100
        win_y = (avail - win_h) // 2
        sendkey("f2")
        time.sleep(0.8)
        w, h, raw = screendump("03-files.ppm")
        # The Files title is white text in the title bar.
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Files title not on screen")
        print("ok: Files app opens")
        # Open a file: move down one row and press Enter. The
        # viewer must show the file's text on screen.
        sendkey("down")
        sendkey("ret")
        time.sleep(0.8)
        w, h, raw = screendump("03b-file-view.ppm")
        text_n = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                win_x + 600, win_y + 300)
        if text_n < 50:
            raise DesktopTestFailure("file content not shown (%d)" % text_n)
        print("ok: Files opens a file")
        sendkey("esc")  # back to the list
        time.sleep(0.5)
        sendkey("f3")
        time.sleep(0.8)
        w, h, raw = screendump("04-settings.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Settings title not on screen")
        print("ok: Settings app opens")

        # 4. Changing the accent in Settings changes the screen:
        #    press "2" (violet) and check the title-bar underline
        #    turns violet.
        sendkey("2")
        time.sleep(0.8)
        w, h, raw = screendump("05-accent-violet.ppm")
        violet_n = count_near(raw, w, h, win_x, win_y + 26,
                              win_x + win_w, win_y + 32,
                              (124, 92, 255), 40)
        if violet_n < 20:
            raise DesktopTestFailure(
                "accent change not visible (%d violet pixels)" % violet_n)
        print("ok: accent change visible on screen")
        # 'w' switches to the drawn (gradient) wallpaper: the top
        # strip becomes exactly the dusk colour everywhere. 'w'
        # again restores the image.
        sendkey("w")
        time.sleep(0.8)
        w, h, raw = screendump("05b-wallpaper-drawn.ppm")
        drawn_n = count_near(raw, w, h, 0, 0, w, 10, (11, 16, 38), 30)
        if drawn_n < 12000:
            raise DesktopTestFailure(
                "drawn wallpaper not shown (%d dusk pixels)" % drawn_n)
        print("ok: wallpaper toggles to drawn gradient (%d)" % drawn_n)
        sendkey("w")
        time.sleep(0.8)
        w, h, raw = screendump("05c-wallpaper-image.ppm")
        img_n = count_near(raw, w, h, 1260, 0, 1280, 10, (55, 42, 105), 30)
        if img_n < 10:
            raise DesktopTestFailure(
                "wallpaper image not restored (%d)" % img_n)
        print("ok: wallpaper toggles back to image")
        # Save the settings, go back to the Terminal and read the
        # saved file through the shell: it must hold the new accent.
        sendkey("s")
        time.sleep(0.5)
        sendkey("f1")
        time.sleep(0.5)
        os.write(master_fd, b"cat settings.txt\r")
        wait_for("accent=#7C5CFF", 20, "saved settings file")
        print("ok: settings saved to settings.txt")
        wait_for("prismos> ", 20, "shell prompt after cat")
        # Taskbar toggle: 't' moves the taskbar to the top, 't'
        # again puts it back at the bottom.
        sendkey("f3")
        sendkey("t")
        time.sleep(0.8)
        w, h, raw = screendump("06-taskbar-top.ppm")
        # The taskbar's accent line runs along its top edge: when
        # the taskbar is at the top, the first rows are accent
        # (violet now, after the accent change in step 4).
        top_n = count_near(raw, w, h, 0, 0, w, 4, (124, 92, 255), 40)
        if top_n < w // 2:
            raise DesktopTestFailure("taskbar did not move to top")
        print("ok: taskbar moves to top")
        # 't' cycles on: left, right, then back to bottom.
        sendkey("t")
        time.sleep(0.8)
        w, h, raw = screendump("06b-taskbar-left.ppm")
        left_n = count_near(raw, w, h, 117, 0, 121, h,
                            (124, 92, 255), 40)
        if left_n < h // 2:
            raise DesktopTestFailure("taskbar did not move to left")
        print("ok: taskbar moves to left")
        sendkey("t")
        time.sleep(0.8)
        sendkey("t")
        time.sleep(0.8)
        sendkey("f1")
        time.sleep(0.8)
        w, h, raw = screendump("07-back-terminal.ppm")
        print("ok: back to Terminal")

        # 4b. Shortcut editing in Settings: Tab opens the
        #     Shortcuts page. Rebind Browser from F4 to F9 (the
        #     key Photos used: the two swap), save, and check
        #     settings.txt. F9 must then open the Browser, and
        #     resetting to defaults must restore F4.
        sendkey("f3")
        time.sleep(0.8)
        sendkey("tab")
        time.sleep(0.8)
        w, h, raw = screendump("07b-shortcuts.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Shortcuts page not on screen")
        print("ok: Shortcuts page opens")
        # Selection starts at Terminal (index 0); Browser is 3.
        sendkey("down")
        sendkey("down")
        sendkey("down")
        sendkey("ret")  # start key capture
        time.sleep(0.4)
        sendkey("f9")   # captured: Browser <-> Photos swap
        time.sleep(0.5)
        sendkey("s")    # save
        time.sleep(0.5)
        sendkey("f1")
        time.sleep(0.5)
        os.write(master_fd, b"cat settings.txt\r")
        wait_for("bind_browser=F9", 20, "rebound browser key")
        wait_for("bind_photos=F4", 20, "swapped photos key")
        print("ok: shortcut rebound and saved (Browser=F9, Photos=F4)")
        wait_for("prismos> ", 20, "shell prompt after cat")
        # F9 now opens the Browser.
        sendkey("f9")
        time.sleep(0.8)
        w, h, raw = screendump("07c-browser-f9.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("F9 did not open Browser")
        print("ok: F9 opens Browser after rebind")
        # Restore the defaults and save again. (The Settings
        # app is still on the Shortcuts page from before.)
        sendkey("f3")
        time.sleep(0.8)
        sendkey("0")
        time.sleep(0.5)
        sendkey("s")
        time.sleep(0.5)
        sendkey("f1")
        time.sleep(0.5)
        os.write(master_fd, b"cat settings.txt\r")
        wait_for("bind_browser=F4", 20, "restored browser key")
        print("ok: shortcuts reset to defaults")
        wait_for("prismos> ", 20, "shell prompt after cat")

        # 5. F4 opens the Browser. Enter fetches the page from
        #    the local test server (10.0.2.2) and the text must
        #    appear on screen: far more lit pixels in the
        #    content area than the pre-fetch prompt screen.
        sendkey("f4")
        time.sleep(0.8)
        w, h, raw = screendump("08-browser.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Browser title not on screen")
        print("ok: Browser app opens")
        for _i in range(11):
            sendkey("backspace")
        type_text("10.0.2.2:8380/one.html")
        time.sleep(0.8)
        w, h, raw = screendump("08b-browser-typed.ppm")
        before = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                win_x + win_w - 8, win_y + win_h - 60)
        sendkey("ret")
        time.sleep(10)
        w, h, raw = screendump("09-browser-fetched.ppm")
        after = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                               win_x + win_w - 8, win_y + win_h - 60)
        if after <= before + 100:
            raise DesktopTestFailure(
                "fetched page text not on screen (%d -> %d white pixels)"
                % (before, after))
        print("ok: Browser shows fetched page (%d -> %d white pixels)"
              % (before, after))

        # 5b. Typing a different address fetches that page:
        #     erase the current URL, type the second local
        #     page, Enter fetches it.
        for _i in range(40):
            sendkey("backspace")
        type_text("10.0.2.2:8380/two.html")
        time.sleep(0.8)
        w, h, raw = screendump("09b-browser-typed.ppm")
        before2 = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                 win_x + win_w - 8, win_y + win_h - 60)
        sendkey("ret")
        time.sleep(10)
        w, h, raw = screendump("09c-browser-org.ppm")
        after2 = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                win_x + win_w - 8, win_y + win_h - 60)
        if after2 <= before2 + 100:
            raise DesktopTestFailure(
                "typed address not fetched (%d -> %d white pixels)"
                % (before2, after2))
        print("ok: Browser fetches a typed address (%d -> %d white pixels)"
              % (before2, after2))

        # 6. F5 opens Notes. Typed text must appear on screen,
        #    and F6 must save it to note.txt (verified through
        #    the shell afterwards).
        sendkey("f5")
        time.sleep(0.8)
        w, h, raw = screendump("10-notes.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Notes title not on screen")
        print("ok: Notes app opens")
        type_text("Hello Notes\nSecond line")
        time.sleep(0.8)
        w, h, raw = screendump("11-notes-typed.ppm")
        text_n = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                win_x + 600, win_y + 200)
        if text_n < 50:
            raise DesktopTestFailure("typed note not on screen (%d)" % text_n)
        print("ok: Notes shows typed text")
        sendkey("f6")
        time.sleep(0.8)
        sendkey("f1")
        time.sleep(0.5)
        os.write(master_fd, b"cat note.txt\r")
        wait_for("Hello Notes", 20, "saved note content")
        print("ok: Notes saved to note.txt")
        wait_for("prismos> ", 20, "shell prompt after cat note")

        # 6b. Japanese IME in Notes: F9 turns the IME on,
        #     typing "kanji" shows the kana preedit, Space
        #     converts to the first candidate (the kanji for
        #     "kanji"), Enter commits it into the text, F9
        #     turns the IME off again, F6 saves. note.txt
        #     must then contain the committed kanji.
        sendkey("f5")
        time.sleep(0.8)
        sendkey("f9")
        time.sleep(0.5)
        wait_for("notify: IME on", 20, "IME on notification")
        type_text("kanji")
        time.sleep(0.5)
        sendkey("spc")
        time.sleep(0.5)
        sendkey("ret")
        time.sleep(0.5)
        wait_for("ime: commit", 20, "IME commit")
        sendkey("f9")
        time.sleep(0.5)
        sendkey("f6")
        time.sleep(0.8)
        sendkey("f1")
        time.sleep(0.5)
        os.write(master_fd, b"cat note.txt\r")
        wait_for("漢字", 20, "IME kanji in saved note")
        print("ok: IME converts romaji to kanji in Notes")
        wait_for("prismos> ", 20, "shell prompt after IME cat")

        # 7. F7 opens the Calendar. The month grid must show day
        #    numbers, and today must be highlighted with the
        #    current accent (violet after step 4).
        sendkey("f7")
        time.sleep(0.8)
        w, h, raw = screendump("12-calendar.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Calendar title not on screen")
        grid_n = count_whiteish(raw, w, h, win_x + 8, win_y + 60,
                                win_x + 420, win_y + 300)
        if grid_n < 50:
            raise DesktopTestFailure("calendar day numbers not on screen")
        today_n = count_near(raw, w, h, win_x + 8, win_y + 60,
                             win_x + 420, win_y + 300, (124, 92, 255), 40)
        if today_n < 10:
            raise DesktopTestFailure(
                "today not highlighted (%d violet pixels)" % today_n)
        print("ok: Calendar shows month grid and today highlighted")
        sendkey("f1")
        time.sleep(0.8)

        # 8. The mouse moves a cursor on screen, and clicking a
        #    Dock button switches apps. The cursor starts in the
        #    middle of the screen (640, 400); with twenty Dock
        #    buttons (narrowed to fit the screen) the Files
        #    button spans roughly x=75..134 at y~730, so the
        #    click at (120, 730) lands on it.
        mouse_move(-520, 330)
        w, h, raw = screendump("13-mouse-moved.ppm")
        cursor_n = count_whiteish(raw, w, h, 112, 728, 136, 748)
        if cursor_n < 10:
            raise DesktopTestFailure(
                "mouse cursor not at the Files button (%d white pixels)"
                % cursor_n)
        print("ok: mouse cursor moves (%d white pixels at target)"
              % cursor_n)
        mouse_click()
        wait_for("desktop: mouse click -> Files", 20,
                 "mouse click on the Files Dock button")
        print("ok: mouse click switches to Files")
        time.sleep(0.8)
        w, h, raw = screendump("14-mouse-files.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Files title not on screen after click")
        # Click the first file row: it is already selected, so the
        # click opens it and the file content must appear.
        row_y = win_y + 28 + 8 + 2 * 16 + 8
        row_x = win_x + 100
        mouse_move(row_x - 120, row_y - 730)
        mouse_click()
        time.sleep(0.8)
        w, h, raw = screendump("15-mouse-file-open.ppm")
        text_n = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                win_x + 600, win_y + 300)
        if text_n < 50:
            raise DesktopTestFailure(
                "file content not shown after mouse click (%d)" % text_n)
        print("ok: mouse click opens a file")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("f1")
        time.sleep(0.8)

        # 9. F8 opens the search / window list. Typing "cal" must
        #    show the Calendar result, and Enter must open it.
        sendkey("f8")
        time.sleep(0.8)
        w, h, raw = screendump("16-search.ppm")
        search_n = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                  win_x + 600, win_y + 200)
        if search_n < 50:
            raise DesktopTestFailure(
                "search list not on screen (%d)" % search_n)
        print("ok: search list opens")
        type_text("cal")
        time.sleep(0.8)
        w, h, raw = screendump("17-search-cal.ppm")
        cal_n = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                               win_x + 600, win_y + 200)
        if cal_n < 20:
            raise DesktopTestFailure(
                "search results not on screen (%d)" % cal_n)
        print("ok: search filters results")
        sendkey("ret")
        time.sleep(0.8)
        w, h, raw = screendump("18-search-calendar.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Calendar not opened from search")
        print("ok: search opens Calendar")
        sendkey("f1")
        time.sleep(0.8)

        # 10. F9 opens Photos: the .bmp list, then Enter shows the
        #     picture. The sample prism.bmp is red on the left and
        #     green on the right, so those colours must appear in
        #     the window's image area.
        sendkey("f9")
        time.sleep(0.8)
        w, h, raw = screendump("19-photos.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Photos title not on screen")
        print("ok: Photos app opens")
        sendkey("ret")
        time.sleep(0.8)
        w, h, raw = screendump("20-photo-open.ppm")
        text_rows = (win_h - 28 - 16) // 16
        img_y0 = win_y + 28 + 8 + 2 * 16
        img_y1 = win_y + 28 + 8 + (text_rows - 3) * 16
        red_n = count_near(raw, w, h, win_x + 100, img_y0 + 100,
                           win_x + 400, img_y0 + 300, (255, 0, 0), 24)
        green_n = count_near(raw, w, h, win_x + 600, img_y0 + 100,
                             win_x + 900, img_y0 + 300, (0, 200, 0), 24)
        if red_n < 1000 or green_n < 1000:
            raise DesktopTestFailure(
                "photo not drawn (red %d, green %d)" % (red_n, green_n))
        print("ok: photo shown (red %d, green %d pixels)"
              % (red_n, green_n))
        sendkey("esc")
        time.sleep(0.5)
        sendkey("f1")
        time.sleep(0.8)

        # 11. F10 opens Music: the SB16 device must be ready, and
        #     Enter plays melody.wav through it (serial confirms).
        sendkey("f10")
        time.sleep(0.8)
        w, h, raw = screendump("21-music.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Music title not on screen")
        print("ok: Music app opens")
        sendkey("ret")
        wait_for("music: play done", 20, "music playback")
        print("ok: Music plays melody.wav")
        time.sleep(0.5)
        sendkey("f1")
        time.sleep(0.8)

        # 12. F11 opens Cards (blackjack): a new game deals hands,
        #     hit/stand finish it, and a result line appears.
        sendkey("f11")
        time.sleep(0.8)
        w, h, raw = screendump("22-cards.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Cards title not on screen")
        print("ok: Cards app opens (game menu)")
        sendkey("1")
        wait_for("cards: game Blackjack", 20, "blackjack selected")
        sendkey("n")
        time.sleep(0.8)
        w, h, raw = screendump("23-cards-dealt.ppm")
        hand_n = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                win_x + 600, win_y + 120)
        if hand_n < 20:
            raise DesktopTestFailure(
                "blackjack hands not on screen (%d)" % hand_n)
        print("ok: blackjack hands dealt")
        sendkey("h")
        time.sleep(0.5)
        sendkey("s")
        time.sleep(0.8)
        w, h, raw = screendump("24-cards-result.ppm")
        # The result line is at text row 5: text starts at
        # win_y + 28 + 8, so the row is win_y + 116.
        result_n = count_near(raw, w, h, win_x + 8, win_y + 116,
                              win_x + 600, win_y + 132,
                              (124, 92, 255), 40)
        if result_n < 10:
            raise DesktopTestFailure(
                "blackjack result not on screen (%d violet pixels)"
                % result_n)
        print("ok: blackjack result shown")

        # 12b. The other trump games: select each from the
        #      menu, start it, and play one move.
        sendkey("esc")
        time.sleep(0.5)
        sendkey("2")
        wait_for("cards: game Draw Poker", 20, "poker selected")
        wait_for("cards: poker new game", 20, "poker dealt")
        sendkey("d")
        wait_for("cards: poker you=", 20, "poker result")
        print("ok: draw poker plays to a result")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("3")
        wait_for("cards: game Baccarat", 20, "baccarat selected")
        sendkey("n")
        wait_for("cards: baccarat", 20, "baccarat result")
        print("ok: baccarat deals and settles")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("4")
        wait_for("cards: game War", 20, "war selected")
        sendkey("n")
        wait_for("cards: war new game", 20, "war started")
        sendkey("f")
        time.sleep(0.5)
        print("ok: war starts and flips")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("5")
        wait_for("cards: game Old Maid", 20, "old maid selected")
        sendkey("n")
        wait_for("cards: oldmaid new game", 20, "old maid started")
        sendkey("d")
        time.sleep(0.5)
        print("ok: old maid starts and draws")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("6")
        wait_for("cards: game Sevens", 20, "sevens selected")
        sendkey("n")
        wait_for("cards: sevens new game", 20, "sevens started")
        print("ok: sevens starts")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("7")
        wait_for("cards: game Memory", 20, "memory selected")
        sendkey("n")
        wait_for("cards: memory new game", 20, "memory started")
        sendkey("ret")
        time.sleep(0.4)
        print("ok: memory starts and flips")
        sendkey("f1")
        time.sleep(0.8)

        # 13. F12 opens Shooter (3D raycasting game): the view
        #     must show brick walls, the HUD must show the
        #     score, Space must hit the enemy straight ahead
        #     (serial confirms), and walking forward must make
        #     the enemy bigger on screen.
        sendkey("f12")
        time.sleep(1.0)
        w, h, raw = screendump("25-shooter.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            # The first 3D render can lag the keypress on a
            # loaded host; look once more before failing.
            time.sleep(1.5)
            w, h, raw = screendump("25-shooter.ppm")
            title_n = count_whiteish(raw, w, h, win_x + 8,
                                     win_y + 8, win_x + 300,
                                     win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Shooter title not on screen")
        print("ok: Shooter app opens")
        view_y0 = win_y + 28
        view_y1 = win_y + win_h
        # Far wall at the centre: shaded brick (61, 28, 21).
        wall_n = count_near(raw, w, h, win_x, view_y0,
                            win_x + win_w, view_y1, (61, 28, 21), 30)
        if wall_n < 50:
            raise DesktopTestFailure(
                "3D walls not drawn (%d far-wall pixels)" % wall_n)
        print("ok: 3D view drawn (%d far-wall pixels)" % wall_n)
        # HUD row: "Score: 0  Health: 100  Enemies: 6".
        hud_n = count_whiteish(raw, w, h, win_x + 8, win_y + 36,
                               win_x + 600, win_y + 52)
        if hud_n < 20:
            raise DesktopTestFailure("shooter HUD not on screen")
        print("ok: shooter HUD shown")
        # The enemy straight ahead is dark red (139, 26, 26).
        foe_before = count_near(raw, w, h, win_x, view_y0,
                                win_x + win_w, view_y1, (139, 26, 26), 24)
        if foe_before < 100:
            raise DesktopTestFailure(
                "enemy not visible (%d dark red pixels)" % foe_before)
        print("ok: enemy visible ahead (%d pixels)" % foe_before)
        sendkey("spc")
        wait_for("shooter: hit!", 20, "shot hitting the enemy")
        print("ok: shot hits the enemy straight ahead")
        time.sleep(0.5)
        # Walk forward: the enemy must get bigger on screen.
        # Enemies also move on a per-second tick, so a single
        # measurement races the enemy's own movement; use a
        # fresh game per attempt and pass if any attempt
        # shows the approach clearly.
        grew = False
        for _attempt in range(3):
            sendkey("n", 0.3)
            wait_for("shooter: new game", 10, "new game")
            time.sleep(0.6)
            w, h, raw = screendump("26-shooter-closer.ppm")
            base = count_near(raw, w, h, win_x, view_y0,
                              win_x + win_w, view_y1,
                              (139, 26, 26), 24)
            if base < 100:
                continue
            for _ in range(5):
                sendkey("w", 0.15)
            time.sleep(0.8)
            w, h, raw = screendump("26-shooter-closer.ppm")
            foe_after = count_near(raw, w, h, win_x, view_y0,
                                   win_x + win_w, view_y1,
                                   (139, 26, 26), 24)
            if foe_after > base:
                print("ok: enemy bigger after walking "
                      "(%d -> %d pixels)" % (base, foe_after))
                grew = True
                break
        if not grew:
            raise DesktopTestFailure(
                "enemy did not get bigger after walking "
                "(before=%d)" % foe_before)
        sendkey("f1")
        time.sleep(0.8)

        # 14. Ctrl+O opens Sheet (spreadsheet): 10 in A1, 20 in
        #     A2, =SUM(A1:A2) in A3, and the computed 30 must
        #     appear in the A3 cell. Saving to sheet.csv and
        #     loading it back must keep the values.
        sendkey("ctrl-o")
        time.sleep(0.8)
        w, h, raw = screendump("27-sheet.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Sheet title not on screen")
        print("ok: Sheet app opens")
        type_text("10")
        sendkey("ret")
        sendkey("down")
        type_text("20")
        sendkey("ret")
        sendkey("down")
        type_sheet_text("=SUM(A1:A2)")
        sendkey("ret")
        time.sleep(0.8)
        w, h, raw = screendump("28-sheet-sum.ppm")
        # A3 cell: column A starts at win_x+8+40, row 3 is the
        # third grid row (formula bar + header above it).
        cell_x0 = win_x + 8 + 40
        cell_y0 = win_y + 28 + 8 + 2 * 16 + 2 * 16
        sum_n = count_whiteish(raw, w, h, cell_x0, cell_y0,
                               cell_x0 + 90, cell_y0 + 16)
        if sum_n < 10:
            raise DesktopTestFailure(
                "SUM result not in A3 (%d white pixels)" % sum_n)
        print("ok: sheet computes SUM(A1:A2) = 30")
        sendkey("s")
        wait_for("sheet: saved sheet.csv (3 cells)", 20, "sheet save")
        print("ok: sheet saved to sheet.csv")
        sendkey("l")
        wait_for("sheet: loaded sheet.csv (3 cells)", 20, "sheet load")
        time.sleep(0.5)
        w, h, raw = screendump("29-sheet-loaded.ppm")
        sum_n = count_whiteish(raw, w, h, cell_x0, cell_y0,
                               cell_x0 + 90, cell_y0 + 16)
        if sum_n < 10:
            raise DesktopTestFailure(
                "SUM result gone after reload (%d white pixels)" % sum_n)
        print("ok: sheet values survive save + load")
        sendkey("f1")
        time.sleep(0.8)

        # 15. Ctrl+V opens Video: the 24 vid-*.pv1 frames (320x240,
        #     compressed, each with a 2000-sample audio track)
        #     play at 4 fps. The first frame's ball must be drawn,
        #     Space must start playback (the ball moves), and 'r'
        #     + 'n' must seek deterministically.
        sendkey("ctrl-v")
        wait_for("video: 24 frames", 20, "video frame list")
        print("ok: Video found 24 frames")
        time.sleep(0.8)
        w, h, raw = screendump("30-video.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Video title not on screen")
        print("ok: Video app opens")
        # The frame is drawn 1.2x (320x240 -> 384x288) in the
        # window: image x = win_x+8 + (982-384)/2, y = win_y+36+32.
        img_x = win_x + 8 + (win_w - 16 - 384) // 2
        img_y = win_y + 28 + 8 + 2 * 16
        # Frame 1: the ball is at (40, 200) in the 320x240 frame,
        # so on screen around (img_x+48, img_y+240).
        ball1_n = count_near(raw, w, h, img_x + 24, img_y + 216,
                             img_x + 72, img_y + 264, (76, 194, 255), 30)
        if ball1_n < 20:
            raise DesktopTestFailure(
                "first video frame not drawn (%d ball pixels)" % ball1_n)
        print("ok: first video frame drawn (%d ball pixels)" % ball1_n)
        sendkey("spc")
        wait_for("video: play", 20, "video play")
        time.sleep(2.5)
        sendkey("spc")
        wait_for("video: pause", 20, "video pause")
        time.sleep(0.5)
        w, h, raw = screendump("31-video-played.ppm")
        ball1_after = count_near(raw, w, h, img_x + 24, img_y + 216,
                                 img_x + 72, img_y + 264,
                                 (76, 194, 255), 30)
        if ball1_after >= ball1_n:
            raise DesktopTestFailure(
                "ball did not move during playback (%d -> %d)"
                % (ball1_n, ball1_after))
        print("ok: video plays, ball moved (%d -> %d pixels)"
              % (ball1_n, ball1_after))
        sendkey("r")
        time.sleep(0.8)
        w, h, raw = screendump("32-video-restart.ppm")
        ball1_n = count_near(raw, w, h, img_x + 24, img_y + 216,
                             img_x + 72, img_y + 264, (76, 194, 255), 30)
        if ball1_n < 20:
            raise DesktopTestFailure(
                "restart did not return to frame 1 (%d)" % ball1_n)
        print("ok: video restart returns to frame 1")
        sendkey("n")
        time.sleep(0.8)
        w, h, raw = screendump("33-video-next.ppm")
        # Frame 2: the ball is at (54, 188), so on screen around
        # (img_x+65, img_y+226).
        ball2_n = count_near(raw, w, h, img_x + 41, img_y + 202,
                             img_x + 89, img_y + 250, (76, 194, 255), 30)
        if ball2_n < 20:
            raise DesktopTestFailure(
                "next frame not shown (%d ball pixels)" % ball2_n)
        print("ok: video seeks to the next frame")
        sendkey("f1")
        time.sleep(0.8)

        # 16. Ctrl+I opens the Installer. This test boots with
        #     no disk attached, so the disk step must honestly
        #     say no disk was found, and Esc must go back.
        sendkey("ctrl-i")
        wait_for("installer: opened", 20, "installer opened")
        print("ok: Installer app opens")
        time.sleep(0.8)
        w, h, raw = screendump("34-installer.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Installer title not on screen")
        sendkey("ret")
        wait_for("installer: step disk", 20, "installer disk step")
        time.sleep(0.8)
        w, h, raw = screendump("35-installer-nodisk.ppm")
        nodisk_n = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                  win_x + 600, win_y + 200)
        if nodisk_n < 20:
            raise DesktopTestFailure(
                "no-disk message not on screen (%d)" % nodisk_n)
        print("ok: Installer reports no disk honestly")
        sendkey("esc")
        time.sleep(0.8)
        sendkey("f1")
        time.sleep(0.8)

        # 17. Ctrl+M opens Mail. With no account configured
        #     the app opens its setup form (batch 3 item 4);
        #     fill From + SMTP server, save, then the compose
        #     form ('c') must take typed text in the To /
        #     Subject / Body fields.
        sendkey("ctrl-m")
        wait_for("mail: setup form opened", 20, "setup form")
        wait_for("mail: opened", 20, "mail opened")
        print("ok: Mail app opens")
        time.sleep(0.8)
        w, h, raw = screendump("36-mail.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Mail title not on screen")
        print("ok: Mail setup form opens when unconfigured")
        type_text("me")
        sendkey("tab")
        type_text("10.0.2.2")
        for _ in range(11):
            sendkey("tab", 0.25)
        sendkey("ret")
        wait_for("mail: setup saved", 20, "setup saved")
        print("ok: Mail setup form saved the account")
        sendkey("c")
        time.sleep(0.5)
        type_text("friend")
        sendkey("tab")
        type_text("Hello")
        sendkey("tab")
        type_text("body text")
        time.sleep(0.8)
        w, h, raw = screendump("37-mail-compose.ppm")
        to_n = count_whiteish(raw, w, h, win_x + 8, win_y + 64,
                              win_x + 400, win_y + 84)
        if to_n < 10:
            raise DesktopTestFailure(
                "typed To not on screen (%d)" % to_n)
        subj_n = count_whiteish(raw, w, h, win_x + 8, win_y + 80,
                                win_x + 400, win_y + 100)
        if subj_n < 10:
            raise DesktopTestFailure(
                "typed Subject not on screen (%d)" % subj_n)
        body_n = count_whiteish(raw, w, h, win_x + 8, win_y + 128,
                                win_x + 400, win_y + 148)
        if body_n < 10:
            raise DesktopTestFailure(
                "typed Body not on screen (%d)" % body_n)
        print("ok: Mail compose takes typed text")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("esc")
        time.sleep(0.5)
        sendkey("f1")
        time.sleep(0.8)

        # 17b. Mail reply / forward / search: write a
        #      message file with the Editor, open it in
        #      Mail, reply (To is prefilled from From),
        #      forward, and filter the list by keyword.
        sendkey("ctrl-t")
        time.sleep(0.8)
        sendkey("ctrl-g")
        time.sleep(0.5)
        for _ in range(10):
            sendkey("backspace")
        type_text("mail")
        sendkey("minus")
        type_text("0001.txt")
        sendkey("ret")
        time.sleep(0.5)
        type_text("From: alice")
        sendkey("shift-2")
        type_text("example.com\nSubject: Lunch plans\n\n"
                  "Let us meet at noon.")
        sendkey("f6")
        wait_for("editor: saved", 20, "message file saved")
        sendkey("f1")
        time.sleep(0.8)
        os.write(master_fd, b"ls\r")
        wait_for("mail-0001.txt", 20, "message file listed")
        print("ok: message file created with the right name")
        sendkey("ctrl-m")
        wait_for("mail: opened", 20, "mail reopened")
        time.sleep(0.8)
        sendkey("ret")
        time.sleep(0.8)
        sendkey("r")
        wait_for("mail: reply", 20, "reply starts")
        time.sleep(0.8)
        w, h, raw = screendump("37b-mail-reply.ppm")
        to_n = count_whiteish(raw, w, h, win_x + 8, win_y + 64,
                              win_x + 400, win_y + 84)
        if to_n < 10:
            raise DesktopTestFailure(
                "reply To not prefilled (%d)" % to_n)
        print("ok: mail reply prefills the compose form")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("ret")
        time.sleep(0.8)
        sendkey("f")
        wait_for("mail: forward", 20, "forward starts")
        print("ok: mail forward starts")
        sendkey("esc")
        time.sleep(0.5)
        sendkey("slash")
        type_text("alice")
        sendkey("ret")
        wait_for("mail: filter alice", 20, "search filter")
        print("ok: mail search filters the list")
        sendkey("f1")
        time.sleep(0.8)

        # 18. Ctrl+D opens Docs: the sample doc.txt loads, its
        #     "# Welcome to Docs" heading must draw big in the
        #     accent colour (violet after step 4). Typing text
        #     and "\" + "1" must make it a heading, F6 must save
        #     it to doc.txt, Ctrl+E must export a real doc.docx,
        #     and Ctrl+R must import it back.
        sendkey("ctrl-d")
        time.sleep(0.8)
        w, h, raw = screendump("38-docs.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Docs title not on screen")
        print("ok: Docs app opens")
        h1_n = count_near(raw, w, h, win_x + 8, win_y + 68,
                          win_x + 400, win_y + 100, (124, 92, 255), 40)
        if h1_n < 10:
            raise DesktopTestFailure(
                "Docs heading not drawn in accent (%d violet pixels)"
                % h1_n)
        print("ok: Docs heading drawn (%d violet pixels)" % h1_n)
        type_text("Hello Docs")
        sendkey("backslash")
        sendkey("1")
        time.sleep(0.8)
        w, h, raw = screendump("39-docs-heading.ppm")
        sendkey("f6")
        wait_for("docs: saved doc.txt", 20, "docs save")
        print("ok: Docs saves typed text with a heading")
        sendkey("f1")
        time.sleep(0.5)
        type_sheet_text("cat doc.txt\n")
        wait_for("# Hello Docs", 20, "heading in saved doc.txt")
        print("ok: saved doc.txt has the new heading")
        sendkey("ctrl-d")
        time.sleep(0.5)
        sendkey("ctrl-e")
        wait_for("docs: exported doc.docx", 20, "docs export")
        print("ok: Docs exports doc.docx")
        sendkey("f1")
        time.sleep(0.5)
        type_text("ls\n")
        wait_for("doc.docx", 20, "doc.docx in file list")
        print("ok: doc.docx is in the file list")
        sendkey("ctrl-d")
        time.sleep(0.5)
        type_text("ZZZ")
        sendkey("ctrl-r")
        wait_for("docs: imported doc.docx", 20, "docs import")
        print("ok: Docs imports doc.docx")
        sendkey("f1")
        time.sleep(0.8)

        # 19. Ctrl+P opens Slides: the sample slides.txt loads.
        #     Tab to the empty bullet slot, type a bullet, F6
        #     saves it. Ctrl+P again starts the slideshow,
        #     Right moves to slide 2, Esc leaves it. Ctrl+E
        #     exports a real slides.pptx.
        sendkey("ctrl-p")
        time.sleep(0.8)
        w, h, raw = screendump("40-slides.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 300, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Slides title not on screen")
        print("ok: Slides app opens")
        sendkey("tab")
        sendkey("tab")
        sendkey("tab")
        type_text("new point")
        sendkey("ret")
        sendkey("f6")
        wait_for("slides: saved slides.txt", 20, "slides save")
        print("ok: Slides saves a new bullet")
        sendkey("f1")
        time.sleep(0.5)
        type_sheet_text("cat slides.txt\n")
        wait_for("new point", 20, "new bullet in saved slides.txt")
        print("ok: saved slides.txt has the new bullet")
        sendkey("ctrl-p")
        time.sleep(0.8)
        w, h, raw = screendump("41-slideshow.ppm")
        # The first Ctrl+P only opens Slides; the second one
        # (inside Slides) starts the slideshow.
        sendkey("ctrl-p")
        time.sleep(0.8)
        w, h, raw = screendump("41-slideshow.ppm")
        show_n = count_near(raw, w, h, win_x + 8, win_y + 60,
                            win_x + 400, win_y + 100, (124, 92, 255), 40)
        if show_n < 10:
            raise DesktopTestFailure(
                "slideshow title not drawn (%d violet pixels)" % show_n)
        print("ok: slideshow shows slide 1")
        sendkey("right")
        time.sleep(0.8)
        w, h, raw = screendump("42-slideshow-2.ppm")
        show2_n = count_near(raw, w, h, win_x + 8, win_y + 60,
                             win_x + 400, win_y + 100, (124, 92, 255), 40)
        if show2_n < 10:
            raise DesktopTestFailure(
                "slideshow slide 2 not drawn (%d violet pixels)" % show2_n)
        print("ok: slideshow moves to slide 2")
        sendkey("esc")
        time.sleep(0.8)
        sendkey("ctrl-e")
        wait_for("slides: exported slides.pptx", 20, "slides export")
        print("ok: Slides exports slides.pptx")
        sendkey("f1")
        time.sleep(0.5)
        type_text("ls\n")
        wait_for("slides.pptx", 20, "slides.pptx in file list")
        print("ok: slides.pptx is in the file list")
        sendkey("f1")
        time.sleep(0.8)

        # 20. Phase 14: allowlist + Settings security summary.
        #     Set a PIN in the Terminal, deny Music, F10 must be
        #     refused with an on-screen notice; allow it again
        #     and F10 must open Music. Settings (F3) must show
        #     the security summary lines (rows 11-12).
        type_text("passwd\n")
        wait_for("New PIN", 20, "new PIN prompt")
        type_text("1234\n")
        wait_for("Confirm PIN", 20, "confirm PIN prompt")
        type_text("1234\n")
        wait_for("Account set", 20, "account set")
        print("ok: PIN set in desktop test")
        type_text("security deny music\n")
        wait_for("Admin PIN: ", 20, "admin PIN prompt")
        type_text("1234\n")
        wait_for("Denied: music", 20, "music denied")
        print("ok: Music denied by allowlist")
        sendkey("f10")
        wait_for("desktop: app 'Music' is not allowed by the administrator.",
                 20, "denied app refused")
        time.sleep(0.8)
        w, h, raw = screendump("43-denied.ppm")
        # The notice is drawn in the accent colour (violet) on
        # the last text row of the window.
        notice_n = count_near(raw, w, h, 0, win_y + win_h - 48,
                              w, win_y + win_h, (124, 92, 255), 40)
        if notice_n < 10:
            raise DesktopTestFailure(
                "denial notice not on screen (%d violet pixels)"
                % notice_n)
        print("ok: denied app shows a notice on screen")
        sendkey("f1")
        time.sleep(0.5)
        type_text("security allow music\n")
        wait_for("Admin PIN: ", 20, "admin PIN prompt 2")
        type_text("1234\n")
        wait_for("Allowed: music", 20, "music allowed again")
        print("ok: Music allowed again")
        sendkey("f10")
        time.sleep(0.8)
        w, h, raw = screendump("44-music-allowed.ppm")
        title_n = count_whiteish(raw, w, h, win_x + 8, win_y + 8,
                                 win_x + 200, win_y + 24)
        if title_n < 20:
            raise DesktopTestFailure("Music did not open after allow")
        print("ok: Music opens after allow")
        sendkey("f1")
        time.sleep(0.5)
        sendkey("f3")
        time.sleep(0.8)
        w, h, raw = screendump("45-settings-security.ppm")
        # Security summary: text_y() + 11*CELL_H = win_y + 212,
        # row 12 at win_y + 228.
        sec_n = count_whiteish(raw, w, h, win_x + 8, win_y + 212,
                               win_x + 700, win_y + 244)
        if sec_n < 10:
            raise DesktopTestFailure(
                "security summary not on Settings screen (%d)" % sec_n)
        print("ok: Settings shows the security summary")
        sendkey("f1")
        time.sleep(0.8)

        # 21. Phase 16: compat layer. `run hello.elf` in the
        #     Terminal must print the guest greeting and exit
        #     with code 0. Then F2 opens Files, 32 rows down is
        #     hello.elf (built-in order), Enter runs it from
        #     the file manager and switches to the Terminal,
        #     where the greeting must be on screen.
        type_sheet_text("run hello.elf\n")
        wait_for("Hello from the guest app!", 20, "guest greeting")
        wait_for("compat: hello.elf exited with code 0.", 20,
                 "guest exit code 0")
        print("ok: run hello.elf in the Terminal")
        # Ask the shell itself where hello.elf sits in the
        # file list: earlier steps create and delete files,
        # so a hardcoded row drifts. `ls` prints the VFS in
        # the same order the Files app shows it.
        ls_start = pos
        type_sheet_text("ls\n")
        # The prompt printed BEFORE the ls output is also
        # in this segment, so anchor on the listing itself
        # first, then on the prompt that follows it.
        wait_for("hello.elf", 20, "ls lists hello.elf")
        wait_for("prismos> ", 20, "prompt after ls")
        seg = bytes(buf[ls_start:pos]).decode("utf-8",
                                              "replace")
        lines = [ln.strip() for ln in seg.replace("\r", "")
                 .split("\n")]
        names = []
        seen_first = False
        for ln in lines:
            if ln == "README.txt":
                seen_first = True
            if seen_first and ln and " " not in ln and \
                    ">" not in ln:
                names.append(ln)
        # Files はファイル行だけ名前順に並べ替える
        # (バッチ3 項目13) ので、GUI の行順に直してから
        # hello.elf の位置を求める。
        files_sorted = sorted(n for n in names
                              if not n.endswith("/"))
        fit = iter(files_sorted)
        gui_names = [n if n.endswith("/") else next(fit)
                     for n in names]
        hello_idx = gui_names.index("hello.elf")
        print("info: hello.elf is file index %d of %d"
              % (hello_idx, len(gui_names)))
        sendkey("f2")
        time.sleep(0.8)
        # Spacing matters: every keypress redraws the file
        # list in the guest, and faster sending overflows
        # the guest keyboard buffer and drops keys.
        for _ in range(hello_idx + 40):
            sendkey("up", 0.12)
        for _ in range(hello_idx):
            sendkey("down", 0.12)
        sendkey("ret")
        wait_for("Hello from the guest app!", 20,
                 "guest greeting from Files")
        wait_for("compat: hello.elf exited with code 0.", 20,
                 "guest exit code 0 from Files")
        time.sleep(0.8)
        w, h, raw = screendump("46-compat-files.ppm")
        greet_n = count_whiteish(raw, w, h, win_x + 8, win_y + 44,
                                 win_x + 700, win_y + 300)
        if greet_n < 50:
            raise DesktopTestFailure(
                "guest output not on Terminal screen (%d)" % greet_n)
        print("ok: Files runs hello.elf and shows its output")

        # 16. UI wave: notifications, Dock settings, language,
        #     clipboard, the launcher + calculator window and
        #     the right-click context menu.
        def ensure_settings_page0():
            # The Settings page persists between visits and a
            # single Tab keypress can be dropped by the guest,
            # so probe with the page-0-only 'g' key (language
            # toggle notifies) and Tab until it answers. The
            # probe toggles the language twice = no net change.
            # The notify is in the NEW language (that is the
            # localisation working), so accept both forms.
            lang_needles = ["notify: Language:", "notify: 言語"]
            for _attempt in range(9):
                type_text("g")
                try:
                    wait_for_any(lang_needles, 6, "page 0 probe")
                except DesktopTestFailure:
                    sendkey("tab")
                    time.sleep(0.5)
                    continue
                type_text("g")
                wait_for_any(lang_needles, 6, "page 0 probe back")
                return
            raise DesktopTestFailure("Settings page 0 unreachable")

        # 16a. Settings save raises a notification toast, and
        #      the new Dock/language keys persist to
        #      settings.txt.
        sendkey("f3")
        time.sleep(0.8)
        # Step 4b left Settings on the shortcuts page; get
        # back to the appearance page (probed, not assumed).
        sendkey("tab")
        time.sleep(0.5)
        ensure_settings_page0()
        type_text("s")
        wait_for("notify: Settings saved", 20,
                 "settings saved notification")
        print("ok: notification toast on settings save")
        type_text("pzg")
        type_text("s")
        # The language is Japanese now (the 'g' above), so
        # the save notification itself is in Japanese --
        # that is the GUI localisation working as intended.
        wait_for("notify: \u8a2d\u5b9a\u3092\u4fdd\u5b58\u3057"
                 "\u307e\u3057\u305f", 20,
                 "second settings save (Japanese notify)")
        sendkey("f1")
        time.sleep(0.8)
        os.write(master_fd, b"cat settings.txt\r")
        wait_for("dock_pos=left", 20, "dock_pos in settings.txt")
        wait_for("dock_size=large", 20, "dock_size in settings.txt")
        wait_for("language=ja", 20, "language in settings.txt")
        print("ok: Dock position/size and language settings save")

        # 16d2. With the language set to Japanese, the GUI
        #       itself switches: opening Files from the
        #       launcher logs the Japanese app name (the
        #       same string the window title, the Dock
        #       label and the launcher cell all use).
        mouse_to(134, 772)
        mouse_click()
        wait_for("launcher: opened", 20, "launcher opens (JA)")
        # Files はセル index 1 (col 1, row 0)。項目数が
        # 30 (アプリ21+窓3+ウィジェット1+電源5) になり
        # パネルが 8 行になった現在の中心座標。
        mouse_to(560, 218)
        mouse_click()
        wait_for("launcher: \u30d5\u30a1\u30a4\u30eb",
                 20, "Japanese GUI app name")
        print("ok: GUI switches to Japanese (app names in "
              "Japanese)")
        sendkey("f3")
        time.sleep(0.8)
        type_text("pzg")
        type_text("s")
        wait_for("notify: Settings saved", 20,
                 "settings restored save")
        sendkey("f1")
        time.sleep(0.8)

        # 16b. Clipboard in Notes: type a line, copy it with
        #      Ctrl+C, paste it with Ctrl+V on a new line, save
        #      and check the file holds the line twice.
        sendkey("f5")
        time.sleep(0.8)
        # No trailing newline: the cursor must stay on the
        # typed line for Ctrl+C to copy it.
        type_text("hello clipboard")
        sendkey("ctrl-c")
        wait_for("notify: Line copied", 20, "copy notification")
        sendkey("ret")
        sendkey("ctrl-v")
        time.sleep(0.5)
        sendkey("f6")
        time.sleep(0.8)
        sendkey("f1")
        time.sleep(0.8)
        mark = len(buf)
        os.write(master_fd, b"cat note.txt\r")
        wait_for("hello clipboard", 20, "note content")
        # The file prints slowly through the console path;
        # count only after the prompt that ends the output.
        wait_for("prismos> ", 20, "prompt after note cat")
        pump(0.5)
        occurrences = bytes(buf[mark:]).count(b"hello clipboard")
        if occurrences < 2:
            raise DesktopTestFailure(
                "clipboard paste did not duplicate the line "
                "(%d occurrences)" % occurrences)
        print("ok: clipboard copy/paste in Notes (%d lines)"
              % occurrences)

        # 16c. Launcher: click Apps on the taskbar, open the
        #      Calculator window, run 7+5= and close it.
        mouse_to(134, 772)
        mouse_click()
        wait_for("launcher: opened", 20, "launcher opens")
        # 16c-icons: the launcher cells and the Dock draw
        # per-app pixel icons. The Music note purple and
        # the Files folder yellow are distinctive colours
        # that only the icons use at this size.
        _w, _h, raw = screendump("16c-icons.ppm")
        purple = count_near(raw, _w, _h, 0, 0, _w, _h,
                            (0x8E, 0x4E, 0xC6))
        yellow = count_near(raw, _w, _h, 0, 0, _w, _h,
                            (0xFF, 0xD6, 0x0A))
        if purple < 40 or yellow < 40:
            raise DesktopTestFailure(
                "app icons missing (purple=%d yellow=%d)"
                % (purple, yellow))
        print("ok: per-app icons drawn (music purple %d, "
              "folder yellow %d)" % (purple, yellow))
        # Calculator は窓項目の index 22 (アプリ21 の次、
        # kind 1)。col 2, row 5 の中心は (720, 498)。
        mouse_move(720 - 134, 498 - 772)
        mouse_click()
        wait_for("window: opened Calculator", 20,
                 "calculator window opens")
        sendkey("7")
        sendkey("shift-equal")
        sendkey("5")
        sendkey("equal")
        wait_for("calc: result 12", 20, "calculator result")
        print("ok: launcher opens Calculator window, 7+5=12")
        sendkey("esc")
        wait_for("window: closed Calculator", 20,
                 "calculator window closes")

        # 16d. Right-click on the desktop background opens the
        #      context menu; its first item opens Settings.
        mouse_to(1100, 200)
        hmp("mouse_button 2")
        time.sleep(0.5)
        hmp("mouse_button 0")
        wait_for("menu: desktop", 20, "context menu opens")
        mouse_move(88, 18)
        mouse_click()
        wait_for("menu: Settings", 20, "menu Settings item")
        print("ok: right-click context menu opens Settings")
        sendkey("f1")
        time.sleep(0.8)

        # 16e. JIS keyboard layout: switch in Settings, then
        #      Shift+2 types a double quote (US gives @).
        sendkey("f3")
        time.sleep(0.8)
        ensure_settings_page0()
        type_text("j")
        wait_for("notify: Keyboard: JIS", 20, "JIS layout notify")
        type_text("s")
        wait_for("notify: Settings saved", 20, "JIS saved")
        sendkey("f5")
        time.sleep(0.8)
        # New line first: note.txt already has content and
        # the cursor may sit at the end of an existing line;
        # the quote must land on a line of its own.
        sendkey("ret")
        sendkey("shift-2")
        sendkey("f6")
        time.sleep(0.8)
        sendkey("f1")
        time.sleep(0.8)
        mark = len(buf)
        os.write(master_fd, b"cat note.txt\r")
        # wait_for pumps the serial stream; a bare sleep
        # would leave the output unread in the pty.
        wait_for("hello clipboard", 20, "note content (JIS)")
        wait_for("prismos> ", 20, "prompt after note cat (JIS)")
        pump(0.5)
        seg = bytes(buf[mark:])
        # Lines in the serial stream are separated by
        # \r\n with an extra \r before each line; compare
        # per line instead of assuming a byte pattern.
        jis_lines = [ln.strip(b"\r ") for ln in seg.split(b"\n")]
        if b'"' not in jis_lines:
            raise DesktopTestFailure(
                "JIS Shift+2 did not produce a double quote")
        print("ok: JIS layout types Shift+2 as a double quote")
        # Restore the US layout.
        sendkey("f3")
        time.sleep(0.8)
        ensure_settings_page0()
        type_text("j")
        wait_for("notify: Keyboard: US", 20, "US layout notify")
        type_text("s")
        wait_for("notify: Settings saved", 20, "US saved")
        sendkey("f1")
        time.sleep(0.8)

        # 17. The four added apps: Contacts adds and saves a
        #     contact, the Editor types and saves a file,
        #     Paint paints a cell and saves a BMP, and the
        #     Store opens (serial markers confirm each).
        sendkey("ctrl-b")
        time.sleep(0.8)
        type_text("n")
        type_text("Ada")
        sendkey("ret")
        type_text("5550100")
        sendkey("ret")
        wait_for("contacts: saved", 20, "contact saved")
        print("ok: contacts adds and saves a contact")
        sendkey("ctrl-t")
        time.sleep(0.8)
        type_text("hello editor")
        sendkey("f6")
        wait_for("editor: saved", 20, "editor saved")
        print("ok: editor types and saves a file")
        sendkey("ctrl-a")
        time.sleep(0.8)
        sendkey("spc")
        wait_for("paint: cell painted", 20, "paint cell")
        sendkey("f6")
        wait_for("paint: saved paint.bmp", 20, "paint saved")
        print("ok: paint paints and saves a BMP")
        sendkey("ctrl-k")
        wait_for("store-ui: opened", 20, "store opens")
        print("ok: store app opens")

        # 17c. Undo in the Editor, and find/replace + undo
        #      in Notes (note.txt currently holds two
        #      "hello clipboard" lines from step 16b).
        sendkey("ctrl-t")
        time.sleep(0.8)
        sendkey("ctrl-z")
        wait_for("editor: undo", 20, "editor undo")
        print("ok: editor undo works")
        sendkey("f5")
        time.sleep(0.8)
        sendkey("ctrl-h")
        time.sleep(0.5)
        type_text("clipboard")
        sendkey("ret")
        type_text("CLIPBOARD")
        sendkey("ret")
        wait_for("notes: replaced 2 occurrence(s)", 20,
                 "notes replace all")
        print("ok: notes find/replace replaces all matches")
        sendkey("ctrl-z")
        wait_for("notes: undo", 20, "notes undo")
        print("ok: notes undo works")
        sendkey("f1")
        time.sleep(0.8)

        print("DESKTOP TEST PASSED")
        return 0
    except DesktopTestFailure as e:
        print("MISSING: %s" % e)
        failures.append(str(e))
    except OSError as e:
        print("MISSING: QEMU communication error: %s" % e)
        failures.append(str(e))
    finally:
        if monitor_sock is not None:
            try:
                monitor_sock.close()
            except OSError:
                pass
            try:
                os.unlink(MONITOR_SOCK)
            except OSError:
                pass
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        os.close(master_fd)
        log_file.close()
        stderr_file.close()
    if failures:
        print("BOOT LOG (%s):" % args.log)
        try:
            with open(args.log, "rb") as f:
                sys.stdout.write(f.read().decode("utf-8", "replace"))
        except OSError:
            pass
        print("DESKTOP TEST FAILED")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
