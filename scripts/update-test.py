#!/usr/bin/env python3
"""PrismOS update system test.

Boots the ISO in QEMU with a blank 16MB ATA disk and a local
HTTP server, and checks the update system end to end:
  1. `update check` finds a feature update 0.3.0;
  2. `update apply` (no yes) refuses a feature update;
  3. `update apply yes` applies it (2 files downloaded);
  4. a second apply downloads nothing (differential update:
     unchanged files are skipped by hash);
  5. a broken file on the server fails the apply and rolls
     back to the pre-update restore point;
  6. `update channel beta` switches to the beta manifest;
  7. `update auto on` + reboot auto-applies a security
     update at boot.

Note: updates replace files, not the running kernel, so the
compiled version stays 27H1 (`ver` still says 27H1) while
the VERSION file changes. That is by design in this phase.
(The compiled-in ordering version used for update comparison
is separate from the display label; see kernel/version.h.)

Usage: python3 scripts/update-test.py
  (exit code 0 = passed; standard library only)
"""

import hashlib
import importlib.util
import os
import subprocess
import sys

_spec = importlib.util.spec_from_file_location(
    "recovery_test",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "recovery-test.py"))
rt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rt)

WWW = "/tmp/prism-www-update"


class UpdateTestFailure(Exception):
    pass


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def write(path, data):
    with open(path, "w") as f:
        f.write(data)


def manifest(version, channel, mtype, base, files):
    lines = ["version=%s" % version, "channel=%s" % channel,
             "type=%s" % mtype, "base=%s" % base]
    for name, data in files:
        lines.append("file %s %s" % (name, sha256_hex(data.encode())))
    return "\n".join(lines) + "\n"


README_030 = "PrismOS 0.3.0\nA customizable OS: modern centered UI, Dock and quick search,\nand a shell with two command styles.\n"
VERSION_030 = "0.3.0\n"
README_BETA = "PrismOS 0.3.1-beta\nA customizable OS: modern centered UI, Dock and quick search,\nand a shell with two command styles.\n"
VERSION_040 = "0.4.0\n"
VERSION_050 = "0.5.0\n"


def sign(path):
    """マニフェストにテスト用鍵で署名し、<path>.sig を置く。
    scripts/pq-sign.py が scripts/pq-private-key.pem を使う。"""
    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run(
        [sys.executable, os.path.join(here, "pq-sign.py"), path],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise UpdateTestFailure("sign failed for %s: %s" %
                                (path, r.stderr.strip()))


def make_www_030():
    os.makedirs(os.path.join(WWW, "updates"), exist_ok=True)
    os.makedirs(os.path.join(WWW, "updates-beta"), exist_ok=True)
    write(os.path.join(WWW, "updates", "README.txt"), README_030)
    write(os.path.join(WWW, "updates", "VERSION"), VERSION_030)
    write(os.path.join(WWW, "updates-beta", "README.txt"), README_BETA)
    write(os.path.join(WWW, "update-manifest.txt"),
          manifest("0.3.0", "stable", "feature", "/updates",
                   [("README.txt", README_030), ("VERSION", VERSION_030)]))
    write(os.path.join(WWW, "update-manifest.txt.beta"),
          manifest("0.3.1-beta", "beta", "feature", "/updates-beta",
                   [("README.txt", README_BETA)]))
    sign(os.path.join(WWW, "update-manifest.txt"))
    sign(os.path.join(WWW, "update-manifest.txt.beta"))


def make_www_040_broken():
    # 0.4.0 manifest lists the correct hashes, but the server
    # file for README.txt is corrupted, so the download fails
    # the hash check and the apply must roll back.
    write(os.path.join(WWW, "updates", "README.txt"),
          "CORRUPTED, NOT 0.4.0\n")
    write(os.path.join(WWW, "updates", "VERSION"), VERSION_040)
    write(os.path.join(WWW, "update-manifest.txt"),
          manifest("0.4.0", "stable", "feature", "/updates",
                   [("README.txt",
                     "PrismOS 0.4.0\nA customizable OS: modern centered UI, Dock and quick search,\nand a shell with two command styles.\n"),
                    ("VERSION", VERSION_040)]))
    sign(os.path.join(WWW, "update-manifest.txt"))


def make_www_050_security():
    write(os.path.join(WWW, "updates", "VERSION"), VERSION_050)
    write(os.path.join(WWW, "update-manifest.txt"),
          manifest("0.5.0", "stable", "security", "/updates",
                   [("VERSION", VERSION_050)]))
    sign(os.path.join(WWW, "update-manifest.txt"))


def main():
    if not os.path.exists(rt.ISO):
        print("MISSING: ISO not found: %s" % rt.ISO)
        return 1
    rt.make_disk()
    print("disk: created 16MB ATA disk")
    make_www_030()
    # HttpServer serves from its module-level WWW; point it here.
    rt.WWW = WWW
    server = rt.HttpServer()
    server.start()
    try:
        b = rt.Boot(rt.LOG)
        try:
            b.wait_for("PrismOS 27H1", 30, "boot banner")
            print("ok: boot banner")
            b.wait_for("prismos> ", 30, "shell prompt")
            b.send("install yes")
            b.wait_for("Installed to disk", 30, "install done")
            print("ok: installed to disk")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("update server 10.0.2.2 /update-manifest.txt")
            b.wait_for("Update server: 10.0.2.2 /update-manifest.txt", 20,
                       "update server set")
            print("ok: update server configured")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 1. check finds 0.3.0 (feature).
            b.send("update check")
            b.wait_for("Update available: 0.3.0 (feature, stable)", 60,
                       "update check")
            print("ok: update check finds 0.3.0 (feature)")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 1b. unsigned manifest is refused (no .sig file).
            os.remove(os.path.join(WWW, "update-manifest.txt.sig"))
            b.send("update check")
            b.wait_for("Update check failed: bad signature.", 60,
                       "unsigned manifest refused")
            print("ok: unsigned manifest refused")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 1c. tampered manifest is refused (content changed
            # after signing, so the signature no longer matches).
            mpath = os.path.join(WWW, "update-manifest.txt")
            with open(mpath) as f:
                tampered = f.read().replace("version=0.3.0",
                                            "version=0.3.9")
            write(mpath, tampered)
            b.send("update check")
            b.wait_for("Update check failed: bad signature.", 60,
                       "tampered manifest refused")
            print("ok: tampered manifest refused")
            b.wait_for("prismos> ", 20, "shell prompt")
            # 署名済みの 0.3.0 に戻す。
            make_www_030()
            b.send("update check")
            b.wait_for("Update available: 0.3.0 (feature, stable)", 60,
                       "update check after restore")
            print("ok: signed manifest works again after restore")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 2. feature update needs confirmation.
            b.send("update apply")
            b.wait_for("This is a feature update.", 60, "feature confirm")
            b.wait_for("Run `update apply yes` to apply it.", 20,
                       "feature hint")
            print("ok: feature update needs confirmation")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 3. apply with yes: 2 files downloaded.
            b.send("update apply yes")
            b.wait_for("Update applied: 0.3.0 (2 downloaded, 0 unchanged).",
                       60, "update applied")
            print("ok: update applied (2 downloaded)")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat README.txt")
            b.wait_for("PrismOS 0.3.0", 20, "README updated")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat VERSION")
            b.wait_for("0.3.0", 20, "VERSION updated")
            print("ok: files are at 0.3.0")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 4. second apply: differential, nothing to download.
            b.send("update apply yes")
            b.wait_for("Update applied: 0.3.0 (0 downloaded, 2 unchanged).",
                       60, "second apply is a no-op")
            print("ok: differential update skips unchanged files")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 5. broken file: apply fails and rolls back.
            make_www_040_broken()
            b.send("update apply yes")
            b.wait_for("Update failed: bad file README.txt", 60,
                       "bad file detected")
            b.wait_for("Rolled back to the restore point.", 20,
                       "rolled back")
            print("ok: broken update rolled back")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat README.txt")
            b.wait_for("PrismOS 0.3.0", 20, "README still 0.3.0")
            print("ok: files are intact after rollback")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 6. beta channel switches the manifest.
            b.send("update channel beta")
            b.wait_for("Update channel: beta", 20, "beta channel set")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("update check")
            b.wait_for("Update available: 0.3.1-beta (feature, beta)", 60,
                       "beta check")
            print("ok: beta channel finds 0.3.1-beta")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("update channel stable")
            b.wait_for("Update channel: stable", 20, "stable channel set")
            print("ok: back to stable channel")
            b.wait_for("prismos> ", 20, "shell prompt")

            # 7. auto security update at boot.
            make_www_050_security()
            b.send("update auto on")
            b.wait_for("Auto security updates: on", 20, "auto on")
            print("ok: auto security updates on")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("reboot")
            b.wait_for("Rebooting PrismOS...", 20, "rebooting")
            # -no-reboot: the reset exits QEMU, so boot again.
            b.wait_for_exit(20, "QEMU reset")
            print("ok: rebooted")
        finally:
            b.stop()
        b = rt.Boot(rt.LOG)
        try:
            b.wait_for("PrismOS 27H1", 60, "banner after reboot")
            b.wait_for("[update] auto-applying security update 0.5.0", 90,
                       "auto security update started")
            b.wait_for("Update applied: 0.5.0 (1 downloaded, 0 unchanged).",
                       60, "auto security update applied")
            print("ok: security update auto-applied at boot")
            b.wait_for("prismos> ", 60, "shell prompt after auto update")
            b.send("cat VERSION")
            b.wait_for("0.5.0", 20, "VERSION is 0.5.0")
            print("ok: VERSION is 0.5.0 after auto update")
            b.wait_for("prismos> ", 20, "shell prompt")
            # 8. explicit rollback to the pre-update restore point.
            b.send("update rollback")
            b.wait_for("Rolled back to 'pre-update-0.5.0'.", 30,
                       "explicit rollback")
            print("ok: explicit rollback to pre-update-0.5.0")
            b.wait_for("prismos> ", 20, "shell prompt")
            b.send("cat VERSION")
            b.wait_for("0.3.0", 20, "VERSION back to 0.3.0")
            print("ok: VERSION is back to 0.3.0 after rollback")
            b.wait_for("prismos> ", 20, "shell prompt")
        finally:
            b.stop()
        print("UPDATE TEST PASSED")
        return 0
    except rt.RecoveryTestFailure as e:
        print("MISSING: %s" % e)
        return 1
    finally:
        server.stop()


if __name__ == "__main__":
    sys.exit(main())
