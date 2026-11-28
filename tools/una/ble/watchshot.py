#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""A screenshot of the watch read from its framebuffer over the tether, whatever it shows, pairing
prompts included, which no phone app can take.

The compositor's framebuffer is s_framebuffer, 260 by 260 bytes of 8-bit Pebble colour (two bits
each of alpha, red, green, blue, as Gadgetbridge's screenshots decode them too); its address is the
build's, from its ELF. Outside the round display the framebuffer is not masked.

    watchshot.py --elf build/pebbleos.elf OUT.png [--scale 3]
    watchshot.py --addr 0x2001864c OUT.png
"""

import argparse
import os
import struct
import subprocess
import sys
import zlib

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tether")
)
import tether

SIZE = 260


def framebuffer_addr(elf):
    for nm in ("arm-none-eabi-nm", "nm"):
        try:
            out = subprocess.run(
                [nm, elf], check=True, capture_output=True, text=True
            ).stdout
        except (OSError, subprocess.CalledProcessError):
            continue
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == "s_framebuffer":
                return int(parts[0], 16)
    raise SystemExit("watchshot: no s_framebuffer in the ELF (or no nm)")


def png(pixels, width, height, scale):
    def chunk(tag, body):
        return (
            struct.pack(">I", len(body))
            + tag
            + body
            + struct.pack(">I", zlib.crc32(tag + body))
        )

    rows = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            c = pixels[y * width + x]
            rgb = bytes((((c >> 4) & 3) * 85, ((c >> 2) & 3) * 85, (c & 3) * 85))
            line += rgb * scale
        rows += [b"\0" + bytes(line)] * scale
    head = struct.pack(">IIBBBBB", width * scale, height * scale, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", head)
        + chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
        + chunk(b"IEND", b"")
    )


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("out")
    p.add_argument("--elf")
    p.add_argument("--addr", type=lambda s: int(s, 0))
    p.add_argument("--scale", type=int, default=1)
    args = p.parse_args()
    addr = args.addr if args.addr is not None else framebuffer_addr(args.elf)
    t = tether.Tether(tether.open_transport())
    pixels = t.read(addr, SIZE * SIZE)
    with open(args.out, "wb") as f:
        f.write(png(pixels, SIZE, SIZE, args.scale))


if __name__ == "__main__":
    main()
