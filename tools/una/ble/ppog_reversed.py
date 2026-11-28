#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Reversed PPoGATT (the watch's service, as Android's Pebble app uses it): the handshake, the
version, a ping, and with --count N the round trip and the rate from the watch. The console is logged to LOG.

    ppog_reversed.py LOG [--count N]
"""

import argparse
import asyncio
import time

import blecommon
import ppog
from bleak import BleakClient

DATA = blecommon.pebble_uuid(0x40000001)
DATA_WR = blecommon.pebble_uuid(0x40000003)


async def main(args):
    async with BleakClient(await blecommon.find(), timeout=25) as c:
        print("connected, mtu", c.mtu_size, flush=True)

        async def send(packet):
            await c.write_gatt_char(DATA_WR, packet, response=False)

        phone = ppog.Phone(send, verbose=args.verbose)
        loop = asyncio.get_running_loop()
        await c.start_notify(
            DATA, lambda ch, d: loop.call_soon_threadsafe(phone.received, d)
        )
        task = asyncio.create_task(phone.run())
        await asyncio.sleep(1)
        # The phone starts: version 1 and a serial number
        await send(ppog.hdr(ppog.RESET_REQ, 0) + bytes([1]) + b"BLUEZTEST000")
        await asyncio.wait_for(phone.open.wait(), 10)
        await send(ppog.hdr(ppog.RESET_COMPLETE, 0) + bytes([25, 25]))
        print("session open", flush=True)
        await asyncio.sleep(0.5)
        await ppog.exercise(phone, args.count)
        task.cancel()


p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
p.add_argument("log")
p.add_argument("--count", type=int, default=0)
p.add_argument("-v", "--verbose", action="store_true")
args = p.parse_args()
con = blecommon.ConsoleLog(args.log)
time.sleep(1)
asyncio.run(main(args))
con.close()
