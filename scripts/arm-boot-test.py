#!/usr/bin/env python3
"""PrismOS の ARM 起動テスト (QEMU virt 構成、AArch64)。

QEMU の標準入出力を pty に繋ぎ、バナーとプロンプトを待ってから
1 コマンドずつ送る (x86 版の scripts/boot-test.py と同じ流儀)。
期待する文字列が出るまで待つ。QEMU が途中で死んだら失敗にする。

使い方: python3 scripts/arm-boot-test.py [--log PATH]
  (終了コード 0 = 合格)
標準ライブラリのみを使う。RAM は既定 256MB で、環境変数
PRISM_ARM_MEM_MB で変更できる (例: 4096 で 4GB 構成を確認する)。
カーネルは DTB の示す RAM 末尾まで MMU で写像し、PMM で管理する。

チェック:
  * バナー "PrismOS 27H1 (ARM64)"
  * "[boot] gic+timer ok" (GIC と汎用タイマの割り込みが本当に来た証拠)
  * help / ver / ls / about / echo > / cat / mem / ticks / rm
  * reboot で QEMU が終了すること (PSCI リセット + -no-reboot)
"""

import argparse
import os
import pty
import re
import select
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "build", "kernel-arm.bin")
DEFAULT_LOG = os.path.join(ROOT, "build", "arm-boot.log")
DEFAULT_STDERR_LOG = os.path.join(ROOT, "build", "arm-qemu-stderr.log")

QEMU = "qemu-system-aarch64"
MEM_MB = os.environ.get("PRISM_ARM_MEM_MB", "256")
QEMU_ARGS = [
    QEMU,
    "-M", "virt",
    "-cpu", "cortex-a57",
    "-m", MEM_MB,
    "-device", "ramfb",
    "-serial", "stdio",
    "-monitor", "none",
    "-display", "none",
    "-no-reboot",
    "-kernel", KERNEL,
]

CHECKS = [
    ("help", "PrismOS shell", "help output"),
    ("ver", "PrismOS version 27H1", "ver output"),
    ("ls", "README.txt", "ls output"),
    ("about", "A customizable OS", "about output"),
    ("echo hello arm > note.txt", "wrote 10 bytes to note.txt",
     "echo redirect write"),
    ("cat note.txt", "hello arm", "cat created file"),
    ("mem", "memory:", "mem command"),
    ("ticks", "ticks:", "ticks command"),
    ("rm note.txt", "removed note.txt", "rm file"),
    ("cat note.txt", "No such file", "cat removed file"),
]

PROMPT = "prismos> "
BANNER = "PrismOS 27H1 (ARM64)"
TIMER_OK = "gic+timer ok"
FB_OK = "[boot] fb: ramfb 1024x768"

PROMPT_TIMEOUT = 30.0
CMD_TIMEOUT = 20.0
OVERALL_TIMEOUT = 90.0


class ArmBootTestFailure(Exception):
    pass


def main() -> int:
    parser = argparse.ArgumentParser(description="PrismOS ARM boot test")
    parser.add_argument("--log", default=DEFAULT_LOG,
                        help="ブートログの出力先 (既定: build/arm-boot.log)")
    parser.add_argument("--stderr-log", default=DEFAULT_STDERR_LOG,
                        help="QEMU の stderr の出力先")
    args = parser.parse_args()
    LOG = args.log
    STDERR_LOG = args.stderr_log

    if not os.path.exists(KERNEL):
        print("MISSING: ARM カーネルがありません: %s (先に make arm-kernel してください)" % KERNEL)
        return 1

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

    def check_qemu_alive() -> None:
        rc = proc.poll()
        if rc is not None:
            raise ArmBootTestFailure(
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
                raise ArmBootTestFailure(
                    "%s が %s 秒待っても出ませんでした "
                    "(expected %r)。QEMU は生きていますが、"
                    "シェルが応答していません。" % (what, timeout, needle)
                )
            pump(min(0.5, deadline - time.monotonic()))

    def send_command(cmd: str) -> None:
        os.write(master_fd, (cmd + "\r").encode("ascii"))

    try:
        wait_for(BANNER, PROMPT_TIMEOUT, "boot banner")
        print("ok: boot banner")
        wait_for(TIMER_OK, PROMPT_TIMEOUT, "gic+timer ok")
        print("ok: gic+timer ok")
        wait_for(FB_OK, PROMPT_TIMEOUT, "ramfb framebuffer")
        print("ok: ramfb framebuffer 1024x768")
        wait_for(PROMPT, PROMPT_TIMEOUT, "shell prompt")
        print("ok: shell prompt")

        for cmd, needle, label in CHECKS:
            send_command(cmd)
            wait_for(needle, CMD_TIMEOUT, label)
            print("ok: %s" % label)
            wait_for(PROMPT, CMD_TIMEOUT, "shell prompt after %s" % cmd)

        # reboot: PSCI リセットで QEMU が終了するはず (-no-reboot)。
        send_command("reboot")
        wait_for("Rebooting...", CMD_TIMEOUT, "reboot message")
        print("ok: reboot message")
        deadline = time.monotonic() + 10.0
        while proc.poll() is None:
            if time.monotonic() > deadline:
                raise ArmBootTestFailure(
                    "reboot しても QEMU が 10 秒経っても終了しませんでした。"
                )
            time.sleep(0.2)
        print("ok: QEMU exited after reboot (exit code %s)" % proc.returncode)

        # メモリ量の確認: DTB の RAM 末尾まで写像され、PMM の
        # 総ページ数が -m のサイズに見合うこと (1MB あたり
        # 256 ページ。カーネル分を差し引いても 200 以上はある)。
        text = bytes(buf).decode("utf-8", "replace")
        if "RAM mapped to" not in text:
            raise ArmBootTestFailure("MMU の写像範囲の表示がありません")
        if (", %s MB from DTB or fallback" % MEM_MB) not in text:
            raise ArmBootTestFailure(
                "RAM サイズ %s MB が DTB から読めていません" % MEM_MB)
        m = re.search(r"memory: (\d+) pages total", text)
        if not m or int(m.group(1)) < int(MEM_MB) * 200:
            raise ArmBootTestFailure(
                "PMM の総ページ数が RAM サイズに見合いません: %s"
                % (m.group(1) if m else "mem 出力なし"))
        print("ok: PMM manages %s pages for %s MB RAM"
              % (m.group(1), MEM_MB))

        if time.monotonic() - start > OVERALL_TIMEOUT:
            raise ArmBootTestFailure(
                "全体制限 %s 秒を超過しました" % OVERALL_TIMEOUT
            )
        print("ARM BOOT TEST PASSED")
        return 0

    except ArmBootTestFailure as e:
        print("MISSING: %s" % e)
    except OSError as e:
        print("MISSING: QEMU との通信でエラー: %s" % e)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        log_file.close()
        stderr_file.close()
    return 1


if __name__ == "__main__":
    sys.exit(main())
