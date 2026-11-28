#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Reconnect as the bonded central: the link encrypted, the PPS status, the reversed PPoGATT
subscription and an encrypted write, the console logged.

    reconnect.py LOG
"""

import asyncio
import sys
import time

import blecommon
from bleak import BleakClient

STATUS = blecommon.pebble_uuid(1)
DATA = blecommon.pebble_uuid(0x40000001)
DATA_WR = blecommon.pebble_uuid(0x40000003)
con = blecommon.ConsoleLog(sys.argv[1])
time.sleep(2)


async def main():
    async with BleakClient(await blecommon.find(), timeout=25) as c:
        print("connected", flush=True)
        print("pps status", bytes(await c.read_gatt_char(STATUS)).hex(), flush=True)
        await c.start_notify(
            DATA, lambda ch, d: print("notify", bytes(d).hex(), flush=True)
        )
        await asyncio.sleep(2)
        await c.write_gatt_char(DATA_WR, b"\x00", response=False)
        await asyncio.sleep(5)
        print("pps status", bytes(await c.read_gatt_char(STATUS)).hex(), flush=True)
    con.send("bt bonds")
    await asyncio.sleep(2)


asyncio.run(main())
con.close()
