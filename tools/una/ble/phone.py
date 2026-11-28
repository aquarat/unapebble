#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""An Android phone over adb, for testing the watch with Gadgetbridge: shell commands, the screen's
widgets (uiautomator) and taps on them.

adb runs where the phone is plugged in: $UNA_ADB_HOST over ssh (unset: here), $UNA_ADB the adb
binary there, $UNA_ADB_SERIAL the phone's serial, always given, as another device may be attached.

    phone.py SHELL-COMMAND        run it on the phone
    phone.py --texts              what the screen shows
    phone.py --tap REGEX          tap the first widget whose text or description matches
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import xml.etree.ElementTree as ET

HOST = os.environ.get("UNA_ADB_HOST", "")
ADB = os.environ.get("UNA_ADB", "adb")
SERIAL = os.environ.get("UNA_ADB_SERIAL", "")


def adb(*args, timeout=120):
    if not SERIAL:
        raise SystemExit("phone: set UNA_ADB_SERIAL")
    cmd = [ADB, "-s", SERIAL, *args]
    if HOST:
        cmd = ["ssh", "-o", "BatchMode=yes", HOST, shlex.join(cmd)]
    r = subprocess.run(cmd, check=False, capture_output=True, timeout=timeout)
    return r.stdout


def sh(command, timeout=120):
    return adb("shell", command, timeout=timeout).decode(errors="replace")


def dump():
    sh("uiautomator dump /sdcard/window_dump.xml >/dev/null")
    x = sh("cat /sdcard/window_dump.xml")
    return (
        ET.fromstring(x[x.index("<?xml") :])
        if "<?xml" in x
        else ET.Element("hierarchy")
    )


def label(node):
    return node.get("text") or node.get("content-desc") or ""


def texts(root):
    return [label(n) for n in root.iter("node") if label(n)]


def find(root, pattern):
    r = re.compile(pattern, re.IGNORECASE)
    return next((n for n in root.iter("node") if r.search(label(n))), None)


def tap(node):
    x1, y1, x2, y2 = map(int, re.findall(r"\d+", node.get("bounds")))
    sh(f"input tap {(x1 + x2) // 2} {(y1 + y2) // 2}")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--texts", action="store_true")
    p.add_argument("--tap")
    p.add_argument("command", nargs="*")
    args = p.parse_args()
    if args.texts:
        print("\n".join(texts(dump())))
    elif args.tap:
        node = find(dump(), args.tap)
        if node is None:
            raise SystemExit(f"phone: nothing matches {args.tap!r}")
        tap(node)
    else:
        sys.stdout.write(sh(" ".join(args.command)))


if __name__ == "__main__":
    main()
