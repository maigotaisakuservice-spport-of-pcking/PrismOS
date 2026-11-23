#!/usr/bin/env python3
"""PrismOS compat test (phase 16).

Boots the ISO in QEMU (no disk) and checks the compat layer
end to end, with the real guest apps:

  * `compat list` shows the 4 built-in guest apps.
  * `compat calls` shows the supported call table.
  * `compat info hello.elf` shows the ELF header info.
  * `run hello.elf` prints the greeting and exits with code 0.
  * `run calc.elf` prints "sum 1..100 = 5050" and "7! = 5040".
  * `run cat.elf about.txt` prints the file; with no argument
    it prints the usage and exits 1; with a missing file it
    says so and exits 1.
  * `run unsup.elf` proves an unknown call returns -38
    (ENOSYS) instead of a fake success.
  * `run README.txt` / `run missing.elf` fail honestly.

Usage: python3 scripts/compat-test.py
  (exit code 0 = passed; standard library only)
"""

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
LOG = os.path.join(ROOT, "build", "boot-compat.log")
DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug-compat.log")

QEMU = "qemu-system-x86_64"


class CompatTestFailure(Exception):
    pass


class Boot:
    def __init__(self):
        self.buf = bytearray()
        self.pos = 0
        self.log_file = open(LOG, "wb")
        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        attrs = termios.tcgetattr(slave_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
        self.master_fd = master_fd
        self.mon_path = os.path.join(ROOT, "build",
                                     "qemu-monitor-compat.sock")
        if os.path.exists(self.mon_path):
            os.unlink(self.mon_path)
        args = [QEMU, "-cdrom", ISO,
                "-m", "256M", "-serial", "stdio",
                "-monitor", "unix:%s,server,nowait" % self.mon_path,
                "-display", "vnc=:12", "-no-reboot",
                "-nic", "user,model=e1000",
                "-d", "cpu_reset,int", "-D", DEBUG_LOG]
        self.proc = subprocess.Popen(args, stdin=slave_fd, stdout=slave_fd,
                                     stderr=subprocess.DEVNULL, cwd=ROOT,
                                     close_fds=True)
        os.close(slave_fd)
        import socket as _socket
        self.mon = _socket.socket(_socket.AF_UNIX, _socket.SOCK_STREAM)
        for _ in range(50):
            try:
                self.mon.connect(self.mon_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        else:
            raise CompatTestFailure("monitor socket not ready")
        self.mon.settimeout(2)
        self.mon.recv(4096)

    def hmp(self, cmd):
        self.mon.sendall((cmd + "\n").encode())
        time.sleep(0.4)
        try:
            self.mon.recv(4096)
        except Exception:
            pass

    def screendump(self, path):
        self.hmp("screendump %s" % path)
        time.sleep(0.5)
        with open(path, "rb") as f:
            data = f.read()
        rest = data[3:]
        parts = []
        i = 0
        while len(parts) < 3:
            while i < len(rest) and rest[i:i + 1].isspace():
                i += 1
            j = i
            while j < len(rest) and not rest[j:j + 1].isspace():
                j += 1
            parts.append(int(rest[i:j]))
            i = j
        i += 1
        return parts[0], parts[1], rest[i:]

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
                raise CompatTestFailure(
                    "QEMU exited (code %s) while waiting for %s"
                    % (self.proc.returncode, what))
            if time.monotonic() > deadline:
                raise CompatTestFailure(
                    "timeout waiting for %s (%r)" % (what, needle))
            self.pump(0.5)

    def send(self, text):
        os.write(self.master_fd, (text + "\r").encode("utf-8"))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        os.close(self.master_fd)
        self.log_file.close()


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    failures = []
    b = Boot()
    try:
        b.wait_for("prismos> ", 30, "shell prompt")
        print("ok: shell prompt")

        b.send("compat list")
        for name in ("hello.elf", "calc.elf", "cat.elf", "unsup.elf",
                     "evil.elf", "loop.elf", "maindyn.elf", "cp.elf",
                     "wc.elf", "gfxdemo.elf", "netdemo.elf",
                     "hello-pe.exe"):
            b.wait_for(name, 20, "compat list shows %s" % name)
        print("ok: compat list shows the 12 guest apps")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("compat calls")
        b.wait_for("0  exit", 20, "calls table exit")
        b.wait_for("7  getpid", 20, "calls table getpid")
        b.wait_for("8  lseek", 20, "calls table lseek")
        b.wait_for("-38 (ENOSYS", 20, "calls table ENOSYS note")
        print("ok: compat calls shows the call table")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("compat info hello.elf")
        b.wait_for("ELF 64-bit x86-64, type EXEC, entry 0x400000",
                   20, "compat info header")
        b.wait_for("segments: 1", 20, "compat info segments")
        print("ok: compat info hello.elf")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run hello.elf")
        b.wait_for("Hello from the guest app!", 20, "guest greeting")
        b.wait_for("compat: hello.elf exited with code 0.", 20,
                   "hello exit code 0")
        print("ok: run hello.elf")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run calc.elf")
        b.wait_for("sum 1..100 = 5050", 20, "calc sum")
        b.wait_for("7! = 5040", 20, "calc factorial")
        b.wait_for("compat: calc.elf exited with code 0.", 20,
                   "calc exit code 0")
        print("ok: run calc.elf")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run cat.elf about.txt")
        b.wait_for("PrismOS is built to be customized anywhere", 20,
                   "cat reads about.txt")
        b.wait_for("compat: cat.elf exited with code 0.", 20,
                   "cat exit code 0")
        print("ok: run cat.elf about.txt")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run cat.elf")
        b.wait_for("usage: cat <file>", 20, "cat usage")
        b.wait_for("compat: cat.elf exited with code 1.", 20,
                   "cat usage exit code 1")
        print("ok: run cat.elf with no argument")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run cat.elf nope.txt")
        b.wait_for("cat: cannot open file", 20, "cat missing file")
        b.wait_for("compat: cat.elf exited with code 1.", 20,
                   "cat missing exit code 1")
        print("ok: run cat.elf nope.txt")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run unsup.elf")
        b.wait_for("compat: syscall 999 is not supported (ENOSYS).", 20,
                   "unsupported call logged")
        b.wait_for("ok: unsupported syscall returned -38", 20,
                   "guest saw ENOSYS")
        b.wait_for("compat: unsup.elf exited with code 0.", 20,
                   "unsup exit code 0")
        print("ok: run unsup.elf (ENOSYS is honest)")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run README.txt")
        b.wait_for("not an ELF file", 20, "README is not an executable")
        print("ok: run README.txt fails honestly")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run missing.elf")
        b.wait_for("No such file", 20, "missing file")
        print("ok: run missing.elf fails honestly")
        b.wait_for("prismos> ", 20, "shell prompt")

        # --- サンドボックスの検証 ---
        # 悪意ゲスト: カーネルのメモリを読もうとしてフォルトで
        # 死ぬこと。ゲストだけが終わり、シェルが応答すること。
        b.send("run evil.elf")
        b.wait_for("evil guest: touching kernel memory now", 20,
                   "evil guest started")
        b.wait_for("[sandbox] guest fault", 20,
                   "sandbox caught the fault")
        b.wait_for("guest killed, kernel alive", 20,
                   "guest killed by sandbox")
        b.wait_for("run: evil.elf: guest killed (fault, sandbox)", 20,
                   "evil run reported as killed")
        print("ok: run evil.elf killed by sandbox fault")
        b.wait_for("prismos> ", 20, "shell prompt")

        # フォルトの後も互換レイヤーが普通に使えること。
        b.send("run hello.elf")
        b.wait_for("Hello from the guest app!", 20,
                   "hello still works after evil")
        b.wait_for("compat: hello.elf exited with code 0.", 20,
                   "hello exit code 0 after evil")
        print("ok: compat layer still works after a killed guest")
        b.wait_for("prismos> ", 20, "shell prompt")

        # 暴走ゲスト: ウォッチドッグ (約 5 秒) が止めること。
        b.send("run loop.elf")
        b.wait_for("loop guest: spinning forever", 20,
                   "loop guest started")
        b.wait_for("run: loop.elf: guest killed (time limit, sandbox)",
                   30, "watchdog killed the loop guest")
        print("ok: run loop.elf killed by watchdog")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("ls")
        b.wait_for("libguest.so", 20, "libguest.so exists as a file")
        print("ok: libguest.so is in the file system")
        b.wait_for("prismos> ", 20, "shell prompt")

        # --- 動的リンクの検証 ---
        # libguest.so の .init_array が先に走り (ライブラリの
        # 初期化)、本体が PLT/GOT 経由で関数とデータを
        # 使えること。
        b.send("run maindyn.elf")
        b.wait_for("libguest.so: init ran", 20,
                   "library init array ran")
        b.wait_for("dyn link ok: sum and counter match", 20,
                   "dynamic guest used the library")
        b.wait_for("compat: maindyn.elf exited with code 0.", 20,
                   "maindyn exit code 0")
        print("ok: run maindyn.elf (dynamic linking)")
        b.wait_for("prismos> ", 20, "shell prompt")

        # --- 実用ゲスト (cp / wc) ---
        # cp: about.txt (103 バイト) を copy.txt へ写し、
        # lseek(SEEK_END) で大きさを照合して報告する。
        b.send("run cp.elf about.txt copy.txt")
        b.wait_for("cp: ok 103 bytes", 20, "cp copied about.txt")
        b.wait_for("compat: cp.elf exited with code 0.", 20,
                   "cp exit code 0")
        print("ok: run cp.elf about.txt copy.txt")
        b.wait_for("prismos> ", 20, "shell prompt")

        # 写した中身が本物であることを cat で確かめる。
        b.send("run cat.elf copy.txt")
        b.wait_for("theme, wallpaper, taskbar and Dock", 20,
                   "copied content matches")
        b.wait_for("compat: cat.elf exited with code 0.", 20,
                   "cat copy exit code 0")
        print("ok: copied file content verified")
        b.wait_for("prismos> ", 20, "shell prompt")

        # wc: about.txt は 2 行 14 語 103 バイト。
        b.send("run wc.elf about.txt")
        b.wait_for("wc: 2 14 103", 20, "wc counts about.txt")
        b.wait_for("compat: wc.elf exited with code 0.", 20,
                   "wc exit code 0")
        print("ok: run wc.elf about.txt")
        b.wait_for("prismos> ", 20, "shell prompt")

        # --- グラフィクスゲスト (gfxdemo) ---
        # 窓への描画が全部成功し、画面に赤い箱が出る。
        b.send("run gfxdemo.elf")
        b.wait_for("gfxdemo: done", 20, "gfxdemo finished")
        b.wait_for("compat: gfxdemo.elf exited with code 0.", 20,
                   "gfxdemo exit code 0")
        print("ok: run gfxdemo.elf (graphics calls)")
        b.wait_for("prismos> ", 20, "shell prompt")
        shot = os.path.join(ROOT, "build", "shots", "compat-gfx.ppm")
        os.makedirs(os.path.dirname(shot), exist_ok=True)
        w, h, raw = b.screendump(shot)

        def px(x, y):
            k = (y * w + x) * 3
            return raw[k], raw[k + 1], raw[k + 2]

        # 窓は画面中央 (外寸 656x512、内側は +8/+24)。
        wx = (w - 656) // 2
        wy = (h - 512) // 2
        red = px(wx + 8 + 50, wy + 24 + 50)
        if not (abs(red[0] - 0xC0) <= 8 and abs(red[1] - 0x39) <= 8
                and abs(red[2] - 0x2B) <= 8):
            raise CompatTestFailure(
                "guest window red box not on screen: %r" % (red,))
        print("ok: guest graphics window visible on screen")
        blue = px(wx + 8 + 300, wy + 24 + 50)
        if not (abs(blue[0] - 0x24) <= 8 and abs(blue[1] - 0x71) <= 8
                and abs(blue[2] - 0xA3) <= 8):
            raise CompatTestFailure(
                "guest window blue box not on screen: %r" % (blue,))
        print("ok: guest graphics second box visible")

        # --- ネットワークゲスト (netdemo) ---
        # ホスト側サーバー (10.0.2.2:8730) が受けて返す。
        import socket as _socket
        import threading as _threading
        received = []

        def serve_once():
            srv = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
            srv.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
            srv.bind(("127.0.0.1", 8730))
            srv.listen(1)
            srv.settimeout(60)
            try:
                conn, _ = srv.accept()
                conn.settimeout(10)
                data = b""
                while not data.endswith(b"\n"):
                    chunk = conn.recv(1024)
                    if not chunk:
                        break
                    data += chunk
                received.append(data)
                conn.sendall(b"pong from host\n")
                conn.close()
            except Exception as exc:  # noqa: BLE001
                received.append(b"ERROR: %r" % (exc,))
            finally:
                srv.close()

        th = _threading.Thread(target=serve_once, daemon=True)
        th.start()
        b.send("run netdemo.elf")
        b.wait_for("pong from host", 30, "netdemo got the reply")
        b.wait_for("netdemo: done", 20, "netdemo finished")
        b.wait_for("compat: netdemo.elf exited with code 0.", 20,
                   "netdemo exit code 0")
        th.join(timeout=10)
        if received != [b"ping from guest\n"]:
            raise CompatTestFailure(
                "host server received %r" % (received,))
        print("ok: run netdemo.elf (TCP connect/send/recv)")
        b.wait_for("prismos> ", 20, "shell prompt")

        # --- PE ゲスト (hello-pe.exe) ---
        # KERNEL32 のコンソール系サブセットで標準出力へ
        # 書き、about.txt を読み、終了コード 7 で終わる。
        b.send("run hello-pe.exe")
        b.wait_for("Hello from PE guest!", 20, "PE guest greeting")
        b.wait_for("PrismOS is built", 20,
                   "PE guest read about.txt via Win32")
        b.wait_for("compat: hello-pe.exe exited with code 7.", 20,
                   "PE guest exit code 7")
        print("ok: run hello-pe.exe (PE loader + Win32 subset)")
        b.wait_for("prismos> ", 20, "shell prompt")

        # --- 細工 ELF の回帰 (サンドボックスの穴埋め) ---
        b.send("run evil2.elf")
        b.wait_for("segment address outside user space", 20,
                   "kernel-half segment rejected")
        print("ok: run evil2.elf rejected (vaddr validation)")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run evil3.elf")
        b.wait_for("image collides with the guest stack area", 20,
                   "stack collision rejected")
        print("ok: run evil3.elf rejected (stack collision)")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run")
        b.wait_for("Usage: run <file> [args...]", 20, "run usage")
        print("ok: run with no argument shows usage")
        b.wait_for("prismos> ", 20, "shell prompt")

        # QEMU が生きていて、シェルがまだ動くこと。
        b.send("ver")
        b.wait_for("PrismOS version 27H1", 20, "shell still alive")
        print("ok: shell still alive after the guest runs")
    except CompatTestFailure as e:
        print("MISSING: %s" % e)
        failures.append(str(e))
    finally:
        b.stop()
    if failures:
        print("BOOT LOG (%s):" % LOG)
        try:
            with open(LOG, "rb") as f:
                sys.stdout.write(f.read().decode("utf-8", "replace"))
        except OSError:
            pass
        print("COMPAT TEST FAILED")
        return 1
    # デバッグログの例外チェック。サンドボックスの検証では
    # 悪意ゲストのページフォルト (0xe) が 1 回だけ起きて
    # 処理されるのが正常。それ以外の例外 (特に二重フォルト
    # 0x8 とトリプルフォルト) があれば失敗とする。
    try:
        with open(DEBUG_LOG, "rb") as f:
            dbg = f.read().decode("utf-8", "replace")
        bad = []
        for line in dbg.splitlines():
            if "Triple fault" in line:
                bad.append(line)
            elif "check_exception" in line and "new 0xe" not in line:
                bad.append(line)
        if bad:
            print("MISSING: unexpected exception in QEMU debug log:")
            for line in bad[:5]:
                print("  " + line)
            print("COMPAT TEST FAILED")
            return 1
    except OSError:
        pass
    print("COMPAT TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
