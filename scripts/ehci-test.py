#!/usr/bin/env python3
"""PrismOS EHCI (USB 2.0) test: a usb-ehci controller with a
high-speed usb-storage device. The EHCI driver must find the
controller, reset the port, enumerate the device at high
speed, run INQUIRY / READ CAPACITY over Bulk-Only Transport,
and pass the pattern check (`ehci test`: write sector 100,
read it back, compare). The host side also verifies the
pattern landed in the image file.

Usage: python3 scripts/ehci-test.py
  (exit code 0 = passed; standard library only)
"""
import os, pty, select, subprocess, sys, termios, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
IMG = os.path.join(ROOT, "build", "prism-ehci-disk.img")
LOG = os.path.join(ROOT, "build", "boot-ehci.log")


def main():
    with open(IMG, "wb") as f:
        f.truncate(4 * 1024 * 1024)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-serial", "stdio", "-monitor", "none",
            "-display", "none", "-no-reboot",
            "-device", "usb-ehci,id=ehci0",
            "-drive", "if=none,id=ehciusb,file=%s,format=raw" % IMG,
            "-device", "usb-storage,bus=ehci0.0,drive=ehciusb"]
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

    def wait_for(s, t=90):
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
        if not wait_for("[ehci] controller ready"):
            print("MISSING: EHCI controller was not initialized")
            return 1
        print("ok: EHCI controller initialized")
        if not wait_for("speed=hs"):
            print("MISSING: device did not enumerate at high speed")
            return 1
        print("ok: device enumerated at high speed")
        if not wait_for("[ehci] storage:"):
            print("MISSING: storage INQUIRY/READ CAPACITY failed")
            return 1
        print("ok: storage capacity read over BOT")
        if not wait_for("[boot] usb2: storage registered"):
            print("MISSING: storage not registered in block layer")
            return 1
        print("ok: registered as block device")
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        os.write(master, b"ehci\r")
        if not wait_for("ehci: 1 device(s)"):
            print("MISSING: ehci command did not list the device")
            return 1
        print("ok: ehci command lists the device")
        os.write(master, b"ehci test\r")
        if not wait_for("ehci: storage test passed."):
            print("MISSING: pattern test over EHCI failed")
            return 1
        print("ok: storage pattern test passed over EHCI")
    finally:
        proc.terminate()
        log.close()
    # Host-side: sector 100 (byte 51200) must hold the pattern.
    with open(IMG, "rb") as f:
        f.seek(100 * 512)
        data = f.read(512)
    for i, b in enumerate(data):
        if b != (i * 13 + 5) & 0xFF:
            print("MISSING: pattern not found in image at sector 100")
            return 1
    print("ok: pattern verified in the image file")
    print("EHCI TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
