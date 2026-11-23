#!/usr/bin/env python3
"""PrismOS の長時間安定テスト (ソークテスト)。

QEMU を指定した分数だけ動かし続け、1 分おきに `mem` を送って
PMM の空きページ数とヒープの空きバイト数を記録する。最後に
応答があることと、空き量が減っていないこと (リークが無いこと)
を確認する。標準ライブラリのみを使う。

使い方: python3 scripts/soak-test.py [--minutes 30] [--log PATH]
  (終了コード 0 = 合格)
QEMU は -serial stdio を pty に繋いで動かす (boot-test.py と同じ形)。
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
ISO = os.path.join(ROOT, "build", "prismos.iso")
DEFAULT_LOG = os.path.join(ROOT, "build", "soak.log")
DEFAULT_DISK = os.path.join(ROOT, "build", "soak-disk.img")

QEMU = "qemu-system-x86_64"
PROMPT = b"prismos> "


def make_disk(path, size_mb=16):
    with open(path, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)


class Soak:
    def __init__(self, log_path):
        self.log_path = log_path
        self.buf = b""
        self.last_output = b""
        self.logf = open(log_path, "wb")

    def log(self, data):
        self.logf.write(data)
        self.logf.flush()

    def read_until(self, want, timeout, proc):
        """want が出るか QEMU が死ぬかタイムアウトまで読む。"""
        end = time.time() + timeout
        while time.time() < end:
            if want in self.buf:
                return True
            if proc.poll() is not None:
                return False
            r, _, _ = select.select([self.master], [], [], 0.5)
            if r:
                try:
                    data = os.read(self.master, 65536)
                except OSError:
                    return False
                if not data:
                    return False
                self.buf += data
                self.log(data)
        return want in self.buf

    def send(self, text):
        os.write(self.master, text.encode())

    def wait_prompt(self, timeout, proc):
        if not self.read_until(PROMPT, timeout, proc):
            return False
        # プロンプトより前が直前のコマンドの出力。残しておく。
        idx = self.buf.rfind(PROMPT)
        self.last_output = self.buf[:idx]
        self.buf = self.buf[idx + len(PROMPT):]
        return True

    def run(self, minutes, interval):
        make_disk(DEFAULT_DISK)
        master, slave = pty.openpty()
        self.master = master
        # pty を raw にする
        attrs = termios.tcgetattr(master)
        tty.setraw(master)
        cmd = [
            QEMU,
            "-cdrom", ISO,
            "-drive", "file=%s,format=raw,if=ide" % DEFAULT_DISK,
            "-m", "256M",
            "-serial", "stdio",
            "-monitor", "none",
            "-display", "none",
            "-no-reboot",
        ]
        proc = subprocess.Popen(
            cmd, stdin=slave, stdout=slave, stderr=subprocess.DEVNULL,
            close_fds=True)
        os.close(slave)
        samples = []  # (elapsed_sec, free_pages, heap_free)
        try:
            if not self.wait_prompt(90, proc):
                print("MISSING: 起動後のプロンプトが出ない")
                return 1
            print("ok: 起動した (プロンプト確認)")
            # ディスクに一度入れて、永続化の経路も使う
            self.send("install yes\n")
            if not self.wait_prompt(60, proc):
                print("MISSING: install 後のプロンプトが出ない")
                return 1
            print("ok: install 完了")
            end = time.time() + minutes * 60
            n = 0
            while time.time() < end:
                n += 1
                self.send("mem\n")
                if not self.wait_prompt(30, proc):
                    print("MISSING: %d 分目の mem で応答が無い" % n)
                    return 1
                # 直前の mem 出力から空き量を抜く
                text = self.last_output.decode(errors="replace")
                m = re.search(r"memory: \d+ pages total \(\d+ MB\), (\d+) free, (\d+) used", text)
                h = re.search(r"heap: \d+ bytes total, (\d+) free, (\d+) used", text)
                if not m or not h:
                    print("MISSING: %d 分目の mem 出力が読めない" % n)
                    return 1
                free_pages = int(m.group(1))
                heap_free = int(h.group(1))
                elapsed = int(time.time() - (end - minutes * 60))
                samples.append((elapsed, free_pages, heap_free))
                print("ok: %d 分目 free_pages=%d heap_free=%d" %
                      (elapsed // 60, free_pages, heap_free))
                self.buf = b""
                # 次の 1 分まで待つ (QEMU が死んでいないか見る)
                deadline = time.time() + interval
                while time.time() < deadline:
                    if proc.poll() is not None:
                        print("MISSING: 実行中に QEMU が終了した (code %s)" % proc.returncode)
                        return 1
                    time.sleep(1)
            # 最後に ls / ver で応答を確認する
            self.send("ls\n")
            if not self.wait_prompt(30, proc):
                print("MISSING: 最後の ls で応答が無い")
                return 1
            self.send("ver\n")
            if not self.wait_prompt(30, proc):
                print("MISSING: 最後の ver で応答が無い")
                return 1
            print("ok: 最後まで応答あり")
        finally:
            try:
                proc.terminate()
            except Exception:
                pass
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()
            os.close(master)
            self.logf.close()
        # 空き量の推移を判定する。最初と最後だけでなく、
        # 全サンプルの単調性と全体の傾きを見る: ゆっくり
        # 減り続けるリークは中間サンプルでも検出する。
        first = samples[0]
        last = samples[-1]
        print("samples: %d, first free_pages=%d heap_free=%d, "
              "last free_pages=%d heap_free=%d" %
              (len(samples), first[1], first[2], last[1], last[2]))
        drops = 0
        for i in range(1, len(samples)):
            if (samples[i][1] < samples[i - 1][1] or
                    samples[i][2] < samples[i - 1][2]):
                drops += 1
        print("free-amount decreases between samples: %d of %d" %
              (drops, max(0, len(samples) - 1)))
        # 判定: 最終値が最初より減っている、または全体の
        # 半分以上の区間で減り続けている場合はリーク疑い。
        if last[1] < first[1] or last[2] < first[2]:
            print("MISSING: 空き量が減った (リークの疑い)")
            return 1
        if len(samples) >= 4 and drops * 2 >= len(samples) - 1:
            print("MISSING: 空き量が継続的に減っている "
                  "(リークの疑い)")
            return 1
        print("SOAK TEST PASSED (%d 分, 空き量は安定)" % minutes)
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=int, default=30)
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("--log", default=DEFAULT_LOG)
    args = ap.parse_args()
    s = Soak(args.log)
    return s.run(args.minutes, args.interval)


if __name__ == "__main__":
    sys.exit(main())
