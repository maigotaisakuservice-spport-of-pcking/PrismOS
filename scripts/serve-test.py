#!/usr/bin/env python3
"""PrismOS TCP server socket test in QEMU.

The guest runs `serve 8080` (net_tcp_listen based HTTP
server). QEMU user-net forwards host port 18099 to the
guest's 8080, and the host fetches three pages in a row:
three sequential connections must all be answered.
"""
import importlib.util
import os
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "serve-test.log")
HOST_PORT = 18099

_spec = importlib.util.spec_from_file_location(
    "recovery_test",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "recovery-test.py"))
rt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rt)


class ServeBoot(rt.Boot):
    """Boot with a hostfwd rule added."""

    def __init__(self, log_path):
        import pty
        import signal
        import termios
        import tty
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(log_path, "wb")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        args = [rt.QEMU, "-cdrom", rt.ISO,
                "-drive",
                "file=%s,format=raw,if=ide,index=0,media=disk"
                % rt.DISK,
                "-boot", "order=d", "-m", "256M",
                "-serial", "stdio", "-monitor", "none",
                "-display", "none", "-no-reboot",
                "-netdev",
                "user,id=net0,hostfwd=tcp::%d-:8080" % HOST_PORT,
                "-device", "e1000,netdev=net0"]
        self.proc = subprocess.Popen(
            args, stdin=slave_fd, stdout=slave_fd,
            stderr=subprocess.DEVNULL, cwd=ROOT, close_fds=True)
        os.close(slave_fd)
        self._signal = signal


def fetch(path):
    with urllib.request.urlopen(
            "http://127.0.0.1:%d%s" % (HOST_PORT, path),
            timeout=15) as resp:
        return resp.read().decode("utf-8", "replace")


def main():
    rt.make_disk()
    b = ServeBoot(LOG)
    try:
        b.wait_for("prismos> ", 60, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 30, "install done")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("serve 8080")
        b.wait_for("serve: listening on port 8080", 30,
                   "listening")
        print("ok: guest server is listening")
        for i, path in enumerate(["/one", "/two", "/three"]):
            body = fetch(path)
            if "Hello from PrismOS!" not in body:
                raise rt.RecoveryTestFailure(
                    "unexpected body for %s: %r" % (path, body))
            if path not in body:
                raise rt.RecoveryTestFailure(
                    "path not echoed for %s: %r" % (path, body))
            print("ok: connection %d answered (%s)"
                  % (i + 1, path))
            b.wait_for("serve: request " + path, 20,
                       "request logged")
        b.wait_for("serve: done", 30, "server finished")
        print("ok: three sequential connections served")
    finally:
        b.stop()
    print("SERVE TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except rt.RecoveryTestFailure as exc:
        print("FAIL: %s" % exc)
        sys.exit(1)
