#!/usr/bin/env python3
"""PrismOS boot animation test: boot with a VNC display and
take screendumps at several animation frames (synced via the
serial "[boot] anim frame N" lines). Consecutive dumps must
differ (the animation really moves), and the boot must reach
the shell prompt afterwards."""
import os, pty, re, select, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
MON = os.path.join(ROOT, "build", "anim-mon.sock")
DUMPS = [os.path.join(ROOT, "build", "anim-%d.ppm" % i) for i in range(3)]


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    dims = parts[1].split()
    w, h = int(dims[0]), int(dims[1])
    return w, h, parts[3]


def diff_count(a, b):
    n = 0
    for i in range(0, min(len(a), len(b)) - 2, 3):
        if a[i:i + 3] != b[i:i + 3]:
            n += 1
    return n


def main():
    for p in DUMPS + [MON]:
        if os.path.exists(p):
            os.unlink(p)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-monitor",
            "unix:%s,server,nowait" % MON, "-serial", "stdio",
            "-display", "vnc=:11"]
    master, slave = pty.openpty()
    proc = subprocess.Popen(args, stdin=slave, stdout=slave,
                            stderr=subprocess.DEVNULL, cwd=ROOT)
    os.close(slave)
    buf = bytearray()

    def pump(t):
        end = time.monotonic() + t
        while time.monotonic() < end:
            r, _, _ = select.select([master], [], [], 0.1)
            if r:
                try:
                    d = os.read(master, 4096)
                except OSError:
                    return
                if d:
                    buf.extend(d)

    def wait_frame(n, t=40):
        pat = ("[boot] anim frame %d" % n).encode()
        end = time.monotonic() + t
        while time.monotonic() < end:
            if buf.find(pat) >= 0:
                return True
            pump(0.2)
        return False

    def mon_connect(t=20):
        end = time.monotonic() + t
        while time.monotonic() < end:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(MON)
                s.settimeout(5)
                s.recv(4096)
                return s
            except OSError:
                time.sleep(0.3)
        return None

    def screendump(mon, path):
        mon.sendall(("screendump %s\n" % path).encode())
        time.sleep(0.4)
        try:
            mon.recv(4096)
        except socket.timeout:
            pass

    try:
        mon = mon_connect()
        if mon is None:
            print("FAIL: no monitor")
            return 1
        marks = [4, 20, 38]
        for i, m in enumerate(marks):
            if not wait_frame(m):
                print("FAIL: animation frame %d never seen" % m)
                return 1
            screendump(mon, DUMPS[i])
            print("dump %d at frame %d" % (i, m))
        # boot completes to the shell
        end = time.monotonic() + 40
        ok = False
        while time.monotonic() < end:
            if buf.find(b"prismos> ") >= 0:
                ok = True
                break
            pump(0.3)
        print("shell reached:", ok)
        if not ok:
            return 1
        frames = [read_ppm(p) for p in DUMPS]
        d01 = diff_count(frames[0][2], frames[1][2])
        d12 = diff_count(frames[1][2], frames[2][2])
        print("pixel diffs: frame4->20 = %d, frame20->38 = %d"
              % (d01, d12))
        if d01 < 5000:
            print("FAIL: fade-in did not change the screen enough")
            return 1
        if d12 < 200:
            print("FAIL: dots/bar did not move between late frames")
            return 1
        print("BOOT ANIM TEST PASSED")
        return 0
    finally:
        proc.terminate()


if __name__ == "__main__":
    sys.exit(main())
