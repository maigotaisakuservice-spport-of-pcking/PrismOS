#!/usr/bin/env python3
"""PrismOS virtio tablet test: boot with a virtio-tablet-pci
and inject absolute-pointer events through the QEMU
monitor (input-send-event). The driver must report the
tablet ready, and the `tablet` shell command must show the
injected raw coordinates and button state."""
import os, pty, re, select, socket, subprocess, sys, termios
import time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
LOG = os.path.join(ROOT, "build", "boot-tablet.log")
MON = "/tmp/prism-tablet-qmp.sock"


def main():
    if os.path.exists(MON):
        os.unlink(MON)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-serial", "stdio", "-display",
            "vnc=:12", "-qmp", "unix:%s,server,nowait" % MON,
            "-no-reboot", "-device", "virtio-tablet-pci,id=tab0"]
    master, slave = pty.openpty()
    tty.setraw(slave)
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    proc = subprocess.Popen(args, stdin=slave, stdout=slave,
                            stderr=subprocess.DEVNULL, cwd=ROOT)
    os.close(slave)
    buf = bytearray()
    pos = 0
    log = open(LOG, "wb")
    mon = None

    def pump(t):
        end = time.monotonic() + t
        while time.monotonic() < end:
            r, _, _ = select.select([master], [], [], 0.2)
            if r:
                try:
                    d = os.read(master, 4096)
                except OSError:
                    return
                if d:
                    buf.extend(d)
                    log.write(d)
                    log.flush()

    def wait_for(s, t=60):
        nonlocal pos
        end = time.monotonic() + t
        while time.monotonic() < end:
            i = buf.find(s.encode(), pos)
            if i >= 0:
                pos = i + len(s)
                return True
            pump(0.3)
        return False

    def qmp_send(obj):
        nonlocal mon
        if mon is None:
            for _ in range(50):
                if os.path.exists(MON):
                    break
                time.sleep(0.2)
            mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            mon.connect(MON)
            mon.settimeout(3)
            f = mon.makefile("rw")
            f.readline()  # greeting
            f.write('{"execute":"qmp_capabilities"}\n')
            f.flush()
            f.readline()
            mon = f
        import json
        mon.write(json.dumps(obj) + "\n")
        mon.flush()
        line = mon.readline()
        print("qmp resp:", line.strip()[:160])

    def send_events(events):
        # 注意: QEMU 8.2 では input-send-event に device を
        # 指定すると QEMU 自体が落ちる (qom のバグ) ため、
        # device 無しで送る (現在のハンドラ = タブレット)。
        qmp_send({"execute": "input-send-event",
                  "arguments": {"events": events}})

    def tablet_cmd():
        os.write(master, b"tablet\r")
        if not wait_for("tablet:", 20):
            return None
        pump(0.6)
        text = buf.decode("utf-8", "replace")
        m = list(re.finditer(
            r"tablet: raw (\d+),(\d+) max (\d+),(\d+) buttons (\d+)",
            text))
        return m[-1] if m else None

    try:
        if not wait_for("[boot] input: virtio tablet ready"):
            print("MISSING: tablet driver did not come up")
            return 1
        print("ok: virtio tablet initialized")
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        send_events([{"type": "abs", "data": {"axis": "x",
                                               "value": 20000}},
                     {"type": "abs", "data": {"axis": "y",
                                               "value": 10000}}])
        time.sleep(1.0)
        pump(0.3)
        m = tablet_cmd()
        if m is None:
            print("MISSING: tablet state not reported")
            return 1
        rx, ry, mx, my, bt = (int(m.group(i)) for i in range(1, 6))
        print("state:", rx, ry, mx, my, bt)
        if abs(rx - 20000) > 64 or abs(ry - 10000) > 64:
            print("MISSING: absolute position not delivered")
            return 1
        if mx <= 0 or my <= 0:
            print("MISSING: abs range not read from config")
            return 1
        print("ok: absolute coordinates delivered")
        send_events([{"type": "btn", "data":
                      {"down": True, "button": "left"}}])
        time.sleep(0.8)
        pump(0.3)
        m = tablet_cmd()
        if m is None or int(m.group(5)) != 1:
            print("MISSING: left button press not delivered")
            return 1
        print("ok: button press delivered")
        send_events([{"type": "btn", "data":
                      {"down": False, "button": "left"}}])
        time.sleep(0.8)
        pump(0.3)
        m = tablet_cmd()
        if m is None or int(m.group(5)) != 0:
            print("MISSING: button release not delivered")
            return 1
        print("ok: button release delivered")
    finally:
        proc.terminate()
        log.close()
        if os.path.exists(MON):
            os.unlink(MON)
    print("TABLET TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
