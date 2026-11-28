#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Hold a connection to the watch for SECONDS, for measuring connected idle.

hold.py SECONDS
"""

import asyncio
import sys
import time

import blecommon
from bleak import BleakClient


async def main(secs):
    async with BleakClient(await blecommon.find(), timeout=25) as c:
        print("connected", time.strftime("%H:%M:%S"), flush=True)
        start = time.time()
        while time.time() - start < secs and c.is_connected:
            await asyncio.sleep(10)
        print(
            "held", int(time.time() - start), "s, connected", c.is_connected, flush=True
        )


asyncio.run(main(float(sys.argv[1])))
