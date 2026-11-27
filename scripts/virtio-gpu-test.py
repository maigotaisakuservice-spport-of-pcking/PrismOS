#!/usr/bin/env python3
"""virtio-gpu 表示テスト (QEMU 11 必須)。

~/workspace/qemu11 の QEMU 11 で virtio-gpu-pci を付けて起動し、
GPU 側コンソールを screendump して、デスクトップが virtio-gpu
経由で表示されることを確認する。あわせて、報告モードの自動
採用 (xres/yres 未指定時) と、シェルの `res` コマンドによる
解像度変更が GPU 側へ委譲されることも確かめる。

証拠 PNG は build/shots-virtiogpu/ に残す。QEMU 8.x では
virtio-gpu に既知の不具合があるため、バージョンが 11 で
なければテストは開始しない。
"""

import os
import pty
import select
import shutil
import socket
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
OUT = os.path.join(ROOT, "build", "shots-virtiogpu")
QEMU = os.environ.get(
    "PRISM_QEMU11",
    os.path.expanduser("~/workspace/qemu11/bin/qemu-system-x86_64"))
DEPS_LIB = os.path.expanduser(
    "~/workspace/qemu11-deps/root/usr/lib/x86_64-linux-gnu")

FAILURES = []


def note(ok, name, info=""):
    print("%s: %s %s" % ("ok  " if ok else "FAIL", name, info),
          flush=True)
    if not ok:
        FAILURES.append(name)


class Fail(Exception):
    pass


class Boot:
    def __init__(self, tag, disk, gpu_args, vnc):
        self.tag = tag
        self.buf = bytearray()
        self.pos = 0
        os.makedirs(OUT, exist_ok=True)
        self.log_file = open(os.path.join(OUT, "_serial-%s.log" % tag),
                             "wb")
        self.mon_path = "/tmp/virtiogpu-%s.sock" % tag
        self.ppm = "/tmp/virtiogpu-%s.ppm" % tag
        try:
            os.unlink(self.mon_path)
        except OSError:
            pass
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = DEPS_LIB + os.pathsep + \
            env.get("LD_LIBRARY_PATH", "")
        args = [QEMU, "-cdrom", ISO, "-boot", "order=d",
                "-m", "256M", "-serial", "stdio",
                "-monitor", "unix:%s,server,nowait" % self.mon_path,
                "-display", "vnc=%s" % vnc,
                "-netdev", "user,id=net0",
                "-device", "e1000,netdev=net0",
                "-device", "sb16,audiodev=snd0",
                "-audiodev", "none,id=snd0",
                "-drive",
                "file=%s,format=raw,if=ide,index=0,media=disk" % disk]
        args += gpu_args
        self.proc = subprocess.Popen(
            args, stdin=slave_fd, stdout=slave_fd,
            stderr=subprocess.DEVNULL, cwd=ROOT, close_fds=True,
            env=env)
        os.close(slave_fd)
        self.mon = None
        for _ in range(50):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5)
                s.connect(self.mon_path)
                self.mon = s
                break
            except OSError:
                time.sleep(0.2)
        if self.mon is None:
            raise Fail("monitor socket not available")
        self.hmp("")

    def pump(self, timeout=0.3):
        r, _, _ = select.select([self.master_fd], [], [], timeout)
        if not r:
            return
        try:
            chunk = os.read(self.master_fd, 65536)
        except OSError:
            return
        if chunk:
            self.buf += chunk
            self.log_file.write(chunk)
            self.log_file.flush()

    def wait_for(self, needle, timeout, what=""):
        target = needle.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            idx = self.buf.find(target, self.pos)
            if idx >= 0:
                self.pos = idx + len(target)
                return
            if self.proc.poll() is not None:
                raise Fail("QEMU exited while waiting for %s" % what)
            self.pump(0.3)
        raise Fail("MISSING: %s (%r) not seen in %ds; tail=%r"
                   % (what, needle, timeout, bytes(self.buf[-300:])))

    def send(self, text):
        os.write(self.master_fd, (text + "\n").encode())
        time.sleep(0.3)

    def hmp(self, cmd):
        self.mon.sendall((cmd + "\n").encode("ascii"))
        data = b""
        self.mon.settimeout(5)
        try:
            while b"(qemu)" not in data:
                chunk = self.mon.recv(4096)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        return data

    def shot(self, name, device=None):
        """screendump を PNG にして保存し、画像を返す。"""
        from PIL import Image
        cmd = "screendump %s" % self.ppm
        if device:
            cmd += " %s" % device
        resp = self.hmp(cmd)
        if b"Error" in resp:
            raise Fail("screendump failed: %r" % resp[-200:])
        time.sleep(0.6)
        img = Image.open(self.ppm)
        img.load()
        dest = os.path.join(OUT, name + ".png")
        img.save(dest)
        print("    shot %s (%dx%d, %d bytes)"
              % (dest, img.size[0], img.size[1],
                 os.path.getsize(dest)), flush=True)
        return img

    def stop(self):
        try:
            self.mon.close()
        except OSError:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        os.close(self.master_fd)
        self.log_file.close()


def colors_of(img):
    """画素を間引いて数え、色数の目安を返す。"""
    rgb = img.convert("RGB")
    w, h = rgb.size
    raw = rgb.tobytes()
    seen = set()
    step = 7 * 3
    for i in range(0, len(raw) - 2, step):
        seen.add(raw[i:i + 3])
    return len(seen)


def login_desktop(b):
    b.wait_for("PrismOS login", 90, "login screen")
    b.wait_for("PIN: ", 20, "PIN prompt")
    b.send("1234")
    b.wait_for("Welcome to PrismOS.", 30, "admin login")
    b.wait_for("desktop: ready", 30, "desktop ready")
    time.sleep(2.0)


def make_installed_disk(path):
    """ISO から素のディスクへインストールし PIN を設定する。"""
    with open(path, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    b = Boot("install", path, [], ":25")
    try:
        b.wait_for("prismos> ", 90, "shell prompt")
        b.send("install yes")
        b.wait_for("language> ", 30, "language prompt")
        b.send("2")
        b.wait_for("Installed to disk", 180, "install done")
        b.send("passwd")
        b.wait_for("New PIN (4-16 chars): ", 20, "new PIN prompt")
        b.send("1234")
        b.wait_for("Confirm PIN: ", 20, "confirm prompt")
        b.send("1234")
        b.wait_for("Account set.", 20, "PIN set")
        b.wait_for("prismos> ", 20, "prompt after passwd")
        b.send("sync")
        b.wait_for("prismos> ", 20, "prompt after sync")
    finally:
        b.stop()


def phase_sized(base_disk):
    """xres=1280,yres=800 を明示した GPU で表示と解像度変更。"""
    disk = os.path.join(OUT, "disk-sized.img")
    shutil.copyfile(base_disk, disk)
    b = Boot("sized", disk,
             ["-device", "virtio-gpu-pci,id=gpu0,xres=1280,yres=800"],
             ":26")
    try:
        b.wait_for("virtio-gpu: display ready", 60, "gpu init")
        b.wait_for("1280x800 via virtio-gpu", 30, "gpu fb line")
        note(True, "gpu-init", "1280x800 を GPU 表示として採用")
        login_desktop(b)
        img = b.shot("gpu-desktop-1280x800", device="gpu0")
        note(img.size == (1280, 800), "gpu-size",
             "size=%s" % (img.size,))
        n = colors_of(img)
        note(n > 100, "gpu-colorful", "sampled colors=%d" % n)
        vga = b.shot("vga-console-1280x800")
        note(vga.tobytes() != img.tobytes(), "gpu-differs-from-vga",
             "GPU 画面が VGA コンソールと異なる")
        # 解像度変更が GPU へ委譲されること。
        b.send("res 1024 768")
        b.wait_for("res: 1024x768", 30, "res changed")
        time.sleep(1.5)
        img2 = b.shot("gpu-desktop-1024x768", device="gpu0")
        note(img2.size == (1024, 768), "gpu-resize",
             "size=%s" % (img2.size,))
        note(colors_of(img2) > 100, "gpu-resize-colorful",
             "sampled colors=%d" % colors_of(img2))
        b.send("res 1280 800")
        b.wait_for("res: 1280x800", 30, "res restored")
    finally:
        b.stop()


def phase_s3(base_disk):
    """S3 スリープ/復帰後も GPU 表示が戻ること。"""
    disk = os.path.join(OUT, "disk-s3.img")
    shutil.copyfile(base_disk, disk)
    b = Boot("s3", disk,
             ["-device", "virtio-gpu-pci,id=gpu0,xres=1280,yres=800"],
             ":28")
    try:
        b.wait_for("virtio-gpu: display ready", 60, "gpu init")
        login_desktop(b)
        b.shot("gpu-s3-before", device="gpu0")
        b.send("sleep")
        b.wait_for("power: entering S3 sleep", 20, "s3 enter")
        time.sleep(3.0)
        b.hmp("system_wakeup")
        b.wait_for("power: resumed from S3", 60, "s3 resumed")
        time.sleep(2.0)
        img = b.shot("gpu-s3-after", device="gpu0")
        note(img.size == (1280, 800), "gpu-s3-size",
             "size=%s" % (img.size,))
        note(colors_of(img) > 100, "gpu-s3-colorful",
             "sampled colors=%d" % colors_of(img))
    finally:
        b.stop()


def phase_default(base_disk):
    """xres/yres 未指定: 機器報告モードをそのまま使う。"""
    disk = os.path.join(OUT, "disk-default.img")
    shutil.copyfile(base_disk, disk)
    b = Boot("default", disk,
             ["-device", "virtio-gpu-pci,id=gpu0"], ":27")
    try:
        b.wait_for("virtio-gpu: display ready", 60, "gpu init")
        login_desktop(b)
        img = b.shot("gpu-desktop-default", device="gpu0")
        n = colors_of(img)
        note(img.size[0] >= 640 and img.size[1] >= 480,
             "gpu-default-size", "size=%s" % (img.size,))
        note(n > 100, "gpu-default-colorful",
             "sampled colors=%d" % n)
    finally:
        b.stop()


def main():
    if not os.path.exists(QEMU):
        print("FAIL: QEMU 11 not found at %s" % QEMU)
        return 1
    ver = subprocess.run([QEMU, "--version"], capture_output=True,
                         text=True).stdout
    print("QEMU: %s" % ver.splitlines()[0] if ver else "?")
    if "version 11." not in ver:
        print("FAIL: QEMU 11 is required (got: %s)" % ver.strip())
        return 1
    if not os.path.exists(ISO):
        print("FAIL: ISO not built: %s" % ISO)
        return 1
    os.makedirs(OUT, exist_ok=True)
    base_disk = os.path.join(OUT, "disk-base.img")
    try:
        make_installed_disk(base_disk)
        note(True, "install", "テスト用ディスクを用意")
    except Fail as e:
        note(False, "install", str(e))
        return 1
    for phase in (phase_sized, phase_s3, phase_default):
        try:
            phase(base_disk)
        except Fail as e:
            note(False, phase.__name__, str(e))
    if FAILURES:
        print("VIRTIO-GPU TEST FAILED: %s" % ", ".join(FAILURES))
        return 1
    print("VIRTIO-GPU TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
