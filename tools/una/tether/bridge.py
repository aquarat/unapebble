#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Serve the tether stub's serial port on TCP, for a watch plugged into another machine.

On the machine the watch is plugged into (standard library only):

    python3 bridge.py                        # 127.0.0.1:4500, for an SSH tunnel
    python3 bridge.py --listen 0.0.0.0:4500  # the whole network: no authentication

then elsewhere: tether.py --tcp HOST:4500 info. One client at a time; a new
one replaces the last. The port is opened again when the watch comes back
after a reset, so a client survives it.
"""

import argparse
import os
import select
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tether  # noqa: E402


def log(msg):
    print(f"bridge: {msg}", file=sys.stderr, flush=True)


def open_port(path):
    path = path or os.environ.get("TETHER_PORT")
    ports = [path] if path else tether.find_ports()
    for p in ports:
        try:
            return tether.SerialTransport(p)
        except OSError:
            continue
    return None


def pump(client, port_path, stop=None):
    """Moves bytes both ways until the client goes. Returns when it does."""
    serial = None
    waiting_since = None
    try:
        while stop is None or not stop.is_set():
            if serial is None:
                serial = open_port(port_path)
                if serial is None:
                    if waiting_since is None:
                        waiting_since = time.monotonic()
                        log("waiting for the watch's port")
                else:
                    log(f"port {serial.name} open")
                    waiting_since = None
            fds = [client] + ([serial.fd] if serial else [])
            r, _, _ = select.select(fds, [], [], 0.2 if serial else 0.5)
            if client in r:
                data = client.recv(65536)
                if not data:
                    log("client gone")
                    return
                if serial:
                    try:
                        serial.write(data)
                    except OSError as e:
                        log(f"port lost: {e}")
                        serial.close()
                        serial = None
            if serial and serial.fd in r:
                try:
                    data = os.read(serial.fd, 65536)
                    if not data:
                        raise OSError("end of file")
                except BlockingIOError:
                    data = b""
                except OSError as e:
                    log(f"port lost: {e}")
                    serial.close()
                    serial = None
                    continue
                if data:
                    client.sendall(data)
    finally:
        if serial:
            serial.close()


def serve(listen, port_path=None, once=False, ready=None, stop=None):
    host, _, port = listen.rpartition(":")
    srv = socket.socket(socket.AF_INET6 if ":" in host.strip("[]") else socket.AF_INET)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host.strip("[]") or "127.0.0.1", int(port)))
    srv.listen(1)
    if ready is not None:
        ready(srv.getsockname())
    log(f"listening on {listen}")
    try:
        while stop is None or not stop.is_set():
            r, _, _ = select.select([srv], [], [], 0.5)
            if not r:
                continue
            client, addr = srv.accept()
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            log(f"client {addr[0]}:{addr[1]}")
            try:
                pump(client, port_path, stop)
            except OSError as e:
                log(f"client lost: {e}")
            finally:
                client.close()
            if once:
                return
    finally:
        srv.close()


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", help="the stub's serial port (default: found by its USB IDs or name)")
    p.add_argument("--listen", default="127.0.0.1:4500",
                   help="[HOST:]PORT to listen on (default 127.0.0.1:4500)")
    args = p.parse_args()
    listen = args.listen if ":" in args.listen else f"127.0.0.1:{args.listen}"
    if not listen.startswith(("127.", "localhost", "[::1]")):
        log("listening beyond this machine: anyone who can reach the port can drive the watch")
    try:
        serve(listen, args.port)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
