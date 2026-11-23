#!/usr/bin/env python3
"""PrismOS IPv6 test in QEMU (serial shell, user networking).

1. `ip6` derives the EUI-64 link-local address, sends a
   Router Solicitation and applies the Router Advertisement
   from the user-net gateway (SLAAC): a global address and a
   router link-local address must both appear.
2. `ping6 <router link-local>` must get ICMPv6 echo replies
   (this exercises Neighbor Solicitation/Advertisement in
   both directions plus ICMPv6 echo).
"""
import importlib.util
import os
import sys

_spec = importlib.util.spec_from_file_location(
    "recovery_test",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "recovery-test.py"))
rt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rt)


def main():
    rt.make_disk()
    b = rt.Boot(rt.LOG)
    try:
        b.wait_for("prismos> ", 60, "shell prompt")
        b.send("ip6")
        m = b.wait_for_re(r"ipv6 link-local (fe80::[0-9a-f:]+)", 30,
                          "link-local address")
        print("ok: link-local %s" % m.group(1))
        m = b.wait_for_re(r"ipv6 global ([0-9a-f:]+)", 30,
                          "global address")
        print("ok: global %s (SLAAC)" % m.group(1))
        m = b.wait_for_re(r"ipv6 router (fe80::[0-9a-f:]+)", 30,
                          "router address")
        router = m.group(1)
        print("ok: router %s" % router)
        b.wait_for("prismos> ", 20, "prompt")

        b.send("ping6 %s" % router)
        b.wait_for("reply from %s" % router, 40, "ping6 replies")
        b.wait_for_re(r"ping6: [1-3]/3 replies", 40, "ping6 summary")
        print("ok: ping6 %s answered" % router)
    finally:
        b.stop()
    print("PING6 TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
