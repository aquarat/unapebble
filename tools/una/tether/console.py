#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Run a command on PebbleOS's console, over its USB serial port or a bridge.py on TCP.

PebbleOS with its USB developer access on has the console, logs and shell,
on the first of its two serial ports; bridge.py serves it on TCP port 4501.
Standard library only. See docs/boards/unawatch/tether.md.

    console.py --tcp 127.0.0.1:4501 'battery status'
    console.py --tcp 127.0.0.1:4501 reboot
    console.py --tcp 127.0.0.1:4501 --follow           # the logs, until Ctrl-C
    console.py 'gauge'                                  # the port found by its USB IDs
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tether  # noqa: E402

PROMPT = "pebble> "
# What the console sends around a log line, to clear and redraw the prompt, and terminal codes
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[@-Z\\-_]")
# Commands after which the watch goes away, and what it says first
GOING = re.compile(r"^(reboot|sys (reset|poweroff|powercut))\b")


def open_console(port=None, tcp=None):
    tcp = tcp or os.environ.get("UNA_CONSOLE_TCP")
    port = port or os.environ.get("UNA_CONSOLE_PORT")
    if tcp:
        return tether.TcpTransport(tcp)
    if not port:
        ports = tether.find_ports("console")
        if len(ports) != 1:
            raise SystemExit("console: no PebbleOS console port found" if not ports
                             else f"console: several ports, pick one with --port: {' '.join(ports)}")
        port = ports[0]
    return tether.SerialTransport(port)


def clean(text):
    """The text as a terminal would leave it: no codes, lines ending in a newline."""
    text = ANSI.sub("", text)
    lines = []
    for line in text.replace("\r\n", "\n").split("\n"):
        # A carriage return starts the line again: what follows the last one stays
        lines.append(line.rsplit("\r", 1)[-1])
    return "\n".join(lines)


class Console:
    def __init__(self, transport, verbose=False):
        self.t = transport
        self.verbose = verbose

    def read_for(self, seconds):
        out = bytearray()
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return bytes(out)
            out += self.t.read(65536, min(left, 0.1))

    def read_until(self, pattern, timeout):
        """What came until the pattern matched the cleaned text, and whether it did."""
        out = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                data = self.t.read(65536, min(deadline - time.monotonic(), 0.1))
            except ConnectionError:
                return out.decode(errors="replace"), False
            if data:
                out += data
                if self.verbose:
                    sys.stderr.write(data.decode(errors="replace"))
                if pattern.search(clean(out.decode(errors="replace"))):
                    return out.decode(errors="replace"), True
        return out.decode(errors="replace"), False

    def prompt(self, timeout=3.0):
        """A fresh prompt: Ctrl-C drops a half-typed line and wakes a shell not yet active."""
        self.read_for(0.2)
        self.t.write(b"\x03")
        _, ok = self.read_until(re.compile(re.escape(PROMPT) + r"$"), timeout)
        return ok

    def run(self, command, timeout=10.0):
        """Runs a command; returns (output, whether the prompt came back)."""
        self.t.write(command.encode() + b"\r")
        going = GOING.match(command) is not None
        pattern = re.compile(r"\n" + re.escape(PROMPT) + r"$") if not going else \
            re.compile(r"Restarting|Powering off|Resetting|\n" + re.escape(PROMPT) + r"$")
        raw, ok = self.read_until(pattern, timeout)
        text = clean(raw)
        lines = text.split("\n")
        # The echo of the command, and the prompt that ends the output
        if lines and lines[0].strip() == command.strip():
            lines = lines[1:]
        if lines and lines[-1].startswith(PROMPT.rstrip()):
            lines = lines[:-1]
        return "\n".join(lines).rstrip("\n") + ("\n" if lines else ""), ok or going


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", help="PebbleOS's console port (default: found by its USB IDs, "
                                  "$UNA_CONSOLE_PORT)")
    p.add_argument("--tcp", help="HOST:PORT of a bridge.py serving it, usually :4501 ($UNA_CONSOLE_TCP)")
    p.add_argument("-t", "--timeout", type=float, default=10.0,
                   help="seconds for the command to give the prompt back")
    p.add_argument("-f", "--follow", action="store_true",
                   help="then print what the console sends, the logs, until Ctrl-C or the timeout "
                        "of --for")
    p.add_argument("--for", dest="duration", type=float, default=0.0,
                   help="with --follow: stop after this many seconds")
    p.add_argument("-v", "--verbose", action="store_true", help="echo what comes in, to stderr")
    p.add_argument("command", nargs="*", help="a shell command, e.g. 'battery status'")
    args = p.parse_args(argv)

    transport = open_console(args.port, args.tcp)
    con = Console(transport, args.verbose)
    status = 0
    try:
        if args.command:
            command = " ".join(args.command)
            if not con.prompt():
                print("console: no prompt from the watch", file=sys.stderr)
                return 2
            out, ok = con.run(command, args.timeout)
            sys.stdout.write(out)
            if not ok:
                print(f"console: no prompt within {args.timeout:g} s", file=sys.stderr)
                status = 2
        if args.follow or not args.command:
            deadline = time.monotonic() + args.duration if args.duration else None
            while deadline is None or time.monotonic() < deadline:
                data = transport.read(65536, 0.2)
                if data:
                    sys.stdout.write(ANSI.sub("", data.decode(errors="replace")).replace("\r", ""))
                    sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    except (TimeoutError, ConnectionError) as e:
        print(f"console: {e}", file=sys.stderr)
        return 2
    finally:
        transport.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
