#!/usr/bin/env python3
"""PrismOS ARM ネットワークテスト。

QEMU の virt 構成に virtio-net-device を付け、ユーザーネット
ワーキング (slirp) 経由で確認する:
  1. `net` で DHCP により 10.0.2.x が付く ([net] ready)。
  2. `fetch 10.0.2.2:8080 /` でホスト側 HTTP サーバのページが取れる。

ホスト側サーバは 8080 番を使う。80 番は特権ポートのため
CI ランナーのような非 root 環境では bind できない。
ゲスト側 fetch は host:port 形式に対応している。

外部ネットワークには出ない (ホストの 10.0.2.2 で完結する)。

使い方: python3 scripts/arm-net-test.py
成功したら "ARM NET TEST PASSED" と出す。
"""
import functools
import http.server
import os
import select
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "build", "kernel-arm.bin")
LOG = os.path.join(ROOT, "build", "arm-net.log")
MARKER = "PrismOS ARM net test page"
FAILURES = []


def check(label, ok):
    print(("ok: " if ok else "FAIL: ") + label)
    if not ok:
        FAILURES.append(label)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = ("<html><body><h1>" + MARKER + "</h1></body></html>"
                ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


class Session:
    def __init__(self):
        self.buf = b""
        self.proc = subprocess.Popen(
            [
                "qemu-system-aarch64",
                "-M", "virt",
                "-cpu", "cortex-a57",
                "-m", os.environ.get("PRISM_ARM_MEM_MB", "256"),
                "-kernel", KERNEL,
                "-device", "ramfb",
                "-netdev", "user,id=n0",
                "-device", "virtio-net-device,netdev=n0",
                "-serial", "stdio",
                "-display", "none",
                "-no-reboot",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    def read_until(self, needle, timeout=60.0):
        needle_b = needle.encode()
        deadline = time.time() + timeout
        while needle_b not in self.buf:
            if self.proc.poll() is not None:
                return False
            remain = deadline - time.time()
            if remain <= 0:
                return False
            r, _, _ = select.select([self.proc.stdout], [], [], remain)
            if not r:
                return False
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                return False
            self.buf += chunk
        idx = self.buf.index(needle_b) + len(needle_b)
        self.buf = self.buf[idx:]
        return True

    def send(self, line):
        self.proc.stdin.write(line.encode() + b"\n")
        self.proc.stdin.flush()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


def main():
    if not os.path.exists(KERNEL):
        print("kernel-arm.bin がありません。make arm-kernel を先に。")
        return 1
    server = http.server.HTTPServer(("0.0.0.0", 8080), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    sess = Session()
    log = open(LOG, "wb")
    try:
        ok = sess.read_until("PrismOS 27H1 (ARM64)", 30.0)
        check("boot banner (ARM)", ok)
        ok = sess.read_until("prismos> ", 30.0)
        check("shell prompt", ok)
        sess.send("net")
        ok = sess.read_until("[net] ready", 60.0)
        check("net stack ready (DHCP)", ok)
        ok = sess.read_until("net: my ip is 10.0.2.", 20.0)
        check("DHCP address 10.0.2.x", ok)
        sess.send("fetch 10.0.2.2:8080 /")
        ok = sess.read_until(MARKER, 60.0)
        check("HTTP fetch returns host page", ok)
        log.write(sess.buf)
    finally:
        sess.stop()
        log.close()
        server.shutdown()

    if FAILURES:
        print("ARM NET TEST FAILED:", ", ".join(FAILURES))
        print("serial log:", LOG)
        return 1
    print("ARM NET TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
