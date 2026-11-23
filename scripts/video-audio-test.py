#!/usr/bin/env python3
"""PrismOS video audio test: video playback with sound, really.

Boots the ISO in QEMU with an SB16 audio device wired to a WAV
capture backend, opens the Video app (Ctrl+V), plays the 12
vid-*.pv1 frames (320x240, each frame carries a 2000-sample
8000 Hz audio track: a C5 = 523.25 Hz sine), and analyses the
captured audio: it must be non-silent, about as long as the
playback (about 4 s of wall time), and its loudest part must be
close to 523 Hz. The serial log must also show one
"video: audio frame N" line per played frame, so the sound is
tied to the frames and not a single beep.

Usage: python3 scripts/video-audio-test.py
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
import termios
import time
import tty
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
CAP = os.path.join(ROOT, "build", "video-capture.wav")
LOG = os.path.join(ROOT, "build", "video-audio-boot.log")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-video-audio.sock")

QEMU = "qemu-system-x86_64"
PLAY_SECONDS = 4.0


class VideoAudioTestFailure(Exception):
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
        "-serial", "stdio",
        "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
        "-display", "vnc=:8",
        "-device", "sb16,audiodev=snd0",
        "-audiodev", "wav,id=snd0,path=%s" % CAP,
        "-no-reboot",
        "-d", "cpu_reset,int",
        "-D", os.path.join(ROOT, "build", "qemu-debug-video-audio.log"),
    ]

    log_file = open(LOG, "wb")
    stderr_file = open(os.path.join(ROOT, "build",
                                    "qemu-stderr-video-audio.log"), "wb")
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    proc = subprocess.Popen(qemu_args, stdin=slave_fd, stdout=slave_fd,
                            stderr=stderr_file, cwd=ROOT, close_fds=True)
    os.close(slave_fd)
    buf = bytearray()
    pos = 0
    monitor_sock = None
    failures = []

    def pump(timeout):
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
                return
            if not chunk:
                return
            buf.extend(chunk)
            log_file.write(chunk)
            log_file.flush()
            return

    def wait_for(needle, timeout, what):
        nonlocal pos
        needle_b = needle.encode("utf-8", "replace")
        deadline = time.monotonic() + timeout
        while True:
            idx = buf.find(needle_b, pos)
            if idx >= 0:
                pos = idx + len(needle_b)
                return
            if proc.poll() is not None:
                raise VideoAudioTestFailure(
                    "QEMU exited early (code %s)" % proc.returncode)
            if time.monotonic() > deadline:
                raise VideoAudioTestFailure(
                    "%s not seen in %s s" % (what, timeout))
            pump(min(0.5, deadline - time.monotonic()))

    def hmp(cmd):
        monitor_sock.sendall((cmd + "\n").encode("ascii"))
        data = b""
        while b"(qemu)" not in data:
            chunk = monitor_sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def sendkey(key, delay=0.25):
        hmp("sendkey %s" % key)
        time.sleep(delay)

    try:
        wait_for("PrismOS 27H1", 30, "boot banner")
        print("ok: boot banner")
        wait_for("desktop: ready", 30, "desktop ready")
        print("ok: desktop ready")
        wait_for("prismos> ", 30, "shell prompt")
        print("ok: shell prompt")
        time.sleep(1.0)

        monitor_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor_sock.settimeout(5)
        monitor_sock.connect(MONITOR_SOCK)
        hmp("")

        sendkey("ctrl-v")
        wait_for("video: 24 frames", 20, "video frame list")
        print("ok: Video found 24 frames")
        time.sleep(0.8)
        sendkey("spc")
        wait_for("video: play", 20, "video play")
        print("ok: video playing")
        time.sleep(PLAY_SECONDS)
        sendkey("spc")
        wait_for("video: pause", 20, "video pause")
        print("ok: video paused after %.1f s of playback"
              % PLAY_SECONDS)
        time.sleep(1.0)
        monitor_sock.close()
        monitor_sock = None
    except VideoAudioTestFailure as e:
        print("MISSING: %s" % e)
        failures.append(str(e))
    except OSError as e:
        print("MISSING: QEMU communication error: %s" % e)
        failures.append(str(e))
    finally:
        if monitor_sock is not None:
            try:
                monitor_sock.close()
            except OSError:
                pass
            try:
                os.unlink(MONITOR_SOCK)
            except OSError:
                pass
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
        print("VIDEO AUDIO TEST FAILED")
        return 1

    # The serial log must show one audio line per played frame.
    with open(LOG, "rb") as f:
        log_text = f.read().decode("utf-8", "replace")
    audio_lines = [ln for ln in log_text.splitlines()
                   if ln.startswith("video: audio frame ")]
    print("serial: %d video audio frame lines" % len(audio_lines))
    if len(audio_lines) < 10:
        print("MISSING: only %d audio frames played, expected at "
              "least 10 in %.1f s" % (len(audio_lines), PLAY_SECONDS))
        print("VIDEO AUDIO TEST FAILED")
        return 1

    # Analyse the capture.
    if not os.path.exists(CAP) or os.path.getsize(CAP) < 1000:
        print("MISSING: no audio captured")
        print("VIDEO AUDIO TEST FAILED")
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
    if duration < 2.5 or duration > 5.0:
        print("MISSING: capture length %.2f s, expected about %.1f s "
              "of playback" % (duration, PLAY_SECONDS))
        print("VIDEO AUDIO TEST FAILED")
        return 1
    if rms < 1000 or peak < 5000:
        print("MISSING: capture is too quiet (rms %.0f, peak %d)"
              % (rms, peak))
        print("VIDEO AUDIO TEST FAILED")
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
        print("VIDEO AUDIO TEST FAILED")
        return 1
    print("VIDEO AUDIO TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
