#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Connect, list the watch's services, read what can be read, write the PPS trigger.

gatt_dump.py [--trigger HEX]
"""

import argparse
import asyncio

import blecommon
from bleak import BleakClient


async def main(args):
    async with BleakClient(await blecommon.find(), timeout=20) as c:
        print("connected, mtu", c.mtu_size)
        for s in c.services:
            print("svc", s.handle, s.uuid, s.description)
            for ch in s.characteristics:
                value = None
                if "read" in ch.properties:
                    try:
                        value = bytes(await c.read_gatt_char(ch))
                    except Exception as e:  # noqa: BLE001 (any refusal is shown)
                        value = f"ERR {e}"
                print("   chr", ch.handle, ch.uuid, ch.properties, value)
                for d in ch.descriptors:
                    print("      dsc", d.handle, d.uuid)
        if args.trigger is not None:
            # 0x02: no security request
            trigger = blecommon.pebble_uuid(2)
            await c.write_gatt_char(trigger, bytes.fromhex(args.trigger), response=True)
            print("wrote the trigger", args.trigger)


p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
p.add_argument(
    "--trigger", help="bytes to write to the Pebble Pairing Service trigger, e.g. 02"
)
asyncio.run(main(p.parse_args()))
