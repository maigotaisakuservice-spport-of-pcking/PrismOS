#!/usr/bin/env python3
"""PrismOS Prism Store QEMU test.

A host HTTP server (10.0.2.2:8377) serves the trust root,
a catalog and two packages: one correctly signed with
ML-DSA-44 (payload = the hello compat guest) and one with
a corrupted signature. The guest provisions the trust
root, lists the catalog, installs the good package, runs
it in the compat sandbox, is refused the bad package, and
uninstalls cleanly.
"""
import hashlib
import importlib.util
import os
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 8377
LOG = os.path.join(ROOT, "build", "store-test.log")

_spec = importlib.util.spec_from_file_location(
    "recovery_test",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "recovery-test.py"))
rt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rt)

from cryptography.hazmat.primitives.asymmetric import mldsa
from cryptography.hazmat.primitives import serialization

SK = mldsa.MLDSA44PrivateKey.generate()
PK = SK.public_key()
PK_BYTES = PK.public_bytes(serialization.Encoding.Raw,
                            serialization.PublicFormat.Raw)
FINGERPRINT = hashlib.sha256(PK_BYTES).hexdigest()[:16]


def build_pkg(name, file, version, payload, corrupt=False):
    sig = bytearray(SK.sign(payload))
    if corrupt:
        sig[100] ^= 0xFF
    hdr = f"name={name}\nfile={file}\nversion={version}\n".encode()
    return (b"PRISMPK1" + struct.pack("<II", len(hdr), len(payload))
            + hdr + bytes(sig) + payload)


with open(os.path.join(ROOT, "build", "compat", "hello.elf"),
          "rb") as fh:
    HELLO = fh.read()

FILES = {
    "/store.pub": PK_BYTES,
    "/catalog.txt": b"helloapp 1.0 helloapp.pkg\n"
                    b"evilapp 9.9 evilapp.pkg\n",
    "/helloapp.pkg": build_pkg("helloapp", "helloapp.elf", "1.0",
                               HELLO),
    "/evilapp.pkg": build_pkg("evilapp", "evilapp.elf", "9.9",
                              HELLO, corrupt=True),
}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        data = FILES.get(self.path)
        if data is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def main():
    server = HTTPServer(("0.0.0.0", PORT), Handler)
    threading.Thread(target=server.serve_forever,
                     daemon=True).start()
    rt.make_disk()
    b = rt.Boot(LOG)
    try:
        b.wait_for("prismos> ", 60, "shell prompt")
        b.send("install yes")
        b.wait_for("Installed to disk", 30, "install done")
        b.wait_for("prismos> ", 20, "shell prompt")
        b.send("passwd")
        b.wait_for("New PIN", 20, "new PIN prompt")
        b.send("1357")
        b.wait_for("Confirm PIN", 20, "confirm PIN prompt")
        b.send("1357")
        b.wait_for("Account set", 20, "account set")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("store server 10.0.2.2 %d /" % PORT)
        b.wait_for("store: server configured.", 20, "server cfg")
        b.send("store pubkey fetch 10.0.2.2 %d /store.pub" % PORT)
        b.wait_for("store: fetched pubkey sha256: " + FINGERPRINT, 30,
                   "fetched fingerprint shown in full")
        b.wait_for("Type yes to trust this key: ", 20,
                   "trust confirmation prompt")
        b.send("yes")
        b.wait_for("store: trust root updated (fetched).", 30,
                   "pubkey fetch")
        print("ok: trust root provisioned over HTTP (with "
              "fingerprint confirmation)")
        b.send("store pubkey")
        b.wait_for("store pubkey sha256: " + FINGERPRINT, 20,
                   "fingerprint")
        print("ok: fingerprint matches the served key")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("store list")
        b.wait_for("helloapp 1.0 helloapp.pkg", 30, "catalog")
        print("ok: catalog fetched and listed")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("store install helloapp")
        b.wait_for("store: installed helloapp 1.0", 60, "install")
        print("ok: signed package installed from the catalog")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("run helloapp.elf")
        b.wait_for("Hello from the guest app!", 30, "run app")
        print("ok: installed app runs in the compat sandbox")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("store install evilapp")
        b.wait_for("store: install failed: signature "
                   "verification failed", 60, "evil refused")
        print("ok: corrupted signature refused")
        b.wait_for("prismos> ", 20, "shell prompt")

        b.send("store uninstall helloapp")
        b.wait_for("store: uninstalled helloapp", 20, "uninstall")
        print("ok: uninstall removes the record")
        b.wait_for("prismos> ", 20, "shell prompt")
        pos = b.pos
        b.send("run helloapp.elf")
        b.wait_for("prismos> ", 30, "prompt after run")
        seg = b.segment_since(pos)
        if b"Hello from the guest app!" in seg:
            raise rt.RecoveryTestFailure("app still runs after "
                                         "uninstall")
        print("ok: app file gone after uninstall")
    finally:
        b.stop()
    print("STORE TEST PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except rt.RecoveryTestFailure as exc:
        print("FAIL: %s" % exc)
        sys.exit(1)
