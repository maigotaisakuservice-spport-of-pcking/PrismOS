#!/usr/bin/env python3
"""PrismOS NVMe test: boot the ISO with an NVMe drive as the
only disk. The block layer must pick the NVMe controller,
`disk` must report it, and a pattern write/read through the
block layer must round-trip."""
import os, pty, select, subprocess, sys, termios, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
IMG = os.path.join(ROOT, "build", "prism-nvme.img")
LOG = os.path.join(ROOT, "build", "boot-nvme.log")


def main():
    with open(IMG, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-m", "256M",
            "-drive", "file=%s,if=none,id=nv0,format=raw" % IMG,
            "-device", "nvme,drive=nv0,serial=prism-nvme",
            "-serial", "stdio", "-monitor", "none",
            "-display", "none", "-no-reboot"]
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

    def cmd(c, wait, t=30):
        os.write(master, (c + "\r").encode())
        return wait_for(wait, t)

    try:
        ok = wait_for("prismos> ")
        print("boot:", ok)
        if not ok:
            return 1
        if not cmd("disk", "nvme"):
            print("MISSING: disk does not report nvme")
            return 1
        print("ok: disk reports nvme")
        if not cmd("disk test 0", "disk test: ok"):
            print("MISSING: disk test 0 failed")
            return 1
        print("ok: nvme pattern write/read round-trip")
        print("NVME TEST PASSED")
        return 0
    finally:
        proc.terminate()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
