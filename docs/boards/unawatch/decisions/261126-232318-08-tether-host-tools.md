# Host tools on the standard library, with a TCP bridge

- Status: Accepted
- Date: 2026-11-26 23:23 UTC
- Decided by: maintainer

## Context
The watch may be plugged into a Mac while experiments are driven from a
Linux machine. Neither should need extra Python packages.

## Decision
`tether.py` talks to the port through termios (macOS and Linux) or to TCP,
and finds the stub by its USB IDs or name. `bridge.py` serves the port on
TCP, on 127.0.0.1 by default for an SSH tunnel, the network only on request,
and opens the port again after a reset. Both use the standard library only.
`test_tether.py` runs `tether.py` and `bridge.py` against the stub's own
protocol code built for the host (`sim/`), over pipes, a pty and TCP.

## Consequences
The bridge has no authentication: on the network, anyone who reaches its port
can drive the watch, which its log says when it listens beyond localhost.
