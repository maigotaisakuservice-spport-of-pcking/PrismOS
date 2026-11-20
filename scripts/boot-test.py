#!/usr/bin/env python3
"""PrismOS の QEMU 起動テスト (頑丈なドライバ)。

旧ドライバ (scripts/boot-test.sh) の問題点:
  * コマンドを壁時計の sleep だけで送る。QEMU が遅いと、シェルの
    プロンプトが出る前に送ってしまい、入力が失われる
    (QEMU は UART の FIFO を FCR 書き込みでリセットするため、
    serial_init() より前に届いたバイトは捨てられる)。
  * QEMU の生死を一切見ない。`|| true` で終了コードを隠し、
    標準入力のパイプが閉じて QEMU が死んでも、残りの sleep を
    続けてしまう。ステップ時間は QEMU の寿命ではなく sleep の
    合計になる。
  * `-serial stdio` + パイプでは、パイプの EOF で QEMU が即終了する。

このドライバは代わりに:
  * QEMU の標準入出力を pty に繋ぐ。pty のマスターが開いている限り
    EOF は来ないので、QEMU は入力待ちで死なない。
  * 出力を増分で読み、期待する文字列が出るまで待つ。
  * 各コマンドは `prismos> ` プロンプトを確認してから送る。
  * QEMU が途中で死んだら、その時点で検出してログ付きで失敗にする。
  * 全チェックが通るまで標準入力を開けたままにし、最後に QEMU を
    きれいに終了させる。

使い方: python3 scripts/boot-test.py [--log PATH]   (終了コード 0 = 合格)
標準ライブラリのみを使う。

--log を指定すると、そのパスにブートログを書く。並行して走る
テストと build/boot.log を共有すると、片方がファイルを切り詰めて
もう片方の QEMU の出力が NUL バイトまみれになるため、並行実行
する場合は必ず --log で別パスを使うこと。
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
ISO = os.path.join(ROOT, "build", "prismos.iso")
DEFAULT_LOG = os.path.join(ROOT, "build", "boot.log")
DEFAULT_STDERR_LOG = os.path.join(ROOT, "build", "qemu-stderr.log")

QEMU = "qemu-system-x86_64"
QEMU_ARGS = [
    QEMU,
    "-cdrom", ISO,
    "-m", "256M",
    "-serial", "stdio",
    "-monitor", "none",
    "-display", "none",
    "-no-reboot",
]

# (送るコマンド, その応答に含まれるはずの文字列, ラベル)
CHECKS = [
    ("help", "PrismOS shell", "help output"),
    ("ver", "PrismOS version 0.1.0", "ver output"),
    ("ls", "README.txt", "ls output"),
    ("about", "A customizable OS", "about output"),
]

PROMPT = "prismos> "
BANNER = "PrismOS 0.1.0"

PROMPT_TIMEOUT = 30.0   # プロンプト待ちの制限 (秒)
CMD_TIMEOUT = 20.0      # 1 コマンドの応答待ちの制限 (秒)
OVERALL_TIMEOUT = 90.0  # 全体の制限 (秒)


class BootTestFailure(Exception):
    pass


def main() -> int:
    parser = argparse.ArgumentParser(description="PrismOS QEMU boot test")
    parser.add_argument("--log", default=DEFAULT_LOG,
                        help="ブートログの出力先 (既定: build/boot.log)")
    parser.add_argument("--stderr-log", default=DEFAULT_STDERR_LOG,
                        help="QEMU の stderr の出力先")
    args = parser.parse_args()
    LOG = args.log
    STDERR_LOG = args.stderr_log

    if not os.path.exists(ISO):
        print("MISSING: ISO がありません: %s (先に make してください)" % ISO)
        return 1

    log_dir = os.path.dirname(LOG)
    if log_dir:
        os.makedirs(log_dir, exist_ok=True)
    log_file = open(LOG, "wb")
    stderr_file = open(STDERR_LOG, "wb")

    # pty を作る。スレーブ側は QEMU の stdin/stdout になる。
    # raw モードにしておく (行編集や \r -> \n 変換をさせないため)。
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
    # pos: すでに確認済みの出力の末尾。ここから先だけを探すことで、
    # 古いプロンプトや古い出力を「新しい応答」と誤認しない。
    pos = 0
    start = time.monotonic()
    failures = []

    def check_qemu_alive() -> None:
        rc = proc.poll()
        if rc is not None:
            raise BootTestFailure(
                "QEMU が途中で終了しました (exit code %s)。" % rc
            )

    def pump(timeout: float) -> None:
        """master から最大 timeout 秒待って読み、buf に追加する。
        読むものがなくても timeout で戻る。QEMU が死んでいたら
        BootTestFailure を投げる。"""
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
                # pty の読み手が死んだ = QEMU が終了した。
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
        """pos より後ろに needle が出るまで読む。出なければ失敗。
        途中で QEMU が死んだ場合も失敗 (理由付き)。"""
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
                raise BootTestFailure(
                    "%s が %s 秒待っても出ませんでした "
                    "(expected %r)。QEMU は生きていますが、"
                    "シェルが応答していません。" % (what, timeout, needle)
                )
            pump(min(0.5, deadline - time.monotonic()))

    def send_command(cmd: str) -> None:
        # \r で行を確定する (カーネルは \r / \n で行末とみなす)。
        os.write(master_fd, (cmd + "\r").encode("ascii"))

    try:
        # 1. バナーと最初のプロンプトを待つ。
        wait_for(BANNER, PROMPT_TIMEOUT, "boot banner")
        print("ok: boot banner")
        wait_for(PROMPT, PROMPT_TIMEOUT, "shell prompt")
        print("ok: shell prompt")

        # 2. プロンプトを確認してから 1 コマンドずつ送る。
        for cmd, needle, label in CHECKS:
            send_command(cmd)
            wait_for(needle, CMD_TIMEOUT, label)
            print("ok: %s" % label)
            # 次のプロンプトが出るまで待ってから次のコマンドを送る。
            # (シェルが次の行を読み始めた証拠になる)
            wait_for(PROMPT, CMD_TIMEOUT, "shell prompt after %s" % cmd)

        if time.monotonic() - start > OVERALL_TIMEOUT:
            raise BootTestFailure(
                "全体制限 %s 秒を超過しました" % OVERALL_TIMEOUT
            )
        print("BOOT TEST PASSED")
        return 0

    except BootTestFailure as e:
        print("MISSING: %s" % e)
        failures.append(str(e))
    except OSError as e:
        print("MISSING: QEMU との通信でエラー: %s" % e)
        failures.append(str(e))
    finally:
        # QEMU をきれいに止める。まず SIGTERM、ダメなら SIGKILL。
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        else:
            # 既に死んでいた場合は終了コードを記録する。
            print("note: QEMU exit code = %s" % proc.returncode)
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
        print("BOOT TEST FAILED")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
