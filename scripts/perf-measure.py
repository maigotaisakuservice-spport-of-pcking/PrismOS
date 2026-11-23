#!/usr/bin/env python3
"""PrismOS performance measurement (spec section 5.1).

Measures, on the standard QEMU test machine (256 MB):
  1. boot time: QEMU start -> shell prompt, and -> the
     "desktop: ready" serial line,
  2. app launch time: F2 (Files) and F4 (Browser) key
     press -> the app title visible in a screendump
     (polling every ~0.2 s, so values are upper bounds),
  3. idle memory: the shell `mem` command output at the
     desktop with no apps open.

Prints the numbers; docs/PERF_MEASUREMENT_*.md records
them. Exit code 0 when all measurements were taken.
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
ISO = os.path.join(ROOT, "build", "prismos.iso")
MON = os.path.join(ROOT, "build", "qemu-monitor-perf.sock")
SHOT = os.path.join(ROOT, "build", "shots", "perf.ppm")


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
        parts.append(rest[i:j])
        i = j
    w, h = int(parts[0]), int(parts[1])
    i += 1
    return w, h, rest[i:i + w * h * 3]


def title_lit(raw, w, x0, y0):
    """White-ish pixel count over the whole window area
    (title + content): an app switch changes it a lot."""
    n = 0
    for y in range(y0, y0 + 420, 2):
        for x in range(x0, x0 + 960, 2):
            i = (y * w + x) * 3
            if raw[i] > 200 and raw[i + 1] > 200 and raw[i + 2] > 200:
                n += 1
    return n


def main():
    if not os.path.exists(ISO):
        print("MISSING ISO")
        return 1
    try:
        os.unlink(MON)
    except OSError:
        pass
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-m", "256M",
            "-serial", "stdio",
            "-monitor", "unix:%s,server,nowait" % MON,
            "-display", "vnc=:12",
            "-netdev", "user,id=net0",
            "-device", "e1000,netdev=net0",
            "-no-reboot"]
    master, slave = pty.openpty()
    tty.setraw(slave)
    at = termios.tcgetattr(slave)
    at[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave, termios.TCSANOW, at)
    t0 = time.monotonic()
    proc = subprocess.Popen(args, stdin=slave, stdout=slave,
                            stderr=subprocess.DEVNULL, cwd=ROOT)
    os.close(slave)
    buf = bytearray()
    marks = {}

    def pump_until(needle, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            i = buf.find(needle.encode())
            if i >= 0:
                return time.monotonic()
            r, _, _ = select.select([master], [], [], 0.2)
            if not r:
                continue
            try:
                chunk = os.read(master, 4096)
            except OSError:
                return None
            if not chunk:
                return None
            buf.extend(chunk)
        return None

    t_prompt = pump_until("prismos> ", 120)
    marks["shell_prompt_s"] = (t_prompt - t0) if t_prompt else None
    t_ready = pump_until("desktop: ready", 30)
    marks["desktop_ready_s"] = (t_ready - t0) if t_ready else None

    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(MON)
    mon.recv(4096)

    def hmp(cmd):
        mon.sendall((cmd + "\n").encode())
        d = b""
        while b"(qemu)" not in d:
            c = mon.recv(4096)
            if not c:
                break
            d += c

    def launch_time(key):
        """Press an F key; poll screendumps until the window
        title area changes substantially."""
        hmp("screendump %s" % SHOT)
        time.sleep(0.4)
        w, h, raw = read_ppm(SHOT)
        # Window geometry matches desktop-test: win at
        # ((w-1000)/2, 128) approx; find the title by using
        # the known layout from desktop-test (win_x, win_y).
        win_w = 1000
        win_x = (w - win_w) // 2
        win_y = 128
        base = title_lit(raw, w, win_x + 8, win_y + 8)
        start = time.monotonic()
        hmp("sendkey %s" % key)
        while time.monotonic() - start < 6:
            time.sleep(0.2)
            hmp("screendump %s" % SHOT)
            time.sleep(0.15)
            w2, h2, raw2 = read_ppm(SHOT)
            if abs(title_lit(raw2, w2, win_x + 8, win_y + 8) - base) > 30:
                return time.monotonic() - start
        return None

    marks["files_launch_s"] = launch_time("f2")
    hmp("sendkey f1")
    time.sleep(0.8)
    marks["browser_launch_s"] = launch_time("f4")
    hmp("sendkey f1")
    time.sleep(0.8)

    # Idle memory via the shell `mem` command (only the
    # output produced after the command was sent).
    mark = len(buf)
    os.write(master, b"mem\r")
    end = time.monotonic() + 8
    while time.monotonic() < end and b"used" not in bytes(buf[mark:]):
        r, _, _ = select.select([master], [], [], 0.3)
        if r:
            try:
                chunk = os.read(master, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf.extend(chunk)
    text = bytes(buf[mark:]).decode("utf-8", "replace")
    mem_lines = [ln.strip() for ln in text.splitlines()
                 if "memory:" in ln or "heap:" in ln]
    marks["mem_lines"] = mem_lines

    for k, v in marks.items():
        print("%s: %s" % (k, v))
    proc.terminate()
    ok = all(marks[k] is not None for k in
             ("shell_prompt_s", "desktop_ready_s",
              "files_launch_s", "browser_launch_s"))
    print("PERF MEASURED" if ok else "PERF INCOMPLETE")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
