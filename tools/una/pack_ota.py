#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Pack a PebbleOS image for the UNA Watch bootloader.

The bootloader installs a 64-byte plaintext header followed by an
AES-256-CBC payload (a 16-byte subheader and the zero-padded image) to
0x08060000. The format is described in docs/boards/unawatch/hardware.md,
section 9; this matches UNA's own firmware_packer/encrypt_firmware.py.
"""

import argparse
import base64
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

IMAGE_TYPES = {"ota": 1, "gld": 2}
FLASH_BASE = 0x08060000
RAM_START = 0x20000000
# The bootloader refuses to jump unless the initial SP is at or below this.
RAM_SP_LIMIT = 0x20270000
# The image may use flash up to the end of the 4 MB STM32U5A5, the bootloader's
# limit. --max-size 0x1a0000 checks it also fits a 2 MB part, the STM32U595
# some sources mention for early watches.
DEFAULT_MAX_SIZE = 0x400000 - 0x60000

DEFAULT_KEYS = Path(__file__).with_name("SecurityKeys.pem")


def read_keys(path):
    lines = Path(path).read_text().splitlines()
    blocks = {}
    for i, line in enumerate(lines):
        line = line.strip()
        if line in ("-----BEGIN KEY-----", "-----BEGIN IV KEY-----"):
            blocks[line] = base64.b64decode(lines[i + 1].strip())
    key = blocks.get("-----BEGIN KEY-----")
    iv = blocks.get("-----BEGIN IV KEY-----")
    if key is None or len(key) != 32 or iv is None or len(iv) != 16:
        sys.exit(f"{path}: expected a 32-byte KEY and a 16-byte IV block")
    return key, iv


def pad(image):
    image += b"\0" * (-len(image) % 16)
    # Bootloaders up to 0.1.1 checked the CRC early for these lengths.
    if (len(image) - 16) % 512 == 0:
        image += b"\0" * 16
    return image


def aes_cbc_encrypt(data, key, iv):
    result = subprocess.run(
        ["openssl", "enc", "-aes-256-cbc", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
        input=data,
        capture_output=True,
        check=True,
    )
    return result.stdout


def parse_version(version):
    """Split e.g. v4.47.0-12-gabcdef into (4, 47, 0) and a suffix."""
    m = re.match(r"v?(\d+)\.(\d+)(?:\.(\d+))?(.*)", version or "")
    if not m:
        return (0, 0, 0), (version or "").encode()[:23]
    major, minor, patch = (int(m.group(i) or 0) for i in (1, 2, 3))
    suffix = m.group(4).lstrip("-").encode()[:23]
    return (min(major, 255), min(minor, 255), min(patch, 255)), suffix


def check_vectors(image):
    if len(image) < 8:
        sys.exit("image too small to hold a vector table")
    sp, reset = struct.unpack_from("<II", image)
    if not RAM_START <= sp <= RAM_SP_LIMIT:
        sys.exit(f"initial SP 0x{sp:08x} is outside 0x{RAM_START:08x}..0x{RAM_SP_LIMIT:08x}")
    if not (reset & 1) or not FLASH_BASE <= reset & ~1 < FLASH_BASE + len(image):
        sys.exit(f"reset vector 0x{reset:08x} is not Thumb code inside the image")


def pack(image, key, iv, image_type, version):
    padded = pad(image)
    bin_crc = zlib.crc32(padded)
    plain = struct.pack("<IIII", len(padded), bin_crc, 0, 0) + padded
    enc = aes_cbc_encrypt(plain, key, iv)
    assert len(enc) == len(plain)

    (major, minor, patch), suffix = parse_version(version)
    user_data = struct.pack(
        "<BBBBBBBB23sBBBBB8s",
        1, IMAGE_TYPES[image_type], 0, 0,
        patch, minor, major, 0,
        suffix, 0,
        # Required shared-libc version: 0 skips the check; PebbleOS doesn't use it.
        0, 0, 0, 0,
        b"\0" * 8,
    )
    header = struct.pack("<IIII", 1, zlib.crc32(enc), len(enc), bin_crc) + user_data
    assert len(header) == 60
    header += struct.pack("<I", zlib.crc32(header))
    return header + enc


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bin", type=Path, help="raw image linked at 0x08060000 (pebbleos.bin)")
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("-t", "--type", choices=IMAGE_TYPES, default="ota",
                        help="ota: an update; gld: the factory-reset image")
    parser.add_argument("-v", "--version", default="", help="e.g. the git describe string")
    parser.add_argument("-k", "--keys", type=Path, default=DEFAULT_KEYS)
    parser.add_argument("--max-size", type=lambda s: int(s, 0), default=DEFAULT_MAX_SIZE)
    args = parser.parse_args()

    image = args.bin.read_bytes()
    check_vectors(image)
    if len(image) > args.max_size:
        sys.exit(f"{args.bin}: {len(image)} bytes is over the {args.max_size}-byte limit")
    if args.output.suffix != f".{args.type}":
        sys.exit(f"the bootloader only finds files ending in .{args.type}")

    key, iv = read_keys(args.keys)
    args.output.write_bytes(pack(image, key, iv, args.type, args.version))
    print(f"{args.output}: {len(image)} byte image, type {args.type}")


if __name__ == "__main__":
    main()
