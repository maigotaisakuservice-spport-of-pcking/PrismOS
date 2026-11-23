#!/usr/bin/env python3
"""PrismOS USB hub test: a usb-hub on UHCI root port 1,
with a usb-storage behind hub port 1 and a usb-kbd behind
hub port 2. The driver must enumerate the hub, power and
reset the downstream ports, enumerate both devices
through the hub, and pass the storage pattern check
(`usb test`). Keyboard input through the hub is verified
by typing `version` via QMP (broadcast, no device field:
QEMU 8.2 crashes when input-send-event names a device)."""
import os, pty, select, socket, subprocess, sys, termios
import time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
IMG = os.path.join(ROOT, "build", "prism-hub-disk.img")
LOG = os.path.join(ROOT, "build", "boot-hub.log")
QMP_SOCK = os.path.join(ROOT, "build", "qmp-hub.sock")


def main():
    with open(IMG, "wb") as f:
        f.truncate(4 * 1024 * 1024)
    if os.path.exists(QMP_SOCK):
        os.unlink(QMP_SOCK)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-serial", "stdio", "-monitor", "none",
            "-display", "none", "-no-reboot",
            "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
            "-device", "piix3-usb-uhci",
            "-device", "usb-hub,id=hub0,port=1",
            "-drive", "if=none,id=hubusb,file=%s,format=raw" % IMG,
            "-device", "usb-storage,drive=hubusb,port=1.1",
            "-device", "usb-kbd,id=hubkbd,port=1.2"]
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

    def qmp_keys(qcodes):
        for _ in range(50):
            if os.path.exists(QMP_SOCK):
                break
            time.sleep(0.2)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(QMP_SOCK)
        s.settimeout(5)
        f = s.makefile("rw")
        f.readline()
        f.write('{"execute":"qmp_capabilities"}\n')
        f.flush()
        f.readline()
        import json
        events = []
        for q in qcodes:
            events.append({"type": "key", "data": {
                "down": True, "key": {"type": "qcode",
                                      "data": q}}})
            events.append({"type": "key", "data": {
                "down": False, "key": {"type": "qcode",
                                       "data": q}}})
        f.write(json.dumps({"execute": "input-send-event",
                            "arguments": {"events": events}}) +
                "\n")
        f.flush()
        f.readline()
        s.close()

    try:
        if not wait_for("[usb] hub found"):
            print("MISSING: hub was not detected")
            return 1
        print("ok: hub detected on root port")
        if not wait_for("[usb] storage ready:"):
            print("MISSING: storage behind hub not enumerated")
            return 1
        print("ok: storage enumerated through the hub")
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        os.write(master, b"usb test\r")
        if not wait_for("storage test passed"):
            print("MISSING: pattern test through hub failed")
            return 1
        print("ok: storage pattern test passed through hub")
        # Keyboard behind the hub: type "version".
        qmp_keys(["v", "e", "r", "s", "i", "o", "n", "ret"])
        if not wait_for("PrismOS", 30):
            print("MISSING: keyboard behind hub did not type")
            return 1
        print("ok: keyboard input delivered through the hub")
    finally:
        proc.terminate()
        log.close()
    print("HUB TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
