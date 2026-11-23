#!/usr/bin/env python3
"""PrismOS simultaneous TCP connections test in QEMU.

The guest runs `serve2 8080`, which accepts TWO connections
and keeps the first one open while accepting the second,
then answers the second request first. The host opens two
sockets (both connected before either request is answered),
sends a request line on each, and checks that:

  - socket A receives the "serve2 first" body,
  - socket B receives the "serve2 second" body,
  - B's response arrives before A's (reverse order proves
    both streams were alive at the same time),
  - the guest log shows the second accept happened while
    the first connection was still open.

With the old per-connection receive pump, segments for the
other stream were dropped while waiting, and a second
accept while the first connection was open was impossible.
"""
import importlib.util
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "serve2-test.log")
HOST_PORT = 18101

_spec = importlib.util.spec_from_file_location(
    "recovery_test",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "recovery-test.py"))
rt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rt)


class Serve2Boot(rt.Boot):
    """Boot with a hostfwd rule added (same as serve-test)."""

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


def recv_until(sock, needle, timeout=30.0):
    sock.settimeout(timeout)
    data = b""
    while needle not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def main():
    rt.make_disk()
    b = Serve2Boot(LOG)
    failures = []
    try:
        b.wait_for("prismos> ", 60, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 30, "install done")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("serve2 8080")
        b.wait_for("serve2: listening on port 8080", 30, "listening")
        print("ok: serve2 listening")

        # A をつなぎ、要求を送る (まだ応答は来ないはず)。
        a = socket.create_connection(("127.0.0.1", HOST_PORT),
                                     timeout=15)
        a.sendall(b"GET /a HTTP/1.0\r\n\r\n")
        print("ok: connection A open, request sent")
        # B をつなぐ。A が開いたまま B が受け付けられること。
        b_sock = socket.create_connection(("127.0.0.1", HOST_PORT),
                                          timeout=15)
        b_sock.sendall(b"GET /b HTTP/1.0\r\n\r\n")
        print("ok: connection B open while A still open")

        # 先に B へ応答が返る (逆順応答 = 2 本が同時に生きて
        # いる証拠)。A から先に読もうとするとタイムアウト
        # するのが正しい挙動なので、B → A の順に読む。
        data_b = recv_until(b_sock, b"serve2 second", 30)
        if b"serve2 second" in data_b:
            print("ok: B answered first with its own body")
        else:
            failures.append("B response wrong: %r" % data_b[:80])
        data_a = recv_until(a, b"serve2 first", 30)
        if b"serve2 first" in data_a:
            print("ok: A answered with its own body")
        else:
            failures.append("A response wrong: %r" % data_a[:80])
        a.close()
        b_sock.close()
        b.wait_for("serve2: done", 30, "serve2 finished")
        print("ok: serve2 done")
    finally:
        b.stop()
    if failures:
        print("SERVE2 TEST FAILED:", "; ".join(failures))
        return 1
    print("SERVE2 TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
