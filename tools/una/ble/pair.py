#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Pair BlueZ with the watch by numeric comparison, the watch's side confirmed with Up.

bluetoothctl runs as a DisplayYesNo agent for the session and answers yes; the watch's
confirmation goes through its console (`button click 1`). BlueZ's bond with an earlier address of
the watch is removed first, and the controller is left not pairable again.

    pair.py LOG
"""

import sys
import time

import blecommon
import pexpect

con = blecommon.ConsoleLog(sys.argv[1])
time.sleep(2)
bt = pexpect.spawn("bluetoothctl", encoding="utf-8", timeout=30)
bt.logfile_read = open(sys.argv[1] + ".btctl", "w")  # noqa: SIM115


def cmd(command, expect=None, timeout=10):
    bt.sendline(command)
    if expect:
        return bt.expect(expect, timeout=timeout)
    return None


addr = blecommon.ADDR
cmd("agent DisplayYesNo", ["Agent registered", "already registered", "Failed"])
cmd("default-agent", ["Default agent request successful", "Failed"])
cmd("remove " + addr, ["removed", "not available", "Failed"], 5)
cmd("scan le", ["Discovery started", "Failed", "Invalid"])
try:
    bt.expect(addr, timeout=25)
except pexpect.TIMEOUT:
    raise SystemExit("watch not found") from None
cmd("scan off")
time.sleep(1)
cmd("pair " + addr)
i = bt.expect(
    [
        r"Confirm passkey (\d+)",
        "Pairing successful",
        "Failed to pair.*",
        pexpect.TIMEOUT,
    ]
)
print("bluetoothctl:", bt.match.group(0) if i < 3 else "timeout", flush=True)
if i == 0:
    bt.sendline("yes")
    time.sleep(2)
    con.send("button click 1")
    i = bt.expect(["Pairing successful", "Failed to pair.*", pexpect.TIMEOUT])
    print("bluetoothctl:", bt.match.group(0) if i < 2 else "timeout", flush=True)
time.sleep(3)
con.send("bt bonds")
time.sleep(2)
cmd("disconnect " + addr)
time.sleep(4)
cmd("pairable off", ["succeeded", "Failed"])
bt.sendline("quit")
con.close()
