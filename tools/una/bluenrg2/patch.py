#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Back up, patch and restore the DTM firmware of the UNA Watch's BlueNRG-2, through the tether.

ST's DTM v3.2.0 (SPI, BLE stack 2.1.3, build 2353), which the watch's BLE module ships with,
passes no ACL data in link-layer-only mode. The patch adds a little code of ours in the DTM's
unused flash, at 0x10068000 (acl_patch.c, built by the Makefile here), and points two places of
the DTM at it: the HCI parser's branch for packet types other than commands (0x1004e466) and the
stack's callback for received ACL data (0x10045e9e). It only works on that one image: the tool
takes the user's own backup, read from their chip, checks its SHA-256 against the known image
and refuses anything else, and checks the chip still holds it before writing. No ST code is in
this repository; the backup stays with the user.

    patch.py backup OUT.bin      read the flash 0x10042000..0x1007ffff twice, compare, CRC-check
    patch.py check BACKUP.bin    is it the known image; which pages the patch changes
    patch.py write BACKUP.bin    patch the chip (it must hold BACKUP.bin), verify, restart it
    patch.py restore BACKUP.bin  put the original pages back, verify, restart it
    patch.py verify BACKUP.bin   read the flash and say whether it is the original or patched

**The patched DTM does not start.** Tried on a watch on 2026-11-28: the patch writes and reads
back exactly, but the chip then never sends its start event; changing a single padding byte in
the DTM's code does the same, so something checks the image before it runs (hardware.md,
2.5.1). `write` therefore asks for --force; `restore` puts the original back, entering the
updater by its boot pin (IO3, the SPI MOSI line, held high), which works whatever the DTM does.

The watch must run the tether stub (a slot build of PebbleOS is reset to it). Writes go through
the chip's own updater (experiment bluenrg2, modes 9 to 11): only the changed 2 KiB pages,
never below 0x10042800 nor the stack's data page at 0x1007f000.
"""

import argparse
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tether"))
import tether  # noqa: E402

KNOWN_SHA256 = "9a355e010a776f12f451e52da54b9671a26a5c5260268d07c4f69e0ac7393cb5"
FLASH_DTM = 0x10042000
FLASH_END = 0x10080000
FLASH_SIZE = FLASH_END - FLASH_DTM
PAGE = 0x800
PAGE_LOW = 0x10042800
PAGE_HIGH = 0x1007F000

PATCH_BASE = 0x10068000
# Where the DTM is redirected: (address, the 4 bytes there, the symbol the BL goes to)
HOOKS = [
    # HCI parser: `cmp r1, #255; bne` -> `bl acl_patch_dispatch`
    (0x1004E466, bytes.fromhex("ff2912d1"), "acl_patch_dispatch"),
    # Received ACL data callback: `movs r0, #0; nop` -> `bl acl_patch_rx`
    (0x10045E9E, bytes.fromhex("0020c046"), "acl_patch_rx"),
]

EXPERIMENT = os.path.join(HERE, "..", "tether", "build", "experiments", "bluenrg2.bin")
PATCH_BIN = os.path.join(HERE, "build", "acl_patch.bin")
PATCH_SYM = os.path.join(HERE, "build", "acl_patch.sym")

RAM_TABLE = 0x20100000
RAM_PAGES = 0x20101000
RAM_BACKUP = 0x20100000
WRITE_MAGIC = 0x57524E42


def updater_crc(data):
    """The updater's CRC (0xfc29): CRC-32, polynomial 0x04c11db7, init 0, word by word."""
    crc = 0
    for (w,) in struct.iter_unpack("<I", data):
        crc ^= w
        for _ in range(32):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc >> 31 else 0)) & 0xFFFFFFFF
    return crc


def bl(src, dst):
    """A Thumb-2 BL at src to dst."""
    off = dst - (src + 4)
    if not -(1 << 24) <= off < (1 << 24) or off & 1:
        raise ValueError("BL out of range")
    off &= (1 << 25) - 1
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    h1 = 0xF000 | (s << 10) | ((off >> 12) & 0x3FF)
    h2 = 0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF)
    return struct.pack("<HH", h1, h2)


def load_patch():
    if not os.path.exists(PATCH_BIN):
        raise SystemExit(f"{PATCH_BIN} missing: build it, docker/build.sh -- make -C tools/una/bluenrg2")
    code = open(PATCH_BIN, "rb").read()
    syms = {}
    for line in open(PATCH_SYM):
        m = re.match(r"([0-9a-f]+) \w (\w+)", line)
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    if syms.get("acl_patch_dispatch") != PATCH_BASE:
        raise SystemExit("patch: acl_patch_dispatch is not at the patch's base")
    return code, syms


def check_backup(data):
    if len(data) != FLASH_SIZE:
        raise SystemExit(f"backup: {len(data)} bytes, expected {FLASH_SIZE}")
    sha = hashlib.sha256(data).hexdigest()
    if sha != KNOWN_SHA256:
        raise SystemExit(f"backup: SHA-256 {sha} is not the known DTM v3.2.0 image; refusing")


def patched_image(orig):
    code, syms = load_patch()
    img = bytearray(orig)
    start = PATCH_BASE - FLASH_DTM
    if any(b != 0xFF for b in img[start : start + PAGE]):
        raise SystemExit("patch: the patch's page is not blank in this image")
    if len(code) > PAGE:
        raise SystemExit("patch: code larger than a page")
    img[start : start + len(code)] = code
    for addr, before, sym in HOOKS:
        o = addr - FLASH_DTM
        if img[o : o + 4] != before:
            raise SystemExit(f"patch: unexpected bytes at {addr:#010x}")
        img[o : o + 4] = bl(addr, syms[sym])
    return bytes(img)


def changed_pages(src, dst):
    """(address, the page of dst, flags) for each page that differs: 1 erase, 2 program."""
    pages = []
    for o in range(0, FLASH_SIZE, PAGE):
        if src[o : o + PAGE] == dst[o : o + PAGE]:
            continue
        a = FLASH_DTM + o
        if not PAGE_LOW <= a < PAGE_HIGH:
            raise SystemExit(f"patch: page {a:#010x} is outside what may be written")
        new = dst[o : o + PAGE]
        blank_before = all(b == 0xFF for b in src[o : o + PAGE])
        blank_after = all(b == 0xFF for b in new)
        flags = (0 if blank_before else 1) | (0 if blank_after else 2)
        pages.append((a, new, flags))
    return pages


def connect(args):
    t = tether.Tether(tether.open_transport(args.port, args.tcp), timeout=3.0)
    if tether.is_pebbleos(t.info()):
        print("resetting PebbleOS to the stub")
        args.timeout = 3.0
        t = tether.to_stub(t, args)
    return t


def run_experiment(t, mode_args, timeout_s):
    image = open(EXPERIMENT, "rb").read()
    _, status, res, out, fault = t.run_blob(image, mode_args, timeout_s)
    text = out.decode(errors="replace")
    if status != tether.OK:
        raise SystemExit(f"experiment: {tether.STATUS_NAMES.get(status, status)}\n{text}")
    return res["r0"], text


def read_flash(t, allow_no_start=False):
    """The flash, read twice through the updater, compared, and checked against its CRCs."""
    reads = []
    for k in range(2):
        errors, text = run_experiment(t, [9, RAM_BACKUP], 240.0)
        if errors and not (allow_no_start and "0 blocks failed" in text):
            raise SystemExit(f"backup: read {k + 1} failed\n{text}")
        reads.append(t.read(RAM_BACKUP, FLASH_SIZE))
        crcs = [(int(a, 16), int(n), int(c, 16))
                for a, n, c in re.findall(r"crc ([0-9a-f]{8}) (\d+) ([0-9a-f]{8})", text)]
        if sum(n for _, n, _ in crcs) * PAGE != FLASH_SIZE:
            raise SystemExit(f"backup: the updater's CRCs are missing\n{text}")
        for a, n, c in crcs:
            o = a - FLASH_DTM
            if updater_crc(reads[-1][o : o + n * PAGE]) != c:
                raise SystemExit(f"backup: read {k + 1} differs from the updater's CRC at {a:#010x}")
    if reads[0] != reads[1]:
        raise SystemExit("backup: the two reads differ")
    return reads[0]


def write_pages(t, pages):
    table = struct.pack("<II", WRITE_MAGIC, len(pages))
    for i, (a, data, flags) in enumerate(pages):
        ram = RAM_PAGES + i * PAGE
        t.write(ram, data)
        if t.crc32(ram, PAGE) != tether.crc32(data):
            raise SystemExit("write: page data did not reach the watch's RAM intact")
        table += struct.pack("<III", a, ram, flags)
    t.write(RAM_TABLE, table)
    errors, text = run_experiment(t, [11, RAM_TABLE], 240.0)
    print(text, end="")
    if errors:
        raise SystemExit("write: failed; the chip stays in its updater: run `restore`")


def cmd_backup(t, args):
    data = read_flash(t)
    with open(args.out, "wb") as f:
        f.write(data)
    sha = hashlib.sha256(data).hexdigest()
    print(f"{args.out}: {len(data)} bytes, SHA-256 {sha}")
    print("the known DTM v3.2.0 image" if sha == KNOWN_SHA256 else "NOT the known DTM v3.2.0 image")


def cmd_check(_t, args):
    orig = open(args.backup, "rb").read()
    check_backup(orig)
    for a, _data, flags in changed_pages(orig, patched_image(orig)):
        print(f"page {a:#010x}:{' erase' if flags & 1 else ''}{' program' if flags & 2 else ''}")


def cmd_write(t, args):
    if not args.force:
        raise SystemExit("write: the patched DTM does not start (an integrity check); --force to "
                         "write it anyway, `restore` to undo")
    orig = open(args.backup, "rb").read()
    check_backup(orig)
    new = patched_image(orig)
    current = read_flash(t)
    if current == new:
        print("already patched")
        return
    if current != orig:
        raise SystemExit("write: the chip does not hold the backup's image; refusing")
    write_pages(t, changed_pages(orig, new))
    if read_flash(t) != new:
        raise SystemExit("write: the flash read back is not the patched image: run `restore`")
    print("patched, verified, started")


def cmd_restore(t, args):
    orig = open(args.backup, "rb").read()
    check_backup(orig)
    # A DTM that does not start: read through the updater, entered by its boot pin; the reads
    # still pass, the restart at their end does not
    current = read_flash(t, allow_no_start=True)
    if current == orig:
        print("already the original")
        return
    write_pages(t, changed_pages(current, orig))
    if read_flash(t) != orig:
        raise SystemExit("restore: the flash read back is not the original image")
    print("restored, verified, started")


def cmd_verify(t, args):
    orig = open(args.backup, "rb").read()
    check_backup(orig)
    current = read_flash(t)
    if current == orig:
        print("original")
    elif current == patched_image(orig):
        print("patched")
    else:
        print("neither the original nor the patched image")
        return 1
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="the tether's serial port")
    p.add_argument("--tcp", help="HOST:PORT of a bridge.py")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("backup")
    s.add_argument("out")
    for name in ("check", "write", "restore", "verify"):
        s = sub.add_parser(name)
        s.add_argument("backup")
        if name == "write":
            s.add_argument("--force", action="store_true")
    args = p.parse_args()
    if args.cmd == "check":
        return cmd_check(None, args)
    t = connect(args)
    return {"backup": cmd_backup, "write": cmd_write, "restore": cmd_restore,
            "verify": cmd_verify}[args.cmd](t, args) or 0


if __name__ == "__main__":
    sys.exit(main())
