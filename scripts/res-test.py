#!/usr/bin/env python3
"""PrismOS resolution switching test: boot at the default
mode, switch to 1024x768 with the `res` shell command via
Bochs VBE, and verify with monitor screendumps that the
framebuffer geometry actually changed and the desktop
redrew. Then switch back to 1280x800."""
import os, pty, select, socket, subprocess, sys, termios
import time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
LOG = os.path.join(ROOT, "build", "boot-res.log")
MON = "/tmp/prism-res-mon.sock"
PPM1 = "/tmp/prism-res1.ppm"
PPM2 = "/tmp/prism-res2.ppm"
PPM3 = "/tmp/prism-res3.ppm"


def ppm_size(path):
    with open(path, "rb") as f:
        data = f.read(64)
    parts = data.split()
    # P6 <w> <h> <max>
    return int(parts[1]), int(parts[2])


def ppm_variety(path):
    with open(path, "rb") as f:
        data = f.read()
    nl = data.find(b"\n")
    rest = data[nl + 1:]
    nl2 = rest.find(b"\n")
    rest = rest[nl2 + 1:]
    nl3 = rest.find(b"\n")
    px = rest[nl3 + 1:]
    return len(set(px[::997]))


def main():
    if os.path.exists(MON):
        os.unlink(MON)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-serial", "stdio", "-display",
            "vnc=:14", "-monitor", "unix:%s,server,nowait" % MON,
            "-no-reboot"]
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

    def hmp(cmd):
        nonlocal mon
        if mon is None:
            for _ in range(50):
                if os.path.exists(MON):
                    break
                time.sleep(0.2)
            mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            mon.connect(MON)
            mon.settimeout(3)
            try:
                mon.recv(4096)
            except socket.timeout:
                pass
        mon.sendall((cmd + "\n").encode())
        time.sleep(0.4)
        try:
            mon.recv(4096)
        except socket.timeout:
            pass

    try:
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        pump(1.0)
        hmp("screendump %s" % PPM1)
        w, h = ppm_size(PPM1)
        print("initial mode:", w, h)
        os.write(master, b"res 1024 768\r")
        if not wait_for("res: 1024x768"):
            print("MISSING: res command did not confirm")
            return 1
        pump(1.0)
        hmp("screendump %s" % PPM2)
        w2, h2 = ppm_size(PPM2)
        print("after res:", w2, h2)
        if (w2, h2) != (1024, 768):
            print("MISSING: framebuffer geometry did not change")
            return 1
        if ppm_variety(PPM2) < 8:
            print("MISSING: screen looks blank after switch")
            return 1
        print("ok: resolution switched and desktop redrew")
        os.write(master, b"res 1280 800\r")
        if not wait_for("res: 1280x800"):
            print("MISSING: switch back did not confirm")
            return 1
        pump(1.0)
        hmp("screendump %s" % PPM3)
        w3, h3 = ppm_size(PPM3)
        print("restored mode:", w3, h3)
        if (w3, h3) != (1280, 800):
            print("MISSING: geometry did not restore")
            return 1
        print("ok: resolution restored")
    finally:
        proc.terminate()
        log.close()
        if os.path.exists(MON):
            os.unlink(MON)
    print("RES TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
