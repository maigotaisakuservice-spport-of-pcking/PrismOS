#!/usr/bin/env python3
"""PrismOS network test.

Boots the ISO in QEMU with an e1000 card on user networking
and checks, over the serial shell:
  1. `net` configures an address with DHCP (10.0.2.x),
  2. `fetch example.com` returns the real page body
     (contains "Example Domain").

Usage: python3 scripts/net-test.py [--log PATH]
  (exit code 0 = passed; standard library only)
"""

import argparse
import os
import pty
import select
import signal
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
DEFAULT_LOG = os.path.join(ROOT, "build", "boot-net.log")
QEMU = "qemu-system-x86_64"


class NetTestFailure(Exception):
    pass


def main():
    parser = argparse.ArgumentParser(description="PrismOS network test")
    parser.add_argument("--log", default=DEFAULT_LOG)
    parser.add_argument("--host", default="example.com",
                        help="host to fetch (default: example.com; "
                        "use the local test server address when the "
                        "sandbox blocks external DNS)")
    args = parser.parse_args()
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1

    qemu_args = [
        QEMU, "-cdrom", ISO, "-m", "256M",
        "-serial", "stdio",
        "-monitor", "none",
        "-display", "none",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        "-no-reboot",
        "-d", "cpu_reset,int",
        "-D", os.path.join(ROOT, "build", "qemu-debug-net.log"),
    ]
    log_file = open(args.log, "wb")
    stderr_file = open(os.path.join(ROOT, "build", "qemu-stderr-net.log"),
                       "wb")
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    proc = subprocess.Popen(qemu_args, stdin=slave_fd, stdout=slave_fd,
                            stderr=stderr_file, cwd=ROOT, close_fds=True)
    os.close(slave_fd)
    buf = bytearray()
    pos = 0
    failures = []

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
        needle_b = needle.encode("utf-8", "replace")
        deadline = time.monotonic() + timeout
        while True:
            idx = buf.find(needle_b, pos)
            if idx >= 0:
                pos = idx + len(needle_b)
                return
            if proc.poll() is not None:
                raise NetTestFailure("QEMU exited early (code %s)"
                                     % proc.returncode)
            if time.monotonic() > deadline:
                raise NetTestFailure("%s not seen in %s s" % (what, timeout))
            pump(min(0.5, deadline - time.monotonic()))

    def send_command(cmd):
        os.write(master_fd, (cmd + "\r").encode("ascii"))

    try:
        wait_for("PrismOS 27H1", 30, "boot banner")
        print("ok: boot banner")
        wait_for("prismos> ", 30, "shell prompt")
        print("ok: shell prompt")

        send_command("net")
        wait_for("ip: 10.0.2.", 60, "DHCP address")
        print("ok: DHCP address in 10.0.2.0/24")
        wait_for("[net] ready", 20, "network ready")
        print("ok: network ready (DHCP)")
        wait_for("prismos> ", 20, "shell prompt after net")

        send_command("fetch " + args.host)
        marker = ("Example Domain" if args.host == "example.com"
                  else "PrismOS Network Works")
        wait_for(marker, 60, "fetched page body")
        print("ok: fetched %s (real page body)" % args.host)
        wait_for("prismos> ", 20, "shell prompt after fetch")

        print("NET TEST PASSED")
        return 0
    except NetTestFailure as e:
        print("MISSING: %s" % e)
        failures.append(str(e))
    except OSError as e:
        print("MISSING: QEMU communication error: %s" % e)
        failures.append(str(e))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        os.close(master_fd)
        log_file.close()
        stderr_file.close()
    if failures:
        print("--- boot log (%s) ---" % args.log)
        try:
            with open(args.log, "rb") as f:
                sys.stdout.write(f.read().decode("utf-8", "replace"))
        except OSError:
            pass
        print("NET TEST FAILED")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
