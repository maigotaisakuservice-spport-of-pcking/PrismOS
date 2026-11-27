#!/usr/bin/env python3
"""PrismOS ACPI power test.

Boots the ISO and checks real power management:
  1. `power` reports the ACPI FADT values parsed from the
     firmware tables (PM1a port, S3/S5 types, battery).
  2. `sleep` enters S3: QEMU must report the VM suspended.
  3. `system_wakeup` resumes it: the guest prints the
     resume line and the shell answers again.
  4. `poweroff` enters S5: QEMU exits.

Usage: python3 scripts/power-test.py [--log PATH]
  (exit code 0 = passed; standard library only)
"""

import argparse
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
DEFAULT_LOG = os.path.join(ROOT, "build", "power.log")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-power.sock")
QEMU = "qemu-system-x86_64"


class PowerTestFailure(Exception):
    pass


def main():
    parser = argparse.ArgumentParser(description="PrismOS power test")
    parser.add_argument("--log", default=DEFAULT_LOG)
    args = parser.parse_args()
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    qemu_args = [
        QEMU, "-cdrom", ISO, "-m", "256M",
        "-serial", "stdio",
        "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
        "-display", "none",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        "-no-reboot",
    ]
    try:
        os.unlink(MONITOR_SOCK)
    except OSError:
        pass
    log_file = open(args.log, "wb")
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    proc = subprocess.Popen(qemu_args, stdin=slave_fd,
                            stdout=slave_fd,
                            stderr=open(os.path.join(ROOT, "build", "qemu-stderr-power.log"), "wb"),
                            cwd=ROOT, close_fds=True)
    os.close(slave_fd)
    buf = bytearray()
    pos = 0
    monitor = None

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
        needle_b = needle.encode()
        deadline = time.monotonic() + timeout
        while True:
            idx = buf.find(needle_b, pos)
            if idx >= 0:
                pos = idx + len(needle_b)
                return
            if proc.poll() is not None:
                raise PowerTestFailure("QEMU exited early")
            if time.monotonic() > deadline:
                raise PowerTestFailure(
                    "%s not seen in %s s" % (what, timeout))
            pump(min(0.5, deadline - time.monotonic()))

    def hmp(cmd):
        monitor.sendall((cmd + "\n").encode())
        data = b""
        while b"(qemu)" not in data:
            chunk = monitor.recv(4096)
            if not chunk:
                break
            data += chunk
        return data.decode("utf-8", "replace")

    try:
        wait_for("prismos> ", 120, "shell prompt")
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor.connect(MONITOR_SOCK)
        monitor.recv(4096)

        os.write(master_fd, b"power\r")
        wait_for("power: S3 sleep available (type ", 20,
                 "S3 reported")
        wait_for("power: battery: none in ACPI namespace", 10,
                 "battery honestly absent")

        os.write(master_fd, b"sleep\r")
        wait_for("power: entering S3", 20, "sleep entry")
        # QEMU must report the VM suspended within ~15 s.
        deadline = time.monotonic() + 15
        status = ""
        while time.monotonic() < deadline:
            status = hmp("info status")
            if "suspended" in status or "paused" in status:
                break
            time.sleep(0.5)
        if "suspended" not in status and "paused" not in status:
            raise PowerTestFailure(
                "VM did not suspend (status: %s)" % status.strip())

        hmp("system_wakeup")
        wait_for("power: resumed from S3", 30, "resume line")
        wait_for("prismos> ", 20, "prompt after resume")
        os.write(master_fd, b"version\r")
        wait_for("PrismOS", 20, "shell alive after resume")

        os.write(master_fd, b"poweroff\r")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        if proc.poll() is None:
            raise PowerTestFailure("poweroff did not stop QEMU")
        print("POWER TEST PASSED")
        return 0
    except PowerTestFailure as exc:
        print("POWER TEST FAILED: %s" % exc)
        return 1
    finally:
        try:
            if monitor is not None:
                monitor.close()
        except OSError:
            pass
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        log_file.close()


if __name__ == "__main__":
    sys.exit(main())
