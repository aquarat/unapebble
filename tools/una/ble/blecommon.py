# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""What the BLE test clients share: the watch's address, finding it, and a console logger.

The console logger keeps PebbleOS's console (bridge.py's port 4501, or $UNA_CONSOLE_TCP) open in a
thread, writes everything to a file and sends shell commands on the same connection: the bridge
serves one client at a time.
"""

import asyncio
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tether")
)
import tether

# The module's static random address, config 0x80 (decision 261128-100132-02)
ADDR = os.environ.get("UNA_BLE_ADDR", "CD:CE:E6:EC:45:CE")
CONSOLE_TCP = os.environ.get("UNA_CONSOLE_TCP", "127.0.0.1:4501")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[@-Z\\-_]")


def pebble_uuid(u32):
    """A Pebble 128-bit UUID from its 32 bits, as PBL_BT_PEBBLE_UUID128_INIT() makes it."""
    return f"{u32:08x}-328e-0fbb-c642-1aa6699bdada"


def close_leftover_link():
    """A link BlueZ kept from an earlier client closed: the watch does not advertise meanwhile."""
    info = subprocess.run(
        ["bluetoothctl", "info", ADDR], check=False, capture_output=True, text=True
    ).stdout
    if "Connected: yes" in info:
        subprocess.run(
            ["bluetoothctl", "disconnect", ADDR],
            check=False,
            capture_output=True,
            timeout=15,
        )


async def find(timeout=25.0):
    """The watch, advertising."""
    from bleak import BleakScanner

    await asyncio.to_thread(close_leftover_link)
    dev = await BleakScanner.find_device_by_address(ADDR, timeout=timeout)
    if dev is None:
        raise SystemExit(f"{ADDR} not found")
    return dev


class ConsoleLog:
    def __init__(self, path, tcp=CONSOLE_TCP):
        self.t = tether.TcpTransport(tcp)
        self.log = open(path, "a", buffering=1)  # noqa: SIM115 (open while it logs)
        self.buf = ""
        self.lock = threading.Lock()
        self.stop = False
        self.th = threading.Thread(target=self._run, daemon=True)
        self.th.start()

    def _run(self):
        while not self.stop:
            try:
                data = self.t.read(65536, 0.1)
            except Exception as e:  # noqa: BLE001 (the watch restarting, the bridge reconnecting)
                self.log.write(f"[console read error {e}]\n")
                time.sleep(0.5)
                continue
            if data:
                text = ANSI.sub("", data.decode(errors="replace")).replace("\r", "")
                with self.lock:
                    self.buf += text
                self.log.write(text)

    def send(self, command):
        """A shell command: Ctrl-C first drops a half-typed line."""
        self.log.write(f"\n[> {command}]\n")
        self.t.write(b"\x03")
        time.sleep(0.2)
        self.t.write(command.encode() + b"\r")

    def wait_for(self, pattern, timeout, start=0):
        """The first match of pattern in what came after offset start, or None."""
        r = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                m = r.search(self.buf, start)
            if m:
                return m
            time.sleep(0.1)
        return None

    def close(self):
        self.stop = True
        self.th.join(1)
