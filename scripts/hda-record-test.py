#!/usr/bin/env python3
"""PrismOS HD Audio 録音テスト: intel-hda + hda-duplex で起動し、
録音経路の到達点を確認する。

観測された現実 (2026-11-24、QEMU 8.2.2 で実測):
audiodev none でも、sdl + SDL_AUDIODRIVER=dummy でも、入力
ストリームの DMA は進まない (LPIB が 0 のまま)。そのため
`hda record 2` は 0 バイト採取で正直に失敗し、rec.wav は
保存されない。実サンプルが届くところまでは現環境では
観測できない (実キャプチャバックエンドか実機が必要)。
このテストが固定するのは次の観測可能な範囲である:
- コーデックが上がり、ADC とマイクピンが設定されること。
- `hda record` が DMA 設定 (BDL・ストリーム RUN) まで進み、
  LPIB をポーリングして 0 バイトを報告し、偽の rec.wav を
  作らずに失敗すること。シェルは生き続けること。
採取ロジックそのもの (BDL 構築・目標バイト数・DMA 位置から
の取得量・ピーク走査) はホストテスト test_hda_cap (54 件)
で同じコードを検証している。
"""
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
        print("ok: codec ready")
        if not wait_for("adc nid"):
            print("MISSING: ADC not configured")
            return 1
        print("ok: ADC + mic pin configured")
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        # 録音を試す: DMA は進まないので 0 バイトで失敗する。
        # 偽の rec.wav を作らず、正直に失敗することを確認する。
        if not cmd("hda record 2", "[hda] captured 00000000 bytes",
                   90):
            print("MISSING: record did not report 0 bytes captured")
            return 1
        print("ok: record polled LPIB, captured 0 bytes "
              "(no capture backend in QEMU)")
        if not wait_for("hda record: failed", 20):
            print("MISSING: record failure not reported")
            return 1
        print("ok: record failed honestly, no fake file")
        if not cmd("ls", "prismos> ", 30):
            print("MISSING: shell did not respond after record")
            return 1
        if b"rec.wav" in bytes(buf):
            print("MISSING: rec.wav exists although capture failed")
            return 1
        print("ok: no rec.wav saved; shell still alive")
        rc = 0
    finally:
        proc.terminate()
        log.close()
    if rc == 0:
        print("HDA RECORD TEST PASSED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
