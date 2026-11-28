# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""The phone's side of PPoGATT and of the Pebble protocol, enough to test the watch.

PPoGATT packets: one header byte, the type in bits 0 to 2 (0 data, 1 ack, 2 reset request,
3 reset complete) and the sequence number in bits 3 to 7, then the payload. Pebble protocol frames:
a big-endian length and endpoint, then the payload (subsys/pp/transports/gatt/ppogatt_internal.h).
"""

import asyncio
import struct
import time

DATA, ACK, RESET_REQ, RESET_COMPLETE = range(4)
EP_VERSION, EP_PING = 16, 2001
SN_MOD = 32


def hdr(ptype, sn):
    return bytes([(ptype & 7) | ((sn % SN_MOD) << 3)])


def frame(endpoint, payload):
    return struct.pack(">HH", len(payload), endpoint) + payload


def version_request():
    return frame(EP_VERSION, b"\x00")


def ping(cookie):
    return frame(EP_PING, b"\x00" + struct.pack(">I", cookie))


class Phone:
    """send(bytes) is a coroutine that writes (forward: notifies) one packet to the watch."""

    def __init__(self, send, verbose=True):
        self.send = send
        self.verbose = verbose
        self.rx = asyncio.Queue()
        self.open = asyncio.Event()
        self.frames = asyncio.Queue()
        self.window = 4
        self.next_sn = 0
        self.unacked = 0
        self.acked = asyncio.Event()
        self.buf = b""
        self.t0 = time.time()
        self.retransmitted = 0

    def log(self, *args):
        if self.verbose:
            print(f"{time.time() - self.t0:8.3f}", *args, flush=True)

    def received(self, packet):
        """From the watch, from any thread's callback through the loop."""
        self.rx.put_nowait(bytes(packet))

    async def run(self):
        last_rx = -1
        while True:
            p = await self.rx.get()
            ptype, sn = p[0] & 7, p[0] >> 3
            if ptype == RESET_REQ:
                self.log(
                    "reset request from the watch, version",
                    p[1] if len(p) > 1 else None,
                )
                # Our windows: 25 each
                await self.send(hdr(RESET_COMPLETE, 0) + bytes([25, 25]))
            elif ptype == RESET_COMPLETE:
                if len(p) >= 3:
                    self.window = min(p[1], 25)
                self.log("reset complete from the watch, its rx window", p[1:3].hex())
                if not self.open.is_set():
                    self.open.set()
            elif ptype == ACK:
                self.unacked = max(0, (self.next_sn - (sn + 1)) % SN_MOD)
                self.acked.set()
            elif ptype == DATA:
                if sn == last_rx:
                    self.retransmitted += 1
                last_rx = sn
                await self.send(hdr(ACK, sn))
                self.buf += p[1:]
                while len(self.buf) >= 4:
                    length, endpoint = struct.unpack(">HH", self.buf[:4])
                    if len(self.buf) < 4 + length:
                        break
                    self.frames.put_nowait((endpoint, self.buf[4 : 4 + length]))
                    self.buf = self.buf[4 + length :]

    async def send_data(self, payload):
        while self.unacked >= self.window:
            self.acked.clear()
            await asyncio.wait_for(self.acked.wait(), 6)
        await self.send(hdr(DATA, self.next_sn) + payload)
        self.next_sn = (self.next_sn + 1) % SN_MOD
        self.unacked += 1

    async def frame_from(self, endpoint, timeout=10):
        end = time.time() + timeout
        while True:
            ep, body = await asyncio.wait_for(
                self.frames.get(), max(0.1, end - time.time())
            )
            if ep == endpoint:
                return body
            self.log("frame from endpoint", ep, body[:16].hex())


def firmware_of(version_reply):
    return version_reply[5:37].split(b"\0")[0].decode(errors="replace")


async def exercise(phone, count=0, in_flight=4):
    """The version and a ping; then count version requests one at a time, for the round trip, and
    count with in_flight outstanding, for the rate from the watch. Pings are not used for rates:
    PebbleOS shows a window for each."""
    await phone.send_data(version_request())
    print("firmware:", firmware_of(await phone.frame_from(EP_VERSION)), flush=True)
    await phone.send_data(ping(0xDEADBEEF))
    print("pong:", (await phone.frame_from(EP_PING)).hex(), flush=True)
    if count:
        start = time.time()
        for _ in range(count):
            await phone.send_data(version_request())
            await phone.frame_from(EP_VERSION)
        secs = time.time() - start
        print(
            f"{count} version requests in turn: {secs / count * 1000:.0f} ms each",
            flush=True,
        )
        start = time.time()
        n = 0
        sent = 0
        done = 0
        while done < count:
            while sent < count and sent - done < in_flight:
                await phone.send_data(version_request())
                sent += 1
            n += len(await phone.frame_from(EP_VERSION)) + 4
            done += 1
        secs = time.time() - start
        print(
            f"{count} replies, {in_flight} asked at a time: {n} bytes in {secs:.2f} s, "
            f"{n / secs:.0f} bytes a second from the watch",
            flush=True,
        )
    print("data packets the watch sent twice:", phone.retransmitted, flush=True)
