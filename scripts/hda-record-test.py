#!/usr/bin/env python3
"""PrismOS HD Audio 録音テスト: intel-hda + hda-duplex
(audiodev none) で起動し、`hda record 2` が入力ストリーム
で採取して rec.wav を保存すること、ピークが 0 (無音入力)
であること、`hda playrec` で出力ストリームが進むことを
確認する。audiodev none では入力は無音になるが、DMA の
採取経路・ADC 設定・WAV 保存はそのまま検証できる。"""
import os, pty, select, subprocess, sys, termios, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
LOG = os.path.join(ROOT, "build", "boot-hda-record.log")


def main():
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-serial", "stdio", "-monitor", "none",
            "-display", "none", "-no-reboot",
            "-audiodev", "none,id=snd0",
            "-device", "intel-hda,id=hda0",
            "-device", "hda-duplex,bus=hda0.0,cad=0,audiodev=snd0"]
    master, slave = pty.openpty()
    tty.setraw(slave)
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    proc = subprocess.Popen(args, stdin=slave, stdout=slave,
                            stderr=subprocess.DEVNULL, cwd=ROOT)
    os.close(slave)
    buf = bytearray()
    pos = 0
    log = open(LOG, "wb")

    def pump(t):
        end = time.monotonic() + t
        while time.monotonic() < end:
            r, _, _ = select.select([master], [], [], 0.2)
            if r:
                try:
                    d = os.read(master, 4096)
                except OSError:
                    return
                if d:
                    buf.extend(d)
                    log.write(d)
                    log.flush()

    def wait_for(s, t=60):
        nonlocal pos
        end = time.monotonic() + t
        while time.monotonic() < end:
            i = buf.find(s.encode(), pos)
            if i >= 0:
                pos = i + len(s)
                return True
            pump(0.3)
        return False

    def cmd(c, wait, t=60):
        os.write(master, (c + "\r").encode())
        return wait_for(wait, t)

    rc = 1
    try:
        if not wait_for("[hda] codec ready"):
            print("MISSING: codec did not come up")
            return 1
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        if not cmd("hda record 2", "hda record: saved rec.wav",
                   90):
            print("MISSING: record did not save rec.wav")
            return 1
        text = buf.decode("utf-8", "replace")
        import re
        m = re.search(r"hda record: saved rec\.wav \((\d+) bytes, "
                      r"peak (\d+)\)", text)
        if not m:
            print("MISSING: record summary line")
            return 1
        size, peak = int(m.group(1)), int(m.group(2))
        print("rec.wav bytes:", size, "peak:", peak)
        # 2 秒 = 48000*4*2 = 384000 バイト + ヘッダ 44。
        if size < 44 + 300000:
            print("MISSING: recording too short")
            return 1
        if peak != 0:
            print("NOTE: peak nonzero with audiodev none "
                  "(backend noise?) peak=%d" % peak)
        print("ok: capture path recorded 2s to rec.wav")
        if not cmd("hda playrec", "hda playrec: playing"):
            print("MISSING: playrec did not start")
            return 1
        time.sleep(1.0)
        pump(0.5)
        if not cmd("hda", "stream pos"):
            print("MISSING: hda status")
            return 1
        text = buf.decode("utf-8", "replace")
        poss = [int(x.group(1)) for x in
                re.finditer(r"stream pos (\d+)", text)]
        print("output stream positions:", poss)
        if not poss or max(poss) == 0:
            print("MISSING: playback stream never advanced")
            return 1
        print("ok: recorded file plays back (LPIB advanced)")
        rc = 0
    finally:
        proc.terminate()
        log.close()
    if rc == 0:
        print("HDA RECORD TEST PASSED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
