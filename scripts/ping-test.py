#!/usr/bin/env python3
"""PrismOS ping / firewall test in QEMU (serial shell).

1. `ping 10.0.2.2` gets ICMP echo replies from the
   QEMU user-net host.
2. `fw add 10.0.2.2` blocks it: ping reports the
   firewall block.
3. `fw clear` restores the replies.
4. `ping 10.0.2.99` (nothing there) reports no replies.
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
        b.send("ping 10.0.2.2")
        b.wait_for("reply from 10.0.2.2", 30, "ping replies")
        b.wait_for("ping: 3/3 replies", 30, "ping summary")
        print("ok: ping 10.0.2.2 replies 3/3")
        b.wait_for("prismos> ", 20, "prompt")

        b.send("fw add 10.0.2.2")
        b.wait_for("fw: now dropping 10.0.2.2", 20, "fw add")
        print("ok: fw rule added")
        b.wait_for("prismos> ", 20, "prompt")
        b.send("ping 10.0.2.2")
        b.wait_for("ping: blocked by the firewall.", 20,
                   "ping blocked")
        print("ok: ping blocked by firewall")
        b.wait_for("prismos> ", 20, "prompt")

        b.send("fw clear")
        b.wait_for("fw: all rules cleared.", 20, "fw clear")
        b.wait_for("prismos> ", 20, "prompt")
        b.send("ping 10.0.2.2")
        b.wait_for("ping: 3/3 replies", 30, "ping again")
        print("ok: ping works again after fw clear")
        b.wait_for("prismos> ", 20, "prompt")

        b.send("ping 10.0.2.99")
        b.wait_for("ping: 0/3 replies", 40, "no replies")
        print("ok: unanswered ping reports 0/3")
    finally:
        b.stop()
    print("PING TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
