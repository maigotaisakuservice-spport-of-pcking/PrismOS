#!/usr/bin/env python3
"""PrismOS browser test.

Serves test pages from local servers on the host (the guest
reaches them at 10.0.2.2) and drives the desktop Browser:

  1. page1.html over HTTP: CSS colours from a <style> block
     (red heading checked by pixels), an inline BMP image
     (magenta block checked by pixels), a link to page2.
  2. Tab focuses the link (serial), Enter follows it.
  3. A chunked-transfer page loads.
  4. A 302 redirect is followed to page2.
  5. HTTPS with trust-on-first-use pinning: the first fetch
     stores the certificate fingerprint, the second uses it,
     and after the server certificate is swapped the fetch
     must fail.

Usage: python3 scripts/browser-test.py [--log PATH]
  (exit code 0 = passed; standard library only)
"""

import argparse
import os
import pty
import select
import socket
import ssl
import subprocess
import sys
import termios
import threading
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build", "prismos.iso")
DEFAULT_LOG = os.path.join(ROOT, "build", "browser.log")
MONITOR_SOCK = os.path.join(ROOT, "build", "qemu-monitor-browser.sock")
SHOT_DIR = os.path.join(ROOT, "build", "shots")
HTTP_PORT = 8377
HTTPS_PORT = 8378

QEMU = "qemu-system-x86_64"


class BrowserTestFailure(Exception):
    pass


def make_bmp():
    """64x48 24-bit BMP: magenta with a cyan square."""
    w, h = 64, 48
    row = w * 3
    pad = (4 - row % 4) % 4
    img = bytearray()
    for y in range(h):
        # BMP is bottom-up; colour choice is symmetric anyway.
        for x in range(w):
            if 16 <= x < 48 and 12 <= y < 36:
                img += bytes((255, 255, 0))  # BGR cyan
            else:
                img += bytes((255, 0, 255))  # BGR magenta
        img += b"\x00" * pad
    header = bytearray()
    header += b"BM"
    header += (54 + len(img)).to_bytes(4, "little")
    header += b"\x00\x00\x00\x00"
    header += (54).to_bytes(4, "little")
    header += (40).to_bytes(4, "little")
    header += w.to_bytes(4, "little", signed=True)
    header += h.to_bytes(4, "little", signed=True)
    header += (1).to_bytes(2, "little")
    header += (24).to_bytes(2, "little")
    header += (0).to_bytes(4, "little")
    header += len(img).to_bytes(4, "little")
    header += b"\x00" * 16
    return bytes(header) + bytes(img)


def make_png():
    """48x48 RGB PNG: orange with a blue square (filter 0)."""
    import struct
    import zlib

    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        c += struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        return c

    raw = b""
    for y in range(48):
        row = bytearray()
        for x in range(48):
            if 12 <= x < 36 and 12 <= y < 36:
                row += bytes((0, 0, 255))  # blue
            else:
                row += bytes((255, 128, 0))  # orange
        raw += b"\x00" + bytes(row)
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", 48, 48, 8, 2,
                                       0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(raw))
    out += chunk(b"IEND", b"")
    return out


PAGE_PNG = """<html><head><title>PNG Page</title></head>
<body><h1>PNG page</h1>
<img src="pic.png" alt="png picture">
<p>After the png.</p>
</body></html>
"""

PAGE1 = """<html><head><title>Test Page One</title>
<style>
body { color: #202020; }
h1 { color: #FF0000; }
a { color: #0000CC; }
</style></head>
<body>
<h1>Heading One</h1>
<p>Some body text with <font color="#00AA00">green words</font> inside.</p>
<p><a href="page2.html">Go to page two</a></p>
<img src="pic.bmp" alt="test picture">
<p>After the image.</p>
</body></html>
"""

PAGE2 = """<html><head><title>Second Page</title></head>
<body><h1>Second page</h1>
<p>You followed the link.</p>
</body></html>
"""

PAGE3 = """<html><head><title>Page Three</title>
<style>h2 {color: blue}</style></head>
<body>
<h2>Layout Test Page</h2>
<ul><li>First bullet</li><li>Second bullet</li></ul>
<ol><li>Numbered one</li><li>Numbered two</li></ol>
<table>
<tr><td>Alpha</td><td>Beta</td></tr>
<tr><td>Gamma</td><td>Delta</td></tr>
</table>
<form action="/page2.html">
<input type="text" name="q">
<input type="submit" value="Go">
</form>
</body></html>
"""

CHUNKED_BODY = ("<html><head><title>Chunked</title></head>"
                "<body><p>Chunked transfer works.</p>"
                "</body></html>")

SECURE_PAGE = ("<html><head><title>Secure Page</title></head>"
               "<body><p>Over TLS.</p></body></html>")


def http_response(body, ctype="text/html", extra=""):
    if isinstance(body, str):
        body = body.encode()
    head = ("HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
            "Content-Length: %d\r\nConnection: close\r\n%s\r\n"
            % (ctype, len(body), extra))
    return head.encode() + body


SEEN_COOKIE = {"value": None}


def handle_http(conn):
    try:
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = conn.recv(4096)
            if not chunk:
                return
            req += chunk
        line = req.split(b"\r\n", 1)[0].decode("latin1")
        parts = line.split(" ")
        path = parts[1] if len(parts) > 1 else "/"
        path = path.split("?")[0]
        if path == "/setcookie.html":
            conn.sendall(http_response(
                "<html><body><p>cookie set</p></body></html>",
                extra="Set-Cookie: session=xyz789; Path=/\r\n"))
        elif path == "/whoami.html":
            for hline in req.decode("latin1").split("\r\n"):
                if hline.lower().startswith("cookie:"):
                    SEEN_COOKIE["value"] = hline.split(":", 1)[1].strip()
            conn.sendall(http_response(
                "<html><body><p>whoami</p></body></html>"))
        elif path == "/js.html":
            conn.sendall(http_response(
                "<html><head><title>JS Page</title></head>"
                "<body><h1>JS page</h1>"
                "<script>\n"
                "var s = 0;\n"
                "for (var i = 1; i <= 4; i++) { s += i; }\n"
                "document.write('sum=' + s);\n"
                "</script>"
                "</body></html>"))
        elif path == "/jsbad.html":
            conn.sendall(http_response(
                "<html><body><h1>Bad JS</h1>"
                "<script>document.write(nope);</script>"
                "</body></html>"))
        elif path == "/file.bin":
            conn.sendall(http_response(bytes(range(256)) * 8,
                                       "application/octet-stream"))
        elif path == "/page1.html":
            conn.sendall(http_response(PAGE1))
        elif path == "/page2.html":
            conn.sendall(http_response(PAGE2))
        elif path == "/page3.html":
            conn.sendall(http_response(PAGE3))
        elif path == "/pic.bmp":
            conn.sendall(http_response(make_bmp(), "image/bmp"))
        elif path == "/pngpage.html":
            conn.sendall(http_response(PAGE_PNG))
        elif path == "/pic.png":
            conn.sendall(http_response(make_png(), "image/png"))
        elif path == "/chunked.html":
            body = CHUNKED_BODY.encode()
            head = ("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "Connection: close\r\n\r\n").encode()
            mid = len(body) // 2
            conn.sendall(head)
            conn.sendall(("%x\r\n" % mid).encode() + body[:mid] +
                         b"\r\n")
            conn.sendall(("%x\r\n" % (len(body) - mid)).encode() +
                         body[mid:] + b"\r\n0\r\n\r\n")
        elif path == "/redir":
            conn.sendall(("HTTP/1.1 302 Found\r\n"
                          "Location: /page2.html\r\n"
                          "Content-Length: 0\r\n"
                          "Connection: close\r\n\r\n").encode())
        else:
            conn.sendall(http_response("not found", "text/plain"))
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def serve_http():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", HTTP_PORT))
    srv.listen(8)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            return
        threading.Thread(target=handle_http, args=(conn,),
                         daemon=True).start()


PROXY_PORT = 8379
PROXY_SEEN = []


def handle_proxy(conn):
    try:
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = conn.recv(4096)
            if not chunk:
                return
            req += chunk
        line = req.split(b"\r\n", 1)[0].decode("latin1")
        PROXY_SEEN.append(line)
        parts = line.split(" ")
        uri = parts[1] if len(parts) > 1 else ""
        # uri is absolute: http://host[:port]/path
        rest = uri[7:] if uri.startswith("http://") else uri
        slash = rest.find("/")
        path = rest[slash:] if slash >= 0 else "/"
        origin = socket.create_connection(("127.0.0.1", HTTP_PORT),
                                          timeout=10)
        origin.sendall(("GET %s HTTP/1.0\r\nHost: 10.0.2.2\r\n"
                        "\r\n" % path).encode())
        while True:
            data = origin.recv(65536)
            if not data:
                break
            conn.sendall(data)
        origin.close()
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def serve_proxy():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PROXY_PORT))
    srv.listen(8)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            return
        threading.Thread(target=handle_proxy, args=(conn,),
                         daemon=True).start()


class HttpsServer:
    def __init__(self, cert, key):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(cert, key)
        try:
            self.ctx.set_ciphers("ALL:@SECLEVEL=0")
        except ssl.SSLError:
            pass
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR,
                             1)
        self.srv.bind(("0.0.0.0", HTTPS_PORT))
        self.srv.listen(8)
        self.stop_flag = False
        self.thread = threading.Thread(target=self._run,
                                       daemon=True)

    def _run(self):
        while not self.stop_flag:
            try:
                self.srv.settimeout(0.5)
                conn, _ = self.srv.accept()
            except (OSError, socket.timeout):
                continue
            threading.Thread(target=self._handle, args=(conn,),
                             daemon=True).start()

    def _handle(self, conn):
        try:
            tls = self.ctx.wrap_socket(conn, server_side=True)
            req = b""
            while b"\r\n\r\n" not in req:
                chunk = tls.recv(4096)
                if not chunk:
                    tls.close()
                    return
                req += chunk
            tls.sendall(http_response(SECURE_PAGE))
            tls.close()
        except (OSError, ssl.SSLError):
            try:
                conn.close()
            except OSError:
                pass

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_flag = True
        try:
            self.srv.close()
        except OSError:
            pass


def make_cert(name):
    cert = os.path.join(ROOT, "build", name + ".pem")
    key = os.path.join(ROOT, "build", name + "-key.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048",
         "-keyout", key, "-out", cert, "-days", "2", "-nodes",
         "-subj", "/CN=10.0.2.2"],
        check=True, capture_output=True)
    return cert, key


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6\n"):
        raise BrowserTestFailure("screenshot is not P6 PPM")
    rest = data[3:]
    parts = []
    i = 0
    while len(parts) < 3:
        while i < len(rest) and rest[i:i + 1].isspace():
            i += 1
        j = i
        while j < len(rest) and not rest[j:j + 1].isspace():
            j += 1
        parts.append(rest[i:j])
        i = j
    width, height = int(parts[0]), int(parts[1])
    i += 1
    return width, height, rest[i:i + width * height * 3]


def count_near(raw, width, height, target, tol=30):
    n = 0
    for i in range(0, width * height * 3, 3):
        if (abs(raw[i] - target[0]) <= tol and
                abs(raw[i + 1] - target[1]) <= tol and
                abs(raw[i + 2] - target[2]) <= tol):
            n += 1
    return n


def main():
    parser = argparse.ArgumentParser(description="PrismOS browser test")
    parser.add_argument("--log", default=DEFAULT_LOG)
    args = parser.parse_args()
    os.makedirs(SHOT_DIR, exist_ok=True)
    if not os.path.exists(ISO):
        print("MISSING: ISO not found: %s" % ISO)
        return 1

    threading.Thread(target=serve_http, daemon=True).start()
    threading.Thread(target=serve_proxy, daemon=True).start()
    cert1, key1 = make_cert("browser-cert1")
    cert2, key2 = make_cert("browser-cert2")
    https = HttpsServer(cert1, key1)
    https.start()

    qemu_args = [
        QEMU, "-cdrom", ISO, "-m", "256M",
        "-serial", "stdio",
        "-monitor", "unix:%s,server,nowait" % MONITOR_SOCK,
        "-display", "vnc=:9",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        "-no-reboot",
    ]
    try:
        os.unlink(MONITOR_SOCK)
    except OSError:
        pass
    log_file = open(args.log, "wb")
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    attrs = termios.tcgetattr(slave_fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    proc = subprocess.Popen(qemu_args, stdin=slave_fd,
                            stdout=slave_fd, stderr=subprocess.DEVNULL,
                            cwd=ROOT, close_fds=True)
    os.close(slave_fd)
    buf = bytearray()
    pos = 0
    monitor_sock = None

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
        needle_b = needle.encode()
        deadline = time.monotonic() + timeout
        while True:
            idx = buf.find(needle_b, pos)
            if idx >= 0:
                pos = idx + len(needle_b)
                return
            if proc.poll() is not None:
                raise BrowserTestFailure("QEMU exited early")
            if time.monotonic() > deadline:
                raise BrowserTestFailure(
                    "%s not seen in %s s" % (what, timeout))
            pump(min(0.5, deadline - time.monotonic()))

    def hmp(cmd):
        monitor_sock.sendall((cmd + "\n").encode())
        data = b""
        while b"(qemu)" not in data:
            chunk = monitor_sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def sendkey(key, delay=0.22):
        hmp("sendkey %s" % key)
        time.sleep(delay)

    def type_text(text):
        keymap = {".": "dot", "/": "slash", ":": "shift-semicolon",
                  "-": "minus"}
        for ch in text:
            if ch in keymap:
                sendkey(keymap[ch], 0.13)
            elif ch == " ":
                sendkey("spc", 0.13)
            elif ch.isupper():
                sendkey("shift-" + ch.lower(), 0.13)
            else:
                sendkey(ch, 0.13)

    def clear_and_type(text):
        # The address bar holds the previous URL; erase it.
        for _ in range(60):
            sendkey("backspace", 0.05)
        type_text(text)

    def screendump(name):
        path = os.path.join(SHOT_DIR, name)
        hmp("screendump %s" % path)
        time.sleep(0.5)
        return read_ppm(path)

    try:
        wait_for("desktop: ready", 120, "desktop ready")
        monitor_sock = socket.socket(socket.AF_UNIX,
                                     socket.SOCK_STREAM)
        monitor_sock.connect(MONITOR_SOCK)
        monitor_sock.recv(4096)

        sendkey("f4", 0.8)  # open Browser
        # 1. page1 over HTTP with CSS + image.
        clear_and_type("10.0.2.2:%d/page1.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/page1.html"
                 % HTTP_PORT, 60, "page1 fetch")
        time.sleep(1.5)  # let the redraw land before the shot
        w, h, raw = screendump("browser-page1.ppm")
        magenta = count_near(raw, w, h, (255, 0, 255))
        if magenta < 400:
            raise BrowserTestFailure(
                "BMP image not displayed (magenta px=%d)" % magenta)
        red = count_near(raw, w, h, (255, 0, 0))
        if red < 40:
            raise BrowserTestFailure(
                "CSS h1 colour not applied (red px=%d)" % red)
        green = count_near(raw, w, h, (0, 170, 0))
        if green < 20:
            raise BrowserTestFailure(
                "font colour not applied (green px=%d)" % green)

        # 1a2. ページ内検索・保存・ズーム (バッチ4 項目20)。
        sendkey("ctrl-f", 0.5)
        wait_for("browser: find opened", 20, "find opens")
        type_text("green")
        sendkey("ret", 0.5)
        wait_for("browser: found at line ", 20, "find green")
        print("ok: find-in-page located the word")
        sendkey("esc", 0.4)
        sendkey("ctrl-s", 0.6)
        wait_for("browser: page saved ", 20, "page save")
        print("ok: page saved to a file")
        sendkey("ctrl-equal", 0.6)
        wait_for("browser: zoom 2", 20, "zoom in")
        sendkey("ctrl-minus", 0.6)
        wait_for("browser: zoom 1", 20, "zoom out")
        print("ok: zoom in/out re-rendered the page")

        # 1b. A page with a PNG image: decoded and drawn.
        clear_and_type("10.0.2.2:%d/pngpage.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        # The image log precedes the "fetched" line (images
        # load during parsing), so wait for it first.
        wait_for("browser: image (png) http://10.0.2.2:%d/pic.png"
                 % HTTP_PORT, 60, "png image decode log")
        wait_for("browser: fetched http://10.0.2.2:%d/pngpage.html"
                 % HTTP_PORT, 60, "png page fetch")
        time.sleep(1.5)
        w, h, raw = screendump("browser-png.ppm")
        orange = count_near(raw, w, h, (255, 128, 0))
        if orange < 400:
            raise BrowserTestFailure(
                "PNG image not displayed (orange px=%d)" % orange)
        blue = count_near(raw, w, h, (0, 0, 255))
        if blue < 100:
            raise BrowserTestFailure(
                "PNG image centre missing (blue px=%d)" % blue)
        print("ok: PNG image displayed (orange=%d blue=%d)"
              % (orange, blue))

        # Back to page1 for the link step.
        clear_and_type("10.0.2.2:%d/page1.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/page1.html"
                 % HTTP_PORT, 60, "page1 refetch")

        # 1c. Cookies: Set-Cookie is stored, then sent back.
        clear_and_type("10.0.2.2:%d/setcookie.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: cookie session set for 10.0.2.2", 60,
                 "cookie stored log")
        clear_and_type("10.0.2.2:%d/whoami.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/whoami.html"
                 % HTTP_PORT, 60, "whoami fetch")
        time.sleep(0.5)
        if SEEN_COOKIE["value"] != "session=xyz789":
            raise BrowserTestFailure(
                "cookie not sent back (server saw %r)"
                % SEEN_COOKIE["value"])
        print("ok: cookie stored and sent back (%s)"
              % SEEN_COOKIE["value"])

        # 1d. JavaScript subset: document.write renders the
        #     computed text; a broken script logs an error.
        clear_and_type("10.0.2.2:%d/js.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: script ran", 60, "script ran log")
        wait_for("browser: fetched http://10.0.2.2:%d/js.html"
                 % HTTP_PORT, 60, "js page fetch")
        time.sleep(1.5)

        def dark_ink(name):
            w, h, raw = screendump(name)
            n = 0
            for yy in range(60, h - 110):
                for xx in range(24, w - 24):
                    i = (yy * w + xx) * 3
                    if (raw[i] < 90 and raw[i + 1] < 90 and
                            raw[i + 2] < 90):
                        n += 1
            return n

        dark_js = dark_ink("browser-js.ppm")
        clear_and_type("10.0.2.2:%d/jsbad.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: script error: undefined variable",
                 60, "script error log")
        print("ok: broken script reported as an error")
        time.sleep(1.5)
        dark_bad = dark_ink("browser-jsbad.ppm")
        # The good page has the extra rendered line "sum=10";
        # the bad page has only its heading. The ink counts
        # share the same window/wallpaper, so the difference
        # isolates the script output.
        if dark_js < dark_bad + 100:
            raise BrowserTestFailure(
                "script output text not rendered "
                "(dark js=%d bad=%d)" % (dark_js, dark_bad))
        print("ok: script ran and wrote text "
              "(dark js=%d bad=%d)" % (dark_js, dark_bad))

        # Back to page1 for the link step.
        clear_and_type("10.0.2.2:%d/page1.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/page1.html"
                 % HTTP_PORT, 60, "page1 refetch after js")

        # 2. Tab + Enter follows the link.
        sendkey("tab", 0.5)
        wait_for("browser: focus http://10.0.2.2:%d/page2.html"
                 % HTTP_PORT, 10, "link focus")
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/page2.html"
                 % HTTP_PORT, 60, "page2 fetch")

        # 3. Chunked transfer.
        clear_and_type("10.0.2.2:%d/chunked.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/chunked.html"
                 % HTTP_PORT, 60, "chunked fetch")

        # 4. Redirect is followed.
        clear_and_type("10.0.2.2:%d/redir" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: redirect -> http://10.0.2.2:%d/page2.html"
                 % HTTP_PORT, 60, "redirect log")
        wait_for("browser: fetched http://10.0.2.2:%d/page2.html"
                 % HTTP_PORT, 60, "redirect target fetch")

        # 5. HTTPS trust-on-first-use.
        clear_and_type("https://10.0.2.2:%d/s.html" % HTTPS_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: pin saved for 10.0.2.2", 60,
                 "pin saved on first visit")
        wait_for("browser: fetched https://10.0.2.2:%d/s.html"
                 % HTTPS_PORT, 60, "https fetch 1")
        sendkey("ret", 0.5)  # reload with the stored pin
        wait_for("browser: fetched https://10.0.2.2:%d/s.html"
                 % HTTPS_PORT, 60, "https fetch 2 (pinned)")
        # Swap the certificate: the pin must now refuse.
        https.stop()
        time.sleep(1.0)
        https2 = HttpsServer(cert2, key2)
        https2.start()
        sendkey("ret", 0.5)
        wait_for("browser: fetch failed", 60,
                 "pin mismatch refused")
        https2.stop()

        # 5b. Tabs, bookmarks and downloads.
        clear_and_type("10.0.2.2:%d/page1.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/page1.html"
                 % HTTP_PORT, 60, "page1 for tab test")
        sendkey("ctrl-d", 0.6)
        wait_for("browser: bookmark added "
                 "http://10.0.2.2:%d/page1.html" % HTTP_PORT, 20,
                 "bookmark added")
        print("ok: bookmark added with Ctrl+D")
        sendkey("ctrl-t", 0.6)
        wait_for("browser: new tab", 20, "new tab opens")
        type_text("10.0.2.2:%d/page2.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/page2.html"
                 % HTTP_PORT, 60, "page2 in second tab")
        print("ok: second tab loaded page2")
        sendkey("ctrl-w", 0.6)
        wait_for("browser: tab closed", 20, "tab closed")
        time.sleep(1.5)  # tab restore re-parses (images refetch)
        w, h, raw = screendump("browser-tab-back.ppm")
        red = count_near(raw, w, h, (255, 0, 0))
        if red < 40:
            raise BrowserTestFailure(
                "closing the tab did not restore page1 "
                "(red px=%d)" % red)
        print("ok: closing the tab restored page1 (red=%d)" % red)
        clear_and_type("10.0.2.2:%d/file.bin" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: downloaded file.bin", 60,
                 "download saved")
        print("ok: .bin link target downloaded to disk")

        # 5c. Layout engine: lists, a table and a GET form.
        clear_and_type("10.0.2.2:%d/page3.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/page3.html"
                 % HTTP_PORT, 60, "page3 loads")
        time.sleep(1.0)
        w, h, raw = screendump("browser-page3.ppm")
        blue = count_near(raw, w, h, (0, 0, 255))
        if blue < 40:
            raise BrowserTestFailure(
                "page3 h2 not rendered in blue (blue px=%d)" % blue)
        print("ok: page3 lists/table/form rendered (blue=%d)"
              % blue)
        # Tab reaches the form's text input first (it is the
        # first focusable element on this page).
        sendkey("tab", 0.5)
        wait_for("browser: input focus q", 10, "input focused")
        type_text("hello")
        sendkey("ret", 0.5)
        wait_for("browser: form submit "
                 "http://10.0.2.2:%d/page2.html?q=hello"
                 % HTTP_PORT, 20, "form submitted")
        wait_for("browser: fetched "
                 "http://10.0.2.2:%d/page2.html?q=hello"
                 % HTTP_PORT, 60, "submit target fetched")
        print("ok: GET form submitted with the typed value")

        # 6. Plain HTTP via the proxy: the guest is told
        #    to use 10.0.2.2:8379 as its proxy; the page
        #    must still load, and the proxy must have
        #    seen an absolute-URI request.
        sendkey("f1", 0.8)  # Terminal
        os.write(master_fd,
                 ("proxy set 10.0.2.2 %d\r" % PROXY_PORT).encode())
        wait_for("proxy: set (plain HTTP only).", 20,
                 "proxy configured")
        sendkey("f4", 0.8)  # Browser
        clear_and_type("10.0.2.2:%d/page1.html" % HTTP_PORT)
        sendkey("ret", 0.5)
        wait_for("browser: fetched http://10.0.2.2:%d/page1.html"
                 % HTTP_PORT, 60, "proxied page1 fetch")
        if not any("page1.html" in line and
                   line.startswith("GET http://")
                   for line in PROXY_SEEN):
            raise BrowserTestFailure(
                "proxy saw no absolute-URI request: %r"
                % (PROXY_SEEN,))
        print("ok: browser loads pages through the HTTP proxy")
        sendkey("f1", 0.8)
        os.write(master_fd, b"proxy off\r")
        wait_for("proxy: off.", 20, "proxy cleared")

        print("BROWSER TEST PASSED")
        return 0
    except BrowserTestFailure as exc:
        print("BROWSER TEST FAILED: %s" % exc)
        return 1
    finally:
        try:
            if monitor_sock is not None:
                monitor_sock.close()
        except OSError:
            pass
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        https.stop()
        log_file.close()


if __name__ == "__main__":
    sys.exit(main())
