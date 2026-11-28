#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Forward PPoGATT (the phone's service, as iOS's Pebble app hosts it): BlueZ serves the PPoGATT
service through bless, registered without advertising, the watch is its client. The handshake, the
version, a ping, and with --count N the round trip and the rate from the watch. The console is logged to LOG.

    ppog_forward.py LOG [--count N]
"""

import argparse
import asyncio
import time

import blecommon
import ppog
from bleak import BleakClient
from bless import BlessServer
from bless import GATTAttributePermissions as Perm
from bless import GATTCharacteristicProperties as Prop

SVC = blecommon.pebble_uuid(0x10000000)
DATA = blecommon.pebble_uuid(0x10000001)
META = blecommon.pebble_uuid(0x10000002)
# Versions 0 to 1, the system app's UUID, its session type inferred from it
META_VALUE = bytes([0, 1]) + bytes(16) + bytes([0])


async def main(args):
    loop = asyncio.get_running_loop()
    srv = BlessServer(name="ppogatt-test", loop=loop)
    phone = None

    def on_write(ch, value, **kwargs):
        loop.call_soon_threadsafe(phone.received, bytes(value))

    srv.read_request_func = lambda ch, **kwargs: ch.value
    srv.write_request_func = on_write
    await srv.add_new_service(SVC)
    await srv.add_new_characteristic(SVC, META, Prop.read, META_VALUE, Perm.readable)
    await srv.add_new_characteristic(
        SVC,
        DATA,
        Prop.write_without_response | Prop.notify,
        None,
        Perm.readable | Perm.writeable,
    )
    srv.get_characteristic(META).value = bytearray(META_VALUE)
    # bless's start() would advertise too: only the service is registered
    await srv.setup_task
    srv.bus.export(srv.app.path, srv.app)
    await srv.app.register(srv.adapter)

    async def send(packet):
        srv.get_characteristic(DATA).value = bytearray(packet)
        srv.update_value(SVC, DATA)

    phone = ppog.Phone(send, verbose=args.verbose)
    task = asyncio.create_task(phone.run())
    try:
        async with BleakClient(await blecommon.find(), timeout=25):
            print("connected", flush=True)
            # The watch finds the service, reads the meta, subscribes and asks for a reset
            await asyncio.wait_for(phone.open.wait(), 20)
            print("session open", flush=True)
            await asyncio.sleep(0.5)
            await ppog.exercise(phone, args.count)
    finally:
        task.cancel()
        await srv.app.unregister(srv.adapter)
        srv.bus.unexport(srv.app.path, srv.app)


p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
p.add_argument("log")
p.add_argument("--count", type=int, default=0)
p.add_argument("-v", "--verbose", action="store_true")
args = p.parse_args()
con = blecommon.ConsoleLog(args.log)
time.sleep(1)
asyncio.run(main(args))
con.close()
