#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Serve the watch's serial ports on TCP, for a watch plugged into another machine.

On the machine the watch is plugged into (standard library only):

    python3 bridge.py                        # 127.0.0.1:4500 and :4501, for an SSH tunnel
    python3 bridge.py --listen 0.0.0.0:4500  # the whole network: no authentication

Port 4500 carries the tether protocol: the stub's port, or PebbleOS's tether
port when PebbleOS runs with its USB developer access, whichever is there, so
tether.py --tcp HOST:4500 drives either. Port 4501 (--console-listen) carries
PebbleOS's console, for console.py. Each serves one client at a time; a new
one replaces the last. A port is opened again when the watch comes back after
a reset, as the stub or as PebbleOS, so a client survives it.
"""

import argparse
import os
import select
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tether  # noqa: E402


def log(msg):
    print(f"bridge: {msg}", file=sys.stderr, flush=True)


def open_port(path, role="tether"):
    if path is None and role == "tether":
        path = os.environ.get("TETHER_PORT")
    ports = [path] if path else tether.find_ports(role)
    for p in ports:
        try:
            return tether.SerialTransport(p)
        except OSError:
            continue
    return None


def pump(client, port_path, stop=None, role="tether"):
    """Moves bytes both ways until the client goes. Returns when it does."""
    serial = None
    waiting_since = None
    try:
        while stop is None or not stop.is_set():
            if serial is None:
                serial = open_port(port_path, role)
                if serial is None:
                    if waiting_since is None:
                        waiting_since = time.monotonic()
                        log(f"waiting for the watch's {role} port")
                else:
                    log(f"{role}: port {serial.name} open")
                    waiting_since = None
            fds = [client] + ([serial.fd] if serial else [])
            r, _, _ = select.select(fds, [], [], 0.2 if serial else 0.5)
            if client in r:
                data = client.recv(65536)
                if not data:
                    log(f"{role}: client gone")
                    return
                if serial:
                    try:
                        serial.write(data)
                    except OSError as e:
                        log(f"{role}: port lost: {e}")
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
                    log(f"{role}: port lost: {e}")
                    serial.close()
                    serial = None
                    continue
                if data:
                    client.sendall(data)
    finally:
        if serial:
            serial.close()


def serve(listen, port_path=None, once=False, ready=None, stop=None, role="tether"):
    host, _, port = listen.rpartition(":")
    srv = socket.socket(socket.AF_INET6 if ":" in host.strip("[]") else socket.AF_INET)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host.strip("[]") or "127.0.0.1", int(port)))
    srv.listen(1)
    if ready is not None:
        ready(srv.getsockname())
    log(f"{role}: listening on {listen}")
    try:
        while stop is None or not stop.is_set():
            r, _, _ = select.select([srv], [], [], 0.5)
            if not r:
                continue
            client, addr = srv.accept()
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            log(f"{role}: client {addr[0]}:{addr[1]}")
            try:
                pump(client, port_path, stop, role)
            except OSError as e:
                log(f"{role}: client lost: {e}")
            finally:
                client.close()
            if once:
                return
    finally:
        srv.close()


def _address(text):
    return text if ":" in text else f"127.0.0.1:{text}"


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", help="the tether port (default: the stub's or PebbleOS's, found by their "
                                  "USB IDs and interface, or by name)")
    p.add_argument("--listen", default="127.0.0.1:4500",
                   help="[HOST:]PORT for the tether protocol (default 127.0.0.1:4500)")
    p.add_argument("--console-port", help="PebbleOS's console port (default: found as --port is)")
    p.add_argument("--console-listen", default="127.0.0.1:4501",
                   help="[HOST:]PORT for PebbleOS's console (default 127.0.0.1:4501; 'none' for none)")
    args = p.parse_args()
    listens = [(_address(args.listen), args.port, "tether")]
    if args.console_listen and args.console_listen != "none":
        listens.append((_address(args.console_listen), args.console_port, "console"))
    for listen, _, _ in listens:
        if not listen.startswith(("127.", "localhost", "[::1]")):
            log(f"{listen}: listening beyond this machine: anyone who can reach the port can drive "
                "the watch")
    stop = threading.Event()
    threads = [threading.Thread(target=serve, args=(listen, path), kwargs={"stop": stop, "role": role},
                                daemon=True) for listen, path, role in listens]
    for th in threads:
        th.start()
    try:
        while any(th.is_alive() for th in threads):
            time.sleep(0.5)
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()
