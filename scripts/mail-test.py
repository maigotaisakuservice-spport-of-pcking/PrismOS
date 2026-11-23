#!/usr/bin/env python3
"""PrismOS mail test.

Boots the ISO in QEMU with a blank 16MB ATA disk and a real
SMTP + POP3 server on the host (10.0.2.2), and checks mail
end to end:
  1. `mail setup` configures the servers;
  2. `mail send` delivers a message to the SMTP server
     (the server stores it; the test reads it back);
  3. `mail recv` fetches the seeded message from POP3;
  4. `mail` lists it, `mail read 1` shows its body;
  5. after a reboot with the same disk, the message is
     still there (mailbox files persist like other files);
  6. `mail del 1` deletes it.

The servers are real protocol peers (SMTP EHLO/MAIL/RCPT/
DATA/QUIT, POP3 USER/PASS/STAT/RETR/DELE/QUIT), not stubs:
the guest's TCP stack talks to them over the wire.

Boot 3 repeats the exercise over implicit TLS (the
guest's own TLS 1.2 client, certificate pinned by
SHA-256 fingerprint) and over IMAP (plain and TLS):
sending via SMTPS, receiving via POP3S, then via IMAP,
a wrong-pin attempt that must be refused, and a plain
IMAP receive after `mail tls off`.

Usage: python3 scripts/mail-test.py
  (exit code 0 = passed; TLS certificate generation
  uses the `cryptography` package)
"""

import hashlib
import importlib.util
import os
import socket
import ssl
import sys
import threading
import time

_spec = importlib.util.spec_from_file_location(
    "recovery_test",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "recovery-test.py"))
rt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rt)

SMTP_PORT = 2525
POP3_PORT = 1110
SMTPS_PORT = 2465
POP3S_PORT = 2995
IMAPS_PORT = 2993
IMAP_PORT = 2143
MAIL_DIR = "/tmp/prism-mail"

TLS_SEED_MSG = ("From: secure@prismos.local\r\n"
                "To: prism@prismos.local\r\n"
                "Subject: SecurePOP3\r\n"
                "Date: \r\n"
                "\r\n"
                "Hello over TLS.\r\n")

IMAP_MSG = ("From: imap@prismos.local\r\n"
            "To: prism@prismos.local\r\n"
            "Subject: ViaIMAP\r\n"
            "Date: \r\n"
            "\r\n"
            "IMAP body line.\r\n")

SEED_MSG = ("From: server@prismos.local\r\n"
            "To: prism@prismos.local\r\n"
            "Subject: =?UTF-8?Q?Welcome_MIME?=\r\n"
            "Date: Sat, 22 Nov 2026 09:00:00 -0000\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/mixed; boundary=BB\r\n"
            "\r\n"
            "--BB\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "Hello from the mail server.\r\n"
            "--BB\r\n"
            "Content-Type: text/plain; name=\"hello.txt\"\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "Content-Disposition: attachment; "
            "filename=\"hello.txt\"\r\n"
            "\r\n"
            "QXR0YWNobWVudCBib2R5IDEyMw==\r\n"
            "--BB--\r\n")


class MailTestFailure(Exception):
    pass


class SmtpServer(threading.Thread):
    """A small real SMTP server. Stores each accepted
    message in MAIL_DIR/smtp-received.txt."""

    def __init__(self, port=SMTP_PORT, ssl_ctx=None,
                 received_name="smtp-received.txt"):
        super().__init__(daemon=True)
        self.port = port
        self.ssl_ctx = ssl_ctx
        self.received_path = os.path.join(MAIL_DIR, received_name)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(5)
        self.sock.settimeout(1.0)
        self.running = True

    def run(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                if self.ssl_ctx is not None:
                    conn = self.ssl_ctx.wrap_socket(
                        conn, server_side=True)
                self.handle(conn)
            except (OSError, ssl.SSLError):
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def handle(self, conn):
        conn.settimeout(10.0)
        f = conn.makefile("rwb", buffering=0)
        f.write(b"220 PrismOS test mail ready\r\n")
        data_lines = []
        in_data = False
        while True:
            raw = f.readline()
            if not raw:
                return
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            upper = line.upper()
            if in_data:
                if line == ".":
                    in_data = False
                    msg = "\n".join(data_lines) + "\n"
                    with open(self.received_path, "w") as out:
                        out.write(msg)
                    data_lines = []
                    f.write(b"250 OK queued\r\n")
                else:
                    data_lines.append(line)
                continue
            if upper.startswith("EHLO") or upper.startswith("HELO"):
                f.write(b"250-localhost\r\n250 HELP\r\n")
            elif upper.startswith("MAIL FROM:"):
                f.write(b"250 OK\r\n")
            elif upper.startswith("RCPT TO:"):
                f.write(b"250 OK\r\n")
            elif upper == "DATA":
                in_data = True
                f.write(b"354 End data with <CR><LF>.<CR><LF>\r\n")
            elif upper == "QUIT":
                f.write(b"221 Bye\r\n")
                return
            elif upper == "RSET":
                data_lines = []
                f.write(b"250 OK\r\n")
            elif upper == "NOOP":
                f.write(b"250 OK\r\n")
            else:
                f.write(b"500 Command not recognized\r\n")

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass


class Pop3Server(threading.Thread):
    """A small real POP3 server, seeded with one message.
    DELE marks messages deleted; later STAT reflects it."""

    def __init__(self, port=POP3_PORT, ssl_ctx=None, seed=SEED_MSG):
        super().__init__(daemon=True)
        self.port = port
        self.ssl_ctx = ssl_ctx
        self.messages = [seed]
        self.deleted = set()
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(5)
        self.sock.settimeout(1.0)
        self.running = True

    def run(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                if self.ssl_ctx is not None:
                    conn = self.ssl_ctx.wrap_socket(
                        conn, server_side=True)
                self.handle(conn)
            except (OSError, ssl.SSLError):
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def live(self):
        with self.lock:
            return [i for i in range(len(self.messages))
                    if i not in self.deleted]

    def handle(self, conn):
        conn.settimeout(10.0)
        f = conn.makefile("rwb", buffering=0)
        f.write(b"+OK PrismOS test mail ready\r\n")
        authed = False
        while True:
            raw = f.readline()
            if not raw:
                return
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            upper = line.upper()
            if upper.startswith("USER "):
                f.write(b"+OK\r\n")
            elif upper.startswith("PASS "):
                authed = True
                f.write(b"+OK logged in\r\n")
            elif not authed:
                f.write(b"-ERR not logged in\r\n")
            elif upper == "STAT":
                live = self.live()
                total = sum(len(self.messages[i]) for i in live)
                f.write(("+OK %d %d\r\n" % (len(live), total)).encode())
            elif upper == "LIST":
                live = self.live()
                f.write(("+OK %d messages\r\n" % len(live)).encode())
                for i in live:
                    f.write(("%d %d\r\n" % (i + 1,
                                            len(self.messages[i]))).encode())
                f.write(b".\r\n")
            elif upper.startswith("RETR "):
                try:
                    n = int(line.split(" ", 1)[1])
                except (ValueError, IndexError):
                    f.write(b"-ERR bad number\r\n")
                    continue
                live = self.live()
                if n < 1 or n > len(self.messages) or \
                        (n - 1) not in live:
                    f.write(b"-ERR no such message\r\n")
                    continue
                msg = self.messages[n - 1]
                f.write(("+OK %d octets\r\n" % len(msg)).encode())
                for mline in msg.split("\r\n"):
                    if mline.startswith("."):
                        mline = "." + mline
                    f.write((mline + "\r\n").encode())
                f.write(b".\r\n")
            elif upper.startswith("DELE "):
                try:
                    n = int(line.split(" ", 1)[1])
                except (ValueError, IndexError):
                    f.write(b"-ERR bad number\r\n")
                    continue
                with self.lock:
                    if 1 <= n <= len(self.messages):
                        self.deleted.add(n - 1)
                f.write(b"+OK deleted\r\n")
            elif upper == "QUIT":
                f.write(b"+OK bye\r\n")
                return
            elif upper == "NOOP":
                f.write(b"+OK\r\n")
            else:
                f.write(b"-ERR unknown command\r\n")

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass


class ImapServer(threading.Thread):
    """A minimal but real IMAP4rev1 peer: LOGIN, SELECT
    INBOX (one message), FETCH BODY.PEEK[] with a literal,
    LOGOUT. The message is never deleted (PEEK semantics),
    so repeated receives keep working."""

    def __init__(self, port, ssl_ctx=None):
        super().__init__(daemon=True)
        self.port = port
        self.ssl_ctx = ssl_ctx
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(5)
        self.sock.settimeout(1.0)
        self.running = True

    def run(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                if self.ssl_ctx is not None:
                    conn = self.ssl_ctx.wrap_socket(
                        conn, server_side=True)
                self.handle(conn)
            except (OSError, ssl.SSLError):
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def handle(self, conn):
        conn.settimeout(15.0)
        f = conn.makefile("rwb", buffering=0)
        f.write(b"* OK PrismOS test IMAP ready\r\n")
        while True:
            raw = f.readline()
            if not raw:
                return
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            parts = line.split(" ", 2)
            if len(parts) < 2:
                continue
            tag = parts[0]
            cmd = parts[1].upper()
            if cmd == "LOGIN":
                f.write((tag + " OK LOGIN completed\r\n").encode())
            elif cmd == "SELECT":
                f.write(b"* 1 EXISTS\r\n")
                f.write(b"* OK [UIDVALIDITY 1] ok\r\n")
                f.write((tag + " OK [READ-ONLY] SELECT "
                         "completed\r\n").encode())
            elif cmd == "FETCH":
                body = IMAP_MSG.encode()
                f.write(("* 1 FETCH (BODY[] {%d}\r\n"
                         % len(body)).encode())
                f.write(body)
                f.write(b")\r\n")
                f.write((tag + " OK FETCH completed\r\n").encode())
            elif cmd == "LOGOUT":
                f.write(b"* BYE bye\r\n")
                f.write((tag + " OK LOGOUT completed\r\n").encode())
                return
            else:
                f.write((tag + " BAD unknown command\r\n").encode())

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass


def make_tls_context():
    """Self-signed RSA-2048 certificate for the TLS test
    servers; returns (ssl_context, sha256_fingerprint_hex)
    of the certificate DER (what the guest pins)."""
    import datetime
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    key = rsa.generate_private_key(public_exponent=65537,
                                   key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,
                                         "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder()
            .subject_name(name)
            .issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=30))
            .sign(key, hashes.SHA256()))
    der = cert.public_bytes(serialization.Encoding.DER)
    fp = hashlib.sha256(der).hexdigest()
    cert_path = os.path.join(MAIL_DIR, "cert.pem")
    key_path = os.path.join(MAIL_DIR, "key.pem")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption()))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert_path, key_path)
    # The guest offers exactly one suite; make sure the
    # server side can pick it.
    ctx.set_ciphers("AES128-GCM-SHA256")
    return ctx, fp


def main():
    if not os.path.exists(rt.ISO):
        print("MISSING: ISO not found: %s" % rt.ISO)
        return 1
    os.makedirs(MAIL_DIR, exist_ok=True)
    received = os.path.join(MAIL_DIR, "smtp-received.txt")
    try:
        os.unlink(received)
    except OSError:
        pass
    rt.make_disk()
    print("disk: created 16MB ATA disk")
    smtp = SmtpServer()
    pop3 = Pop3Server()
    smtp.start()
    pop3.start()
    # The servers bind 127.0.0.1; QEMU user networking
    # reaches the host as 10.0.2.2.
    time.sleep(0.5)
    try:
        # Boot 1: set up mail, send one, receive one.
        b = rt.Boot(rt.LOG)
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            print("ok: boot banner")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            print("ok: installed to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("mail setup 10.0.2.2 2525 10.0.2.2 1110 "
                   "prism secret prism@prismos.local")
            b.wait_for("mail: server configured.", 20,
                       "mail setup")
            print("ok: mail server configured")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("mail send friend@example.com Hello "
                   "Test-body-from-PrismOS")
            b.wait_for("mail: sent to friend@example.com", 30,
                       "mail sent")
            print("ok: mail sent over SMTP")
            b.wait_for("prismos> ", 20, "shell prompt")
            # The SMTP server must have the message, headers
            # and body included.
            deadline = time.monotonic() + 10
            stored = ""
            while time.monotonic() < deadline:
                try:
                    with open(received) as f:
                        stored = f.read()
                    if "Test-body-from-PrismOS" in stored:
                        break
                except OSError:
                    pass
                time.sleep(0.5)
            if "To: friend@example.com" not in stored:
                raise MailTestFailure(
                    "SMTP server did not get the To header")
            if "Subject: Hello" not in stored:
                raise MailTestFailure(
                    "SMTP server did not get the Subject header")
            if "Test-body-from-PrismOS" not in stored:
                raise MailTestFailure(
                    "SMTP server did not get the body")
            if "MIME-Version: 1.0" not in stored:
                raise MailTestFailure(
                    "SMTP message has no MIME-Version header")
            date_lines = [ln for ln in stored.splitlines()
                          if ln.startswith("Date: ")]
            import re as _re
            if not date_lines or not _re.match(
                    r"Date: \w{3}, \d{2} \w{3} \d{4} "
                    r"\d{2}:\d{2}:\d{2} [+-]\d{4}",
                    date_lines[0]):
                raise MailTestFailure(
                    "SMTP message Date header not filled: %r"
                    % (date_lines,))
            print("ok: SMTP server received the message "
                  "(MIME + Date: %s)" % date_lines[0])
            b.send("mail")
            b.wait_for("[Sent]", 20, "sent copy in list")
            print("ok: sent message archived in Sent")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("mail recv")
            b.wait_for("mail: attachment saved hello.txt", 30,
                       "attachment extracted on receive")
            b.wait_for("mail: received 1 message(s).", 30,
                       "mail received")
            print("ok: mail received over POP3, attachment "
                  "saved")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("mail")
            b.wait_for("server@prismos.local", 20, "mail list")
            b.wait_for("Welcome MIME", 20,
                       "decoded subject in list")
            print("ok: mail listed (encoded-word decoded)")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("mail read 1")
            b.wait_for("Hello from the mail server.", 20,
                       "mail body")
            print("ok: mail read (MIME text body decoded)")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat hello.txt")
            b.wait_for("Attachment body 123", 20,
                       "attachment content")
            print("ok: attachment content readable")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("mail move 1 Work")
            b.wait_for("moved to Work", 20, "mail move")
            b.send("mail")
            b.wait_for("[Work]", 20, "folder tag in list")
            print("ok: mail folders work")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()

        # Boot 2: same disk, the message must still be there.
        b = rt.Boot(rt.LOG)
        try:
            b.wait_for("[boot] disk: installed system found", 30,
                       "installed system found")
            print("ok: installed system found on reboot")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("mail")
            b.wait_for("server@prismos.local", 20,
                       "mail list after reboot")
            b.wait_for("Welcome", 20, "mail subject after reboot")
            print("ok: mail survived reboot")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("mail del 1")
            b.wait_for("mail: deleted message 1.", 20, "mail delete")
            print("ok: mail deleted")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("mail read 1")
            b.wait_for("mail: no such message.", 20,
                       "deleted message is gone")
            print("ok: mailbox entry gone after delete "
                  "(the Sent copy remains by design)")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()
        # ---- Boot 3: TLS (SMTPS/POP3S/IMAPS) + IMAP ----
        ctx, pin = make_tls_context()
        print("tls: test certificate sha256 pin %s..." % pin[:16])
        smtps = SmtpServer(port=SMTPS_PORT, ssl_ctx=ctx,
                           received_name="smtps-received.txt")
        pop3s = Pop3Server(port=POP3S_PORT, ssl_ctx=ctx,
                           seed=TLS_SEED_MSG)
        imaps = ImapServer(IMAPS_PORT, ssl_ctx=ctx)
        imap = ImapServer(IMAP_PORT)
        for srv in (smtps, pop3s, imaps, imap):
            srv.start()
        time.sleep(0.5)
        try:
            os.unlink(os.path.join(MAIL_DIR, "smtps-received.txt"))
        except OSError:
            pass
        rt.make_disk()
        print("disk: fresh disk for the TLS/IMAP boot")
        try:
            b = rt.Boot(rt.LOG)
            try:
                b.wait_for("prismos> ", 60, "shell prompt (tls boot)")
                b.send("install yes")
                b.wait_for("Installed to disk", 30, "install done")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail setup 10.0.2.2 %d 10.0.2.2 %d "
                       "prism secret prism@prismos.local"
                       % (SMTPS_PORT, POP3S_PORT))
                b.wait_for("mail: server configured.", 20,
                           "mail setup (tls)")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail tls on")
                b.wait_for("mail: TLS on for smtp/pop3/imap.", 20,
                           "mail tls on")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail pin " + pin)
                b.wait_for("mail: certificate pin set.", 20,
                           "mail pin set")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail send friend@example.com Secure "
                       "Secret-body-over-TLS")
                b.wait_for("mail: TLS session established.", 60,
                           "TLS session for SMTP")
                b.wait_for("mail: sent to friend@example.com", 60,
                           "mail sent over TLS")
                print("ok: mail sent over SMTPS (TLS 1.2, pinned)")
                b.wait_for("prismos> ", 20, "shell prompt")
                deadline = time.monotonic() + 10
                stored = ""
                path = os.path.join(MAIL_DIR, "smtps-received.txt")
                while time.monotonic() < deadline:
                    try:
                        with open(path) as fh:
                            stored = fh.read()
                        if "Secret-body-over-TLS" in stored:
                            break
                    except OSError:
                        pass
                    time.sleep(0.5)
                if "Secret-body-over-TLS" not in stored:
                    raise MailTestFailure(
                        "SMTPS server did not get the body")
                print("ok: SMTPS server received the message")
                b.send("mail recv")
                b.wait_for("mail: received 1 message(s).", 60,
                           "mail received over POP3S")
                print("ok: mail received over POP3S")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail imap 10.0.2.2 %d" % IMAPS_PORT)
                b.wait_for("mail: IMAP server set.", 20, "mail imap")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail recv")
                b.wait_for("mail: received 1 message(s).", 60,
                           "mail received over IMAPS")
                print("ok: mail received over IMAPS")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail")
                b.wait_for("ViaIMAP", 20, "IMAP subject in list")
                print("ok: IMAP message listed")
                b.wait_for("prismos> ", 20, "shell prompt")
                # A wrong pin must be refused before any
                # application data flows.
                b.send("mail pin " + "0" * 64)
                b.wait_for("mail: certificate pin set.", 20,
                           "wrong pin set")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail recv")
                b.wait_for("certificate does not match the pin",
                           60, "pin mismatch refused")
                b.wait_for("mail: receive failed.", 20,
                           "receive failed on wrong pin")
                print("ok: wrong certificate pin refused")
                b.wait_for("prismos> ", 20, "shell prompt")
                # Plain IMAP after tls off (message is still
                # on the server: IMAP receive deletes nothing).
                b.send("mail pin " + pin)
                b.wait_for("mail: certificate pin set.", 20,
                           "pin restored")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail tls off")
                b.wait_for("mail: TLS off.", 20, "mail tls off")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail imap 10.0.2.2 %d" % IMAP_PORT)
                b.wait_for("mail: IMAP server set.", 20,
                           "plain imap set")
                b.wait_for("prismos> ", 20, "shell prompt")
                b.send("mail recv")
                b.wait_for("mail: received 1 message(s).", 60,
                           "mail received over plain IMAP")
                print("ok: mail received over plain IMAP")
                b.wait_for("prismos> ", 20, "shell prompt")
            finally:
                b.stop()
        finally:
            for srv in (smtps, pop3s, imaps, imap):
                srv.stop()
        print("MAIL TEST PASSED")
        return 0
    except rt.RecoveryTestFailure as e:
        print("MISSING: %s" % e)
        return 1
    except MailTestFailure as e:
        print("MISSING: %s" % e)
        return 1
    finally:
        smtp.stop()
        pop3.stop()


if __name__ == "__main__":
    sys.exit(main())
