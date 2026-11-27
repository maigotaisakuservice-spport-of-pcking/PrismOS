#!/usr/bin/env python3
"""PrismOS HD Audio test: boot with intel-hda + hda-duplex
and a WAV-capturing audio backend. The driver must find the
codec, `hda beep` must start the output stream (LPIB
advances), and the captured WAV must contain the tone
(non-silent samples)."""
import os, pty, select, struct, subprocess, sys, termios, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
WAV = os.path.join(ROOT, "build", "hda-out.wav")
LOG = os.path.join(ROOT, "build", "boot-hda.log")


def main():
    if os.path.exists(WAV):
        os.unlink(WAV)
    args = ["qemu-system-x86_64", "-cdrom", ISO, "-boot", "d",
            "-m", "256M", "-serial", "stdio", "-monitor", "none",
            "-display", "none", "-no-reboot",
            "-audiodev", "wav,id=snd0,path=%s" % WAV,
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

    def cmd(c, wait, t=30):
        os.write(master, (c + "\r").encode())
        return wait_for(wait, t)

    try:
        if not wait_for("[hda] codec ready"):
            print("MISSING: codec did not come up")
            return 1
        print("ok: HDA codec enumerated and configured")
        if not wait_for("prismos> "):
            print("MISSING: shell prompt")
            return 1
        if not cmd("hda beep", "hda beep: playing"):
            print("MISSING: beep did not start")
            return 1
        time.sleep(1.0)
        pump(0.5)
        # stream position must have advanced
        if not cmd("hda", "stream pos"):
            print("MISSING: hda status")
            return 1
        text = buf.decode("utf-8", "replace")
        import re
        poss = [int(m.group(1)) for m in
                re.finditer(r"stream pos (\d+)", text)]
        print("stream positions seen:", poss)
        if not poss or max(poss) == 0:
            print("MISSING: stream position never advanced")
            return 1
        print("ok: output stream is running (LPIB advanced)")
    finally:
        proc.terminate()
        log.close()
    # analyze the captured WAV
    if not os.path.exists(WAV):
        print("MISSING: no WAV captured")
        return 1
    with open(WAV, "rb") as f:
        data = f.read()
    print("wav bytes:", len(data))
    if len(data) < 1000:
        print("MISSING: WAV too small (silence/no output)")
        return 1
    # find the data chunk and measure peak amplitude
    i = data.find(b"data")
    if i < 0:
        print("MISSING: no data chunk")
        return 1
    # QEMU を止めるとヘッダのサイズ欄が未確定のことがある
    # ので、実ファイルの残りを PCM とみなす。
    size = len(data) - (i + 8)
    pcm = data[i + 8:]
    peak = 0
    for off in range(0, len(pcm) - 1, 2):
        v = struct.unpack("<h", pcm[off:off + 2])[0]
        peak = max(peak, abs(v))
    print("wav data bytes:", size, "peak amplitude:", peak)
    if peak < 5000:
        print("MISSING: captured audio is silent")
        return 1
    print("ok: tone captured in WAV (peak %d)" % peak)
    print("HDA TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
