#!/usr/bin/env python3
"""PrismOS xHCI (USB 3.x) test: a qemu-xhci controller with a
usb-storage device. The xHCI driver must find the controller,
reset the port, enumerate the device, run INQUIRY / READ
CAPACITY over Bulk-Only Transport, and pass the pattern check
(`xhci test`: write sector 100, read it back, compare). The
host side also verifies the pattern landed in the image file.

Usage: python3 scripts/xhci-test.py
  (exit code 0 = passed; standard library only)
"""
import os, pty, select, subprocess, sys, termios, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
IMG = os.path.join(ROOT, "build", "prism-xhci-disk.img")
LOG = os.path.join(ROOT, "build", "boot-xhci.log")


def main():
    with open(IMG, "wb") as f:
        f.truncate(4 * 1024 * 1024)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-serial", "stdio", "-monitor", "none",
            "-display", "none", "-no-reboot",
            "-device", "qemu-xhci,id=xhci0",
            "-drive", "if=none,id=xhciusb,file=%s,format=raw" % IMG,
            "-device", "usb-storage,bus=xhci0.0,drive=xhciusb"]
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
        if not wait_for("[xhci] controller ready"):
            print("MISSING: xHCI controller was not initialized")
            return 1
        print("ok: xHCI controller initialized")
        if not wait_for("[xhci] storage:"):
            print("MISSING: storage INQUIRY/READ CAPACITY failed")
            return 1
        print("ok: storage capacity read over BOT")
        if not wait_for("[boot] usb3: storage registered"):
            print("MISSING: storage not registered in block layer")
            return 1
        print("ok: registered as block device")
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        os.write(master, b"xhci\r")
        if not wait_for("xhci: 1 device(s)"):
            print("MISSING: xhci command did not list the device")
            return 1
        print("ok: xhci command lists the device")
        os.write(master, b"xhci test\r")
        if not wait_for("xhci: storage test passed."):
            print("MISSING: pattern test over xHCI failed")
            return 1
        print("ok: storage pattern test passed over xHCI")
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
    print("XHCI TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
