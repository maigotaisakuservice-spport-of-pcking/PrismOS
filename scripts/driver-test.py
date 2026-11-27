#!/usr/bin/env python3
"""PrismOS ドライバテスト (AHCI / virtio-blk / USB)。

QEMU で複数の構成を順番に起動し、本物の機器が動くことを
確かめる。AHCI と virtio-blk はディスクへの install と
再起動後の読み戻しまで確認する。USB (UHCI) は列挙に加え、
HID キーボード / マウスへの実入力 (QMP input-send-event。
QEMU 8.2.2 は device 指定つき送信で落ちるため一斉送信し、
USB 経路の証拠はゲストの "[usb] hid ..." ログで取る) と、
USB マスストレージへの書き込み・
読み戻し (ホスト側イメージでも検証) まで確認する。

Usage: python3 scripts/driver-test.py
  (exit code 0 = passed; standard library only)
"""

import json
import os
import pty
import select
import signal
import socket
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
DISK = os.path.join(ROOT, "build", "prism-driver-disk.img")
LOG = os.path.join(ROOT, "build", "boot-driver.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-driver.log")

QEMU = "qemu-system-x86_64"


class DriverTestFailure(Exception):
    pass


def make_disk():
    try:
        os.unlink(DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", DISK, "16M"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise DriverTestFailure("qemu-img failed: %s" % r.stderr)


QMP_SOCK = os.path.join(ROOT, "build", "qmp-driver.sock")


class Boot:
    def __init__(self, log_path, extra_args, qmp=False):
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(log_path, "wb")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        self.qmp_sock = None
        if qmp:
            try:
                os.unlink(QMP_SOCK)
            except OSError:
                pass
            extra_args = extra_args + [
                "-qmp", "unix:%s,server,nowait" % QMP_SOCK]
        args = [QEMU, "-cdrom", ISO] + extra_args + [
            "-boot", "order=d",
            "-m", "256M", "-serial", "stdio", "-monitor", "none",
            "-display", "none", "-no-reboot",
            "-d", "cpu_reset,int", "-D", DEBUG_LOG]
        self.proc = subprocess.Popen(args, stdin=slave_fd, stdout=slave_fd,
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
            return

    def wait_for(self, needle, timeout, what):
        needle_b = needle.encode("utf-8", "replace")
        deadline = time.monotonic() + timeout
        while True:
            idx = self.buf.find(needle_b, self.pos)
            if idx >= 0:
                self.pos = idx + len(needle_b)
                return
            if self.proc.poll() is not None:
                raise DriverTestFailure(
                    "QEMU exited early (code %s)" % self.proc.returncode)
            if time.monotonic() > deadline:
                raise DriverTestFailure("%s not seen in %s s (expected %r)"
                                       % (what, timeout, needle))
            self.pump(min(0.5, deadline - time.monotonic()))

    def send(self, text):
        os.write(self.master_fd, (text + "\r").encode("ascii"))

    # ---- QMP (USB 機器だけへ入力を送るため) ----

    def qmp_connect(self):
        deadline = time.monotonic() + 10
        while True:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(QMP_SOCK)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise DriverTestFailure("QMP socket not ready")
                time.sleep(0.2)
        self.qmp_sock = s
        self.qmp_buf = b""
        self.qmp_cmd({"execute": "qmp_capabilities"})

    def qmp_cmd(self, obj):
        data = (json.dumps(obj) + "\r\n").encode()
        self.qmp_sock.sendall(data)
        # 応答 ({"return": ...} か {"error": ...}) を 1 つ読む。
        while True:
            idx = self.qmp_buf.find(b"\n")
            if idx >= 0:
                line = self.qmp_buf[:idx]
                self.qmp_buf = self.qmp_buf[idx + 1:]
                try:
                    msg = json.loads(line.decode("utf-8", "replace"))
                except ValueError:
                    continue
                if "return" in msg or "error" in msg:
                    if "error" in msg:
                        raise DriverTestFailure(
                            "QMP error: %s" % msg["error"])
                    return msg
            chunk = self.qmp_sock.recv(4096)
            if not chunk:
                raise DriverTestFailure("QMP closed")
            self.qmp_buf += chunk

    def qmp_keys(self, device, qcodes):
        events = []
        for qc in qcodes:
            events.append({"type": "key", "data": {
                "down": True,
                "key": {"type": "qcode", "data": qc}}})
            events.append({"type": "key", "data": {
                "down": False,
                "key": {"type": "qcode", "data": qc}}})
        self.qmp_cmd({"execute": "input-send-event",
                      "arguments": {"events": events}})

    def qmp_mouse_move(self, device, dx, dy):
        events = [{"type": "rel", "data": {"axis": "x",
                                            "value": dx}},
                  {"type": "rel", "data": {"axis": "y",
                                            "value": dy}}]
        self.qmp_cmd({"execute": "input-send-event",
                      "arguments": {"events": events}})

    def qmp_mouse_click(self, device):
        events = [{"type": "btn", "data": {"down": True,
                                           "button": "left"}},
                  {"type": "btn", "data": {"down": False,
                                           "button": "left"}}]
        self.qmp_cmd({"execute": "input-send-event",
                      "arguments": {"events": events}})

    def stop(self):
        if self.qmp_sock is not None:
            try:
                self.qmp_sock.close()
            except OSError:
                pass
            self.qmp_sock = None
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        os.close(self.master_fd)
        self.log_file.close()


def disk_roundtrip(name, extra_args, expect_kind):
    """install してファイルを作り、再起動後に読めることを確認する。"""
    make_disk()
    print("disk: created 16MB disk for %s" % name)
    b = Boot(LOG, extra_args)
    try:
        b.wait_for("[boot] disk: %s " % expect_kind, 30,
                   "%s disk detected" % name)
        print("ok: %s disk detected at boot" % name)
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("disk")
        b.wait_for("disk: %s " % expect_kind, 20, "disk command")
        print("ok: disk command shows %s" % expect_kind)
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 30, "install done")
        print("ok: installed to %s disk" % name)
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("echo driver test > drv.txt")
        b.wait_for("wrote 12 bytes to drv.txt", 20, "file written")
        print("ok: file written to %s disk" % name)
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("cat drv.txt")
        b.wait_for("driver test", 20, "file readable")
        print("ok: file readable in the same boot (%s)" % name)
        b.wait_for("prismos> ", 20, "shell prompt")
    finally:
        b.stop()
    # Boot 2: 再起動後にファイルが残っていること。
    b = Boot(LOG, extra_args)
    try:
        b.wait_for("prismos> ", 30, "shell prompt after reboot")
        b.send("cat drv.txt")
        b.wait_for("driver test", 20, "file after reboot")
        print("ok: file readable after reboot (%s)" % name)
        b.wait_for("prismos> ", 20, "shell prompt")
    finally:
        b.stop()


def test_ahci():
    extra = ["-machine", "q35",
             "-device", "ahci,id=ahci",
             "-drive", "file=%s,format=raw,if=none,id=d0" % DISK,
             "-device", "ide-hd,drive=d0,bus=ahci.0"]
    disk_roundtrip("AHCI", extra, "ahci")


def test_virtio():
    extra = ["-drive", "file=%s,format=raw,if=virtio" % DISK]
    disk_roundtrip("virtio-blk", extra, "virtio")


USB_DISK = os.path.join(ROOT, "build", "prism-usb-disk.img")


def test_usb():
    # port を明示する: QEMU は自動でハブを挟むことがあり、
    # ルートポート直結の構成で HID そのものを検証する
    # (ハブ経由の機器は未対応: ハブ対応は今後の課題)。
    extra = ["-device", "piix3-usb-uhci",
             "-device", "usb-kbd,id=usbkbd,port=1",
             "-device", "usb-mouse,id=usbmouse,port=2"]
    b = Boot(LOG, extra, qmp=True)
    try:
        b.wait_for("[boot] usb: UHCI ready, 2 device(s)", 30,
                   "UHCI ready with 2 devices")
        print("ok: UHCI ready, 2 USB devices enumerated at boot")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("usb")
        b.wait_for("usb: 2 device(s)", 20, "usb command")
        b.wait_for("vendor 0x0627", 20, "QEMU USB vendor")
        b.wait_for("hid keyboard", 20, "USB keyboard kind")
        b.wait_for("hid mouse", 20, "USB mouse kind")
        print("ok: usb command lists keyboard + mouse as HID")
        b.wait_for("prismos> ", 20, "shell prompt")
        # キーボードへ "ver" と Enter を送る。USB 経路の
        # 証明はゲスト側の "[usb] hid keyboard key" ログ
        # (PS/2 経路では出ない) で取る。
        b.qmp_connect()
        b.qmp_keys("usbkbd", ["v", "e", "r"])
        b.qmp_keys("usbkbd", ["ret"])
        b.wait_for("[usb] hid keyboard key", 20, "USB HID key report")
        b.wait_for("PrismOS version 27H1", 20, "ver output via USB keyboard")
        print("ok: typed 'ver' on the USB keyboard and it ran")
        b.wait_for("prismos> ", 20, "shell prompt")
        # USB マウスを動かしてクリックする。
        b.qmp_mouse_move("usbmouse", 40, 25)
        b.qmp_mouse_click("usbmouse")
        b.wait_for("[usb] hid mouse report", 20, "USB HID mouse report")
        print("ok: USB mouse reports reach the guest")
    finally:
        b.stop()


def test_usb_storage():
    try:
        os.unlink(USB_DISK)
    except OSError:
        pass
    r = subprocess.run(["qemu-img", "create", "-f", "raw", USB_DISK,
                        "8M"], capture_output=True, text=True)
    if r.returncode != 0:
        raise DriverTestFailure("qemu-img failed: %s" % r.stderr)
    extra = ["-device", "piix3-usb-uhci",
             "-drive", "file=%s,format=raw,if=none,id=usb0" % USB_DISK,
             "-device", "usb-storage,drive=usb0"]
    b = Boot(LOG, extra)
    try:
        b.wait_for("[usb] storage ready:", 30, "USB storage probed")
        b.wait_for("storage registered as block device 1", 30,
                   "USB storage registered")
        print("ok: USB storage probed and registered as block device 1")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("usb")
        b.wait_for("storage 16384 sectors", 20, "usb storage listing")
        print("ok: usb command shows the storage capacity")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("usb test")
        b.wait_for("usb: storage test passed.", 30,
                   "usb storage write/read/verify")
        print("ok: USB storage sector write + read + verify in guest")
        b.wait_for("prismos> ", 20, "shell prompt")
    finally:
        b.stop()
    # ホスト側でも、LBA 100 にパターンが書けていることを
    # 確かめる (ゲスト内だけの自己申告で終わらせない)。
    with open(USB_DISK, "rb") as f:
        f.seek(100 * 512)
        data = f.read(512)
    expect = bytes(((i * 7 + 3) & 0xFF) for i in range(512))
    if data != expect:
        raise DriverTestFailure(
            "USB disk image does not hold the pattern at LBA 100")
    print("ok: pattern verified in the USB disk image on the host")


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    test_ahci()
    test_virtio()
    test_usb()
    test_usb_storage()
    print("DRIVER TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
