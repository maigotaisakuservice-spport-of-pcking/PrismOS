#!/usr/bin/env python3
"""PrismOS の 32bit 起動テスト (qemu-system-i386、Multiboot1)。

build/prismos-32.iso を 32bit 専用 CPU のエミュレータ
(qemu-system-i386) で起動し、確認する:
  * バナー "PrismOS 27H1 (32-bit)"
  * Multiboot 情報からメモリ量を読めていること
  * help / ver / ls / about / echo > / cat / mem / rm
    (64bit 版と同じシェルコア shell/core.c が 32bit で動く)
  * reboot で QEMU が終了すること (8042 リセット + -no-reboot)

使い方: python3 scripts/boot32-test.py [--log PATH]
  (終了コード 0 = 合格。標準ライブラリのみ)
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
ISO = os.path.join(ROOT, "build", "prismos-32.iso")
DEFAULT_LOG = os.path.join(ROOT, "build", "boot32.log")
DEFAULT_STDERR_LOG = os.path.join(ROOT, "build", "qemu-stderr32.log")
DEFAULT_DEBUG_LOG = os.path.join(ROOT, "build", "qemu-debug32.log")

QEMU = "qemu-system-i386"
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor32.sock")
QEMU_ARGS = [
    QEMU,
    "-cdrom", ISO,
    "-boot", "order=d",
    "-m", "256M",
    "-serial", "stdio",
    "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
    "-display", "vnc=:9",
    "-no-reboot",
    "-drive", "file=%s,if=ide,format=raw" % os.path.join(
        ROOT, "build", "disk32.img"),
    "-d", "cpu_reset,int",
    "-D", DEFAULT_DEBUG_LOG,
]

CHECKS = [
    ("help", "PrismOS shell", "help output"),
    ("ver", "PrismOS version 27H1", "ver output"),
    ("ls", "README.txt", "ls output"),
    ("about", "A customizable OS", "about output"),
    ("echo hello 32bit > note.txt", "wrote 12 bytes to note.txt",
     "echo redirect write"),
    ("cat note.txt", "hello 32bit", "cat created file"),
    ("mem", "memory:", "mem command"),
    ("disk", "disk: ata", "disk command"),
    ("disk test", "disk test: ok", "disk pattern test"),
    ("rm note.txt", "removed note.txt", "rm file"),
    ("cat note.txt", "No such file", "cat removed file"),
]

PROMPT = "prismos> "
BANNER = "PrismOS 27H1 (32-bit)"
MEMINFO = "[boot] multiboot: memory"

PROMPT_TIMEOUT = 30.0
CMD_TIMEOUT = 20.0
OVERALL_TIMEOUT = 120.0


class Boot32TestFailure(Exception):
    pass


def main() -> int:
    parser = argparse.ArgumentParser(description="PrismOS 32-bit boot test")
    parser.add_argument("--log", default=DEFAULT_LOG,
                        help="ブートログの出力先 (既定: build/boot32.log)")
    parser.add_argument("--stderr-log", default=DEFAULT_STDERR_LOG,
                        help="QEMU の stderr の出力先")
    args = parser.parse_args()
    LOG = args.log
    STDERR_LOG = args.stderr_log

    if not os.path.exists(ISO):
        print("MISSING: 32bit ISO がありません: %s "
              "(先に make iso32 してください)" % ISO)
        return 1

    with open(os.path.join(ROOT, "build", "disk32.img"),
              "wb") as df:
        df.truncate(8 * 1024 * 1024)
    log_dir = os.path.dirname(LOG)
    if log_dir:
        os.makedirs(log_dir, exist_ok=True)
    log_file = open(LOG, "wb")
    stderr_file = open(STDERR_LOG, "wb")

    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)

    proc = subprocess.Popen(
        QEMU_ARGS,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=stderr_file,
        cwd=ROOT,
        close_fds=True,
    )
    os.close(slave_fd)

    buf = bytearray()
    pos = 0
    start = time.monotonic()
    failures = []

    def check_qemu_alive() -> None:
        rc = proc.poll()
        if rc is not None:
            raise Boot32TestFailure(
                "QEMU が途中で終了しました (exit code %s)。" % rc
            )

    def pump(timeout: float) -> None:
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
                check_qemu_alive()
                return
            if not chunk:
                check_qemu_alive()
                return
            buf.extend(chunk)
            log_file.write(chunk)
            log_file.flush()
            return

    def wait_for(needle: str, timeout: float, what: str) -> None:
        nonlocal pos
        needle_b = needle.encode("utf-8", "replace")
        deadline = time.monotonic() + timeout
        while True:
            idx = buf.find(needle_b, pos)
            if idx >= 0:
                pos = idx + len(needle_b)
                return
            check_qemu_alive()
            if time.monotonic() > deadline:
                raise Boot32TestFailure(
                    "%s が %s 秒待っても出ませんでした "
                    "(expected %r)。QEMU は生きていますが、"
                    "シェルが応答していません。" % (what, timeout, needle)
                )
            pump(min(0.5, deadline - time.monotonic()))

    def send_command(cmd: str) -> None:
        os.write(master_fd, (cmd + "\r").encode("ascii"))

    try:
        wait_for(BANNER, PROMPT_TIMEOUT, "boot banner")
        print("ok: boot banner (32-bit)")
        wait_for(MEMINFO, PROMPT_TIMEOUT, "multiboot memory info")
        print("ok: multiboot memory info")
        wait_for(PROMPT, PROMPT_TIMEOUT, "shell prompt")
        print("ok: shell prompt")

        for cmd, needle, label in CHECKS:
            send_command(cmd)
            wait_for(needle, CMD_TIMEOUT, label)
            print("ok: %s" % label)
            wait_for(PROMPT, CMD_TIMEOUT, "shell prompt after %s" % cmd)

        # 画面 (VGA テキスト) にもバナーが出ていることを、
        # スクリーンダンプの白い画素で確かめる。
        import socket as _socket
        mon = _socket.socket(_socket.AF_UNIX, _socket.SOCK_STREAM)
        mon.connect(MONITOR_SOCK)
        mon.settimeout(2)
        mon.recv(4096)

        def hmp(cmd):
            mon.sendall((cmd + "\n").encode())
            time.sleep(0.4)
            try:
                mon.recv(4096)
            except _socket.timeout:
                pass

        shot = os.path.join(ROOT, "build", "shots", "boot32-vga.ppm")
        os.makedirs(os.path.dirname(shot), exist_ok=True)
        hmp("screendump %s" % shot)
        time.sleep(0.5)
        with open(shot, "rb") as f:
            data = f.read()
        # P6 ヘッダを読み、左上のテキスト行領域の白い画素を数える。
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
        w = parts[0]
        raw = rest[i:]
        # VGA テキストの文字色は属性 0x07 = 明るい灰色
        # (170,170,170) なので、閾値は 140 にする。
        white = 0
        for y in range(0, 60):
            for x in range(0, 300):
                k = (y * w + x) * 3
                if raw[k] > 140 and raw[k + 1] > 140 and raw[k + 2] > 140:
                    white += 1
        if white < 100:
            raise Boot32TestFailure(
                "VGA 画面にバナーの文字が見えません (%d 明画素)" % white)
        print("ok: VGA text screen shows the banner (%d bright px)"
              % white)

        # PS/2 キーボードから打ったコマンドがシェルに届く。
        for key in ("v", "e", "r"):
            hmp("sendkey %s" % key)
        hmp("sendkey ret")
        wait_for("PrismOS version 27H1", CMD_TIMEOUT,
                 "ver typed on the PS/2 keyboard")
        print("ok: PS/2 keyboard input reaches the shell")
        wait_for(PROMPT, CMD_TIMEOUT, "shell prompt after kbd ver")
        mon.close()

        send_command("reboot")
        wait_for("Rebooting...", CMD_TIMEOUT, "reboot message")
        print("ok: reboot message")
        deadline = time.monotonic() + 10.0
        while proc.poll() is None:
            if time.monotonic() > deadline:
                raise Boot32TestFailure(
                    "reboot しても QEMU が 10 秒経っても終了しませんでした。"
                )
            time.sleep(0.2)
        print("ok: QEMU exited after reboot (exit code %s)" % proc.returncode)

        if time.monotonic() - start > OVERALL_TIMEOUT:
            raise Boot32TestFailure(
                "全体制限 %s 秒を超過しました" % OVERALL_TIMEOUT
            )
        print("BOOT32 TEST PASSED")
        return 0

    except Boot32TestFailure as e:
        print("MISSING: %s" % e)
        failures.append(str(e))
    except OSError as e:
        print("MISSING: QEMU との通信でエラー: %s" % e)
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
        print("--- boot log (%s) ---" % LOG)
        try:
            with open(LOG, "rb") as f:
                sys.stdout.write(f.read().decode("utf-8", "replace"))
        except OSError:
            pass
        for label, path in (("qemu debug log", DEFAULT_DEBUG_LOG),
                            ("qemu stderr", STDERR_LOG)):
            try:
                with open(path, "rb") as f:
                    data = f.read()
                if data.strip():
                    print("--- %s (%s) ---" % (label, path))
                    sys.stdout.write(data.decode("utf-8", "replace"))
            except OSError:
                pass
        print("BOOT32 TEST FAILED")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
