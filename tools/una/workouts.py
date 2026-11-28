#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""List the workouts recorded on a UNA Watch and download them as FIT files.

Over PebbleOS's console on USB (the USB Developer setting, or a debug build), or a bridge.py on
TCP. Standard library only; console.py and tether.py next to it in tools/una/tether. See
docs/boards/unawatch/index.md, Workouts.

    workouts.py list
    workouts.py get                    # every workout not downloaded yet, to the current directory
    workouts.py get 6a4b3c20 -o ~/fit  # one, to a directory
    workouts.py get --all --force      # all of them again
    workouts.py delete 6a4b3c20
    workouts.py --tcp 127.0.0.1:4501 list
"""

import argparse
import base64
import datetime
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tether"))
import console

CHUNK = 16384
TRIES = 3
DATA = re.compile(r"^D ([0-9a-f]{8}) ([A-Za-z0-9+/=]+)$")
END = re.compile(r"^E ([0-9a-f]{8}) (-?\d+) ([0-9a-f]{8}) (\d+) ([0-9a-f]{8})$")
ROW = re.compile(r"^([0-9a-f]{8}) (\S+) (\S+) (\d+) ([01]) (\d+) (\d+) (\d+) (\d+)$")


class Watch:
    def __init__(self, con):
        self.con = con

    def run(self, command, timeout=30.0):
        out, ok = self.con.run(command, timeout)
        if not ok:
            raise SystemExit(f"workouts: no answer to '{command}'")
        return out

    def list(self):
        rows = []
        for line in self.run("workout list").splitlines():
            m = ROW.match(line.strip())
            if m:
                rows.append(
                    {
                        "id": m[1],
                        "sport": m[2],
                        "state": m[3],
                        "utc": int(m[4]),
                        "gnss_time": m[5] == "1",
                        "elapsed_s": int(m[6]),
                        "timer_s": int(m[7]),
                        "distance_m": int(m[8]),
                        "laps": int(m[9]),
                    }
                )
        return rows

    def chunk(self, wid, offset, length):
        """(file size, bytes) of a range of the FIT file, checked."""
        out = self.run(f"workout fit {wid} {offset} {length}", timeout=60.0)
        data = bytearray()
        end = None
        for line in out.splitlines():
            line = line.strip()
            m = DATA.match(line)
            if m:
                if int(m[1], 16) != offset + len(data):
                    raise ValueError(f"gap at {offset + len(data):#x}")
                data += base64.b64decode(m[2], validate=True)
                continue
            m = END.match(line)
            if m:
                end = m
            elif line.startswith("ERR") or "no activity" in line:
                raise SystemExit(f"workouts: {line}")
        if end is None:
            raise ValueError("no end line")
        size, sent, crc = int(end[2]), int(end[4]), int(end[5], 16)
        if size < 0:
            raise SystemExit(f"workouts: the watch could not export {wid} ({size})")
        if sent != len(data) or zlib.crc32(data) != crc:
            raise ValueError("CRC-32 mismatch")
        return size, bytes(data)

    def fit(self, wid):
        data = bytearray()
        size = None
        while size is None or len(data) < size:
            for attempt in range(TRIES):
                try:
                    size, part = self.chunk(wid, len(data), CHUNK)
                    break
                except ValueError as e:
                    if attempt == TRIES - 1:
                        raise SystemExit(f"workouts: {wid} at {len(data):#x}: {e}")
            if not part and len(data) < size:
                raise SystemExit(f"workouts: {wid}: short file")
            data += part
        check_fit(data)
        return bytes(data)

    def delete(self, wid):
        return self.run(f"workout delete {wid}")


def fit_crc(data):
    table = (
        0x0000,
        0xCC01,
        0xD801,
        0x1400,
        0xF001,
        0x3C00,
        0x2800,
        0xE401,
        0xA001,
        0x6C00,
        0x7800,
        0xB401,
        0x5000,
        0x9C01,
        0x8801,
        0x4400,
    )
    crc = 0
    for b in data:
        for nibble in (b & 0xF, b >> 4):
            tmp = table[crc & 0xF]
            crc = (crc >> 4) & 0x0FFF
            crc = crc ^ tmp ^ table[nibble]
    return crc


def check_fit(data):
    if len(data) < 16 or data[8:12] != b".FIT" or data[0] != 14:
        raise SystemExit("workouts: not a FIT file")
    if int.from_bytes(data[4:8], "little") != len(data) - 16:
        raise SystemExit("workouts: FIT size mismatch")
    if fit_crc(data[:14]) != 0 or fit_crc(data) != 0:
        raise SystemExit("workouts: FIT CRC mismatch")


def file_name(row):
    t = datetime.datetime.fromtimestamp(row["utc"], datetime.UTC)
    return f"{t:%Y-%m-%d-%H%M%S}-{row['sport'].lower()}-{row['id']}.fit"


def fmt_row(row):
    t = datetime.datetime.fromtimestamp(row["utc"], datetime.UTC)
    timer = row["timer_s"]
    return (
        f"{row['id']}  {t:%Y-%m-%d %H:%M}Z{'' if row['gnss_time'] else '?'}  "
        f"{row['sport']:<5} {row['distance_m'] / 1000:6.2f} km  "
        f"{timer // 3600}:{timer // 60 % 60:02}:{timer % 60:02}  {row['laps']:3} laps  "
        f"{row['state']}"
    )


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument(
        "--port", help="PebbleOS's console port (default: found by its USB IDs)"
    )
    p.add_argument(
        "--tcp", help="HOST:PORT of a bridge.py serving the console, usually :4501"
    )
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="the workouts on the watch")
    g = sub.add_parser("get", help="download workouts as FIT files")
    g.add_argument(
        "ids", nargs="*", help="workout ids (default: those not downloaded yet)"
    )
    g.add_argument("-o", "--out", default=".", help="directory for the files")
    g.add_argument("--all", action="store_true", help="every workout")
    g.add_argument(
        "--force", action="store_true", help="download again files already there"
    )
    g.add_argument(
        "--include-open",
        action="store_true",
        help="also workouts not closed yet, as far as recorded",
    )
    d = sub.add_parser("delete", help="delete workouts from the watch")
    d.add_argument("ids", nargs="+")
    args = p.parse_args(argv)

    transport = console.open_console(args.port, args.tcp)
    con = console.Console(transport)
    try:
        if not con.prompt():
            print("workouts: no prompt from the watch", file=sys.stderr)
            return 2
        watch = Watch(con)
        if args.cmd == "list":
            rows = watch.list()
            for row in rows:
                print(fmt_row(row))
            if not rows:
                print("no workouts")
            print(
                "? : the time is the watch's clock, the GNSS never gave one",
                file=sys.stderr,
            )
        elif args.cmd == "get":
            rows = {r["id"]: r for r in watch.list()}
            ids = args.ids or [
                i
                for i, r in rows.items()
                if r["state"] not in ("unreadable", "recording")
                and (args.include_open or args.all or r["state"] != "open")
            ]
            os.makedirs(args.out, exist_ok=True)
            for wid in ids:
                if wid not in rows:
                    raise SystemExit(f"workouts: no workout {wid}")
                path = os.path.join(args.out, file_name(rows[wid]))
                if os.path.exists(path) and not args.force and not args.ids:
                    continue
                data = watch.fit(wid)
                with open(path + ".part", "wb") as f:
                    f.write(data)
                os.replace(path + ".part", path)
                print(f"{path}  {len(data)} bytes  {fmt_row(rows[wid])}")
        elif args.cmd == "delete":
            for wid in args.ids:
                print(watch.delete(wid).strip())
    except (TimeoutError, ConnectionError) as e:
        print(f"workouts: {e}", file=sys.stderr)
        return 2
    finally:
        transport.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
