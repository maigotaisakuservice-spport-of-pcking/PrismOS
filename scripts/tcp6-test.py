#!/usr/bin/env python3
"""PrismOS TCP-over-IPv6 test in QEMU (serial shell, user networking).

Phase A (client): a host-side IPv6 TCP server listens on
[::]:18711. The guest runs `tcp6 <slirp host> 18711 hello6`;
the host address is the guest's SLAAC prefix with host bits
::2 (the user-net gateway, like 10.0.2 for IPv4). The guest
must connect, deliver the line, and print both reply lines.

Phase B (server): QEMU 8.2's hostfwd cannot parse IPv6
host addresses, so two guests are linked directly with a
socket (multicast) LAN instead. Guest A runs `serve6 18712`;
guest B runs `tcp6 <A's link-local> 18712 ping over six` and
must receive A's "pong6:" reply. This exercises ND in both
directions, the listen/handshake path, data and close with
a real peer stack.
"""
import ipaddress
import os
import pty
import re
import select
import socket
import subprocess
import sys
import termios
import threading
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
DISK = os.path.join(ROOT, "build", "prism-tcp6-disk.img")
LOG = os.path.join(ROOT, "build", "boot-tcp6.log")
QEMU = "qemu-system-x86_64"
CLIENT_PORT = 18711
SERVER_PORT = 18712


class Tcp6Failure(Exception):
    pass


class Boot:
    def __init__(self, log_path, netdev="user,id=net0", mac=None,
                 disk=None):
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(log_path, "wb")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        device = "e1000,netdev=net0"
        if mac is not None:
            device += ",mac=%s" % mac
        args = [QEMU, "-cdrom", ISO,
                "-drive",
                "file=%s,format=raw,if=ide,index=0,media=disk"
                % (disk or DISK),
                "-boot", "order=d",
                "-m", "256M", "-serial", "stdio", "-monitor", "none",
                "-display", "none", "-no-reboot",
                "-netdev", netdev,
                "-device", device]
        self.proc = subprocess.Popen(args, stdin=slave_fd,
                                     stdout=slave_fd,
                                     stderr=subprocess.DEVNULL, cwd=ROOT,
                                     close_fds=True)
        os.close(slave_fd)

    def pump(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            r, _, _ = select.select([self.master_fd], [], [], remaining)
            if not r:
                return
            try:
                chunk = os.read(self.master_fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            self.buf.extend(chunk)
            self.log_file.write(chunk)
            self.log_file.flush()

    def wait_for(self, text, timeout, what):
        needle = text.encode()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            idx = self.buf.find(needle, self.pos)
            if idx >= 0:
                self.pos = idx + len(needle)
                return
            self.pump(min(1.0, max(0.0, deadline - time.monotonic())))
        raise Tcp6Failure("MISSING: %s not seen in %s s (expected %r)"
                          % (what, timeout, text))

    def wait_for_re(self, pattern, timeout, what):
        deadline = time.monotonic() + timeout
        rx = re.compile(pattern)
        while time.monotonic() < deadline:
            text = bytes(self.buf[self.pos:]).decode("utf-8", "replace")
            m = rx.search(text)
            if m:
                self.pos += m.end()
                return m
            self.pump(min(1.0, max(0.0, deadline - time.monotonic())))
        raise Tcp6Failure("MISSING: %s not seen in %s s (pattern %r)"
                          % (what, timeout, pattern))

    def send(self, line):
        os.write(self.master_fd, (line + "\n").encode())

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        os.close(self.master_fd)
        self.log_file.close()


def make_disk():
    if os.path.exists(DISK):
        os.remove(DISK)
    with open(DISK, "wb") as f:
        f.truncate(16 * 1024 * 1024)


class HostServer(threading.Thread):
    """One-shot IPv6 TCP server for the client phase."""

    def __init__(self):
        super().__init__(daemon=True)
        self.received = None
        self.error = None
        self.sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("::", CLIENT_PORT))
        self.sock.listen(1)
        self.sock.settimeout(120)

    def run(self):
        try:
            conn, _ = self.sock.accept()
            conn.settimeout(30)
            data = b""
            while not data.endswith(b"\n"):
                chunk = conn.recv(1024)
                if not chunk:
                    break
                data += chunk
            self.received = data.decode("utf-8", "replace").strip()
            conn.sendall(b"hello6 from host\nsecond line\n")
            time.sleep(0.3)
            conn.close()
        except Exception as e:  # reported by the main thread
            self.error = e
        finally:
            self.sock.close()


def main():
    make_disk()
    server = HostServer()
    server.start()

    guest_global = None
    b = Boot(LOG)
    try:
        b.wait_for("prismos> ", 60, "shell prompt")
        b.send("ip6")
        m = b.wait_for_re(r"ipv6 global ([0-9a-f:]+)", 30,
                          "global address")
        guest_global = m.group(1)
        print("ok: global %s (SLAAC)" % guest_global)
        host_addr = str(ipaddress.IPv6Address(
            (int(ipaddress.IPv6Address(guest_global)) &
             ~((1 << 64) - 1)) | 2))
        b.wait_for("prismos> ", 20, "prompt")

        b.send("tcp6 %s %d hello6" % (host_addr, CLIENT_PORT))
        b.wait_for("tcp6: connected", 40, "tcp6 connect")
        print("ok: tcp6 connected to host %s" % host_addr)
        b.wait_for("tcp6: got hello6 from host", 40, "first reply line")
        b.wait_for("tcp6: got second line", 20, "second reply line")
        b.wait_for("tcp6: done", 20, "tcp6 done")
        print("ok: tcp6 client sent and received over IPv6")
    finally:
        b.stop()
    server.join(timeout=10)
    if server.error is not None:
        raise Tcp6Failure("host server error: %r" % (server.error,))
    if server.received != "hello6":
        raise Tcp6Failure("host server received %r, expected 'hello6'"
                          % (server.received,))
    print("ok: host received the guest's line over IPv6")

    # ---- Phase B: guest as server, second guest as client ----
    disk_b = os.path.join(ROOT, "build", "prism-tcp6-disk-b.img")
    if os.path.exists(disk_b):
        os.remove(disk_b)
    with open(disk_b, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    # QEMU ソケット LAN を listen/connect で直結する
    # (ホストのマルチキャスト配送に依存しないため)。
    lan_a = "socket,id=net0,listen=:1235"
    lan_b = "socket,id=net0,connect=127.0.0.1:1235"
    log_a = os.path.join(ROOT, "build", "boot-tcp6-a.log")
    log_b = os.path.join(ROOT, "build", "boot-tcp6-b.log")
    a_ll = "fe80::5054:ff:fe12:3401"
    ga = Boot(log_a, netdev=lan_a, mac="52:54:00:12:34:01")
    gb = None
    try:
        ga.wait_for("prismos> ", 60, "shell prompt (server guest)")
        ga.send("serve6 %d" % SERVER_PORT)
        ga.wait_for("serve6: listening on port %d" % SERVER_PORT, 30,
                    "serve6 listening")
        gb = Boot(log_b, netdev=lan_b, mac="52:54:00:12:34:02",
                  disk=disk_b)
        gb.wait_for("prismos> ", 60, "shell prompt (client guest)")
        gb.send("tcp6 %s %d ping over six" % (a_ll, SERVER_PORT))
        gb.wait_for("tcp6: connected", 60, "phase B connect")
        print("ok: guest B connected to guest A's serve6")
        gb.wait_for("tcp6: got pong6: ping over six", 40,
                    "phase B pong")
        gb.wait_for("tcp6: done", 20, "phase B client done")
        print("ok: serve6 replied over the guest-to-guest LAN")
        ga.wait_for("serve6: got ping over six", 20, "serve6 got line")
        ga.wait_for("serve6: done", 20, "serve6 done")
        print("ok: serve6 accepted, read and closed over IPv6")
    finally:
        ga.stop()
        if gb is not None:
            gb.stop()
    print("TCP6 TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Tcp6Failure as e:
        print(str(e))
        print("TCP6 TEST FAILED")
        sys.exit(1)
