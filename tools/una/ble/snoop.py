#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""A summary of a btsnoop log (Android's HCI snoop log, from a bugreport's
FS/data/misc/bluetooth/logs/btsnoop_hci.log): ATT opcodes each way, ATT error responses, and the
SMP exchange of a pairing.

    snoop.py btsnoop_hci.log
"""

import collections
import struct
import sys

ATT_CID, SMP_CID = 4, 6


def packets(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:8] != b"btsnoop\0":
        raise SystemExit("not a btsnoop log")
    off = 16
    while off + 24 <= len(d):
        _, ilen, flags, _, ts = struct.unpack(">IIIIq", d[off : off + 24])
        yield ("rx" if flags & 1 else "tx"), ts, d[off + 24 : off + 24 + ilen]
        off += 24 + ilen


def main(path):
    ops = collections.Counter()
    errors = collections.Counter()
    smp = []
    for way, _, p in packets(path):
        # H4 ACL: type, handle, length, then L2CAP length and channel
        if len(p) < 10 or p[0] != 2:
            continue
        cid = struct.unpack("<H", p[7:9])[0]
        if cid == ATT_CID:
            ops[(way, p[9])] += 1
            if p[9] == 0x01 and len(p) >= 14:
                handle = struct.unpack("<H", p[11:13])[0]
                errors[(way, p[10], handle, p[13])] += 1
        elif cid == SMP_CID:
            smp.append((way, p[9], p[10:17].hex()))
    print("ATT opcodes (tx: from the phone)")
    for (way, op), n in sorted(ops.items(), key=lambda x: -x[1]):
        print(f"  {way} {op:#04x} {n}")
    print("ATT errors: way, request, handle, code")
    for (way, req, handle, code), n in errors.items():
        print(f"  {way} {req:#04x} {handle:#06x} {code:#04x} x{n}")
    print("SMP")
    for way, code, head in smp:
        print(f"  {way} {code:#04x} {head}")


if __name__ == "__main__":
    main(sys.argv[1])
