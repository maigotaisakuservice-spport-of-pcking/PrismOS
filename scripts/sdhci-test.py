#!/usr/bin/env python3
"""PrismOS SD/SDHCI test: boot with an sdhci-pci controller
and an sd-card backed by a raw file. The driver must
initialize the card, report its capacity, and pass the
pattern write/read check (`disk test 3`) on the last
sector."""
import os, pty, select, subprocess, sys, termios, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
IMG = os.path.join(ROOT, "build", "sd-test.img")
LOG = os.path.join(ROOT, "build", "boot-sd.log")
SIZE = 16 * 1024 * 1024  # 16MB -> 32768 sectors


def main():
    with open(IMG, "wb") as f:
        f.truncate(SIZE)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-serial", "stdio", "-monitor", "none",
            "-display", "none", "-no-reboot",
            "-drive", "if=none,id=sddrv,file=%s,format=raw" % IMG,
            "-device", "sdhci-pci",
            "-device", "sd-card,drive=sddrv"]
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
        import time
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
        import time
        nonlocal pos
        end = time.monotonic() + t
        while time.monotonic() < end:
            i = buf.find(s.encode(), pos)
            if i >= 0:
                pos = i + len(s)
                return True
            pump(0.3)
        return False

    try:
        if not wait_for("[sd] card ready:"):
            print("MISSING: SD card did not initialize")
            return 1
        text = buf.decode("utf-8", "replace")
        import re
        m = re.search(r"\[sd\] card ready: (\d+) sectors", text)
        print("sectors reported:", m.group(1) if m else "?")
        if not m or abs(int(m.group(1)) - SIZE // 512) > 64:
            print("MISSING: implausible sector count")
            return 1
        print("ok: SD card initialized with plausible capacity")
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        os.write(master, b"disk test 3\r")
        if not wait_for("disk test: ok"):
            print("MISSING: pattern test on SD failed")
            return 1
        print("ok: pattern write/read verified on SD card")
    finally:
        proc.terminate()
        log.close()
    print("SD TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
