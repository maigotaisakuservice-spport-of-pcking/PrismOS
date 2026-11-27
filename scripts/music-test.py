#!/usr/bin/env python3
"""PrismOS music test: real WAV playback through the SB16 device.

Boots the ISO in QEMU with an SB16 audio device wired to a WAV
capture backend, runs `play melody.wav` in the shell, and analyses
the captured audio: it must be non-silent, about as long as the
melody (0.4 s), and its loudest part must be close to the first
note (C5 = 523.25 Hz). The capture backend only records while
audio is produced, so leading silence is not in the file.

Usage: python3 scripts/music-test.py
  (exit code 0 = passed; needs numpy for the frequency check)
"""

import math
import os
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import time
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
CAP = os.path.join(ROOT, "build", "capture.wav")
LOG = os.path.join(ROOT, "build", "music-boot.log")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-music.sock")

QEMU = "qemu-system-x86_64"


class MusicTestFailure(Exception):
    pass


def main():
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1
    try:
        import numpy
    except ImportError:
        print("MISSING: numpy is required for the frequency check")
        return 1
    for path in (CAP, LOG, MONITOR_SOCK):
        try:
            os.unlink(path)
        except OSError:
            pass

    qemu_args = [
        QEMU, "-cdrom", ISO, "-m", "256M",
        "-serial", "file:%s" % LOG,
        "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
        "-display", "none",
        "-no-reboot",
        "-device", "sb16,audiodev=snd0",
        "-audiodev", "wav,id=snd0,path=%s" % CAP,
    ]
    proc = subprocess.Popen(qemu_args, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, close_fds=True)
    failures = []
    try:
        def log_bytes():
            try:
                with open(LOG, "rb") as f:
                    return f.read()
            except OSError:
                return b""

        def wait_for(needle, timeout, what):
            nb = needle.encode()
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if nb in log_bytes():
                    return
                if proc.poll() is not None:
                    raise MusicTestFailure(
                        "QEMU exited early (code %s)" % proc.returncode)
                time.sleep(0.3)
            raise MusicTestFailure("%s not seen in %s s"
                                   % (what, timeout))

        wait_for("prismos> ", 30, "shell prompt")
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                monitor.connect(MONITOR_SOCK)
                break
            except OSError:
                time.sleep(0.1)

        def sendkey(key, delay=0.12):
            monitor.sendall(("sendkey %s\n" % key).encode("ascii"))
            time.sleep(delay)

        def type_text(text):
            for ch in text:
                if ch == " ":
                    sendkey("spc")
                elif ch == ".":
                    sendkey("dot")
                else:
                    sendkey(ch)

        type_text("play melody.wav")
        sendkey("ret", 0.3)
        wait_for("play: done", 20, "play done")
        print("ok: play melody.wav finished")
        time.sleep(1.0)
        monitor.close()
    except MusicTestFailure as e:
        print("MISSING: %s" % e)
        failures.append(str(e))
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
    if failures:
        print("MUSIC TEST FAILED")
        return 1

    # Analyse the capture.
    if not os.path.exists(CAP) or os.path.getsize(CAP) < 1000:
        print("MISSING: no audio captured")
        print("MUSIC TEST FAILED")
        return 1
    w = wave.open(CAP, "rb")
    ch, fr, n = w.getnchannels(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    vals = struct.unpack("<%dh" % (n * ch), raw)
    mono = [(vals[i] + vals[i + 1]) // 2 if ch == 2 else vals[i]
            for i in range(0, len(vals), ch)]
    duration = n / fr
    rms = math.sqrt(sum(v * v for v in mono) / max(1, len(mono)))
    peak = max(abs(v) for v in mono)
    print("capture: %.2f s, rms %.0f, peak %d" % (duration, rms, peak))
    if duration < 0.25 or duration > 0.6:
        print("MISSING: capture length %.2f s, expected about 0.4 s"
              % duration)
        print("MUSIC TEST FAILED")
        return 1
    if rms < 1000 or peak < 5000:
        print("MISSING: capture is too quiet (rms %.0f, peak %d)"
              % (rms, peak))
        print("MUSIC TEST FAILED")
        return 1
    # Dominant frequency of the loudest 100 ms window.
    win = fr // 10
    best_rms, best_i = 0.0, 0
    for i in range(0, max(1, len(mono) - win), win // 2):
        seg = mono[i:i + win]
        r = math.sqrt(sum(v * v for v in seg) / len(seg))
        if r > best_rms:
            best_rms, best_i = r, i
    seg = mono[best_i:best_i + win]
    seg = [float(v) - sum(seg) / len(seg) for v in seg]
    spectrum = abs(numpy.fft.rfft(seg))
    freqs = numpy.fft.rfftfreq(len(seg), 1.0 / fr)
    # Ignore the DC bin and anything below 100 Hz.
    spectrum[0] = 0
    for i, f in enumerate(freqs):
        if f < 100:
            spectrum[i] = 0
    peak_freq = float(freqs[int(numpy.argmax(spectrum))])
    print("loudest window at %.2f s, dominant frequency %.1f Hz"
          % (best_i / fr, peak_freq))
    if abs(peak_freq - 523.25) > 60:
        print("MISSING: dominant frequency %.1f Hz, expected about "
              "523 Hz (C5)" % peak_freq)
        print("MUSIC TEST FAILED")
        return 1
    print("MUSIC TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
