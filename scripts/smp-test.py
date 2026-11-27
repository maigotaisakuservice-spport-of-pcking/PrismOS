#!/usr/bin/env python3
"""PrismOS SMP test: boot with -smp 4. The kernel must parse
the MADT, start the 3 APs via INIT/SIPI, and report 4 CPUs
online; the `smp` shell command must show every AP answering
the liveness IPI."""
import os, pty, select, subprocess, sys, termios, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
LOG = os.path.join(ROOT, "build", "boot-smp.log")


def main():
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-smp", "4", "-serial", "stdio",
            "-monitor", "none", "-display", "none", "-no-reboot"]
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

    try:
        if not wait_for("[smp] 4 CPU(s) online"):
            print("MISSING: 4 CPUs did not come online")
            return 1
        print("ok: 4 CPUs online (BSP + 3 APs via INIT/SIPI)")
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        os.write(master, b"smp\r")
        if not wait_for("smp: 4 CPU(s) online"):
            print("MISSING: smp command output")
            return 1
        # every AP must have answered at least one IPI ping
        text = buf.decode("utf-8", "replace")
        ap_lines = [ln for ln in text.splitlines()
                    if "(AP)" in ln and "pings" in ln]
        print("AP lines:", ap_lines)
        if len(ap_lines) < 3:
            print("MISSING: expected 3 AP lines")
            return 1
        for ln in ap_lines:
            n = int(ln.rsplit("pings", 1)[1].strip())
            if n < 1:
                print("MISSING: an AP did not answer the IPI:", ln)
                return 1
        print("ok: all APs answered the liveness IPI")
        # 実作業分担: AP がハッシュ計算を実行し結果が一致。
        os.write(master, b"smp work\r")
        if not wait_for("smp work: all chunk hashes match"):
            print("MISSING: smp work sharing did not complete")
            return 1
        print("ok: APs executed real work (chunk hashes match)")
        print("SMP TEST PASSED")
        return 0
    finally:
        proc.terminate()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
