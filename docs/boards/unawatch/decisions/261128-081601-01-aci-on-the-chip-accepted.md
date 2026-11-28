# Bluetooth through the BlueNRG-2's own stack, over ST's ACI, accepted

- Status: Accepted
- Date: 2026-11-28 08:16 UTC
- Decided by: maintainer

## Context
261127-204244-01-no-acl-in-link-layer-only-mode asked how to go on once the
DTM was found to pass no ACL data in link-layer-only mode. Of its options,
the `HCI_Reset` order changed nothing (261127-233959-01), and a binary patch
of the DTM does not start: the chip checks its code before running it
(261128-003746-01, 261128-004526-01). Recorded after the fact, from the
maintainer's brief for the work.

## Decision
Option (b) of 261127-152213-01: Bluetooth runs on the stack in the chip, its
host included, driven by ST's ACI over the SPI protocol the transport
already speaks, link-layer-only mode off. The chip's DTM image stays as it
is and is never written. PebbleOS's Bluetooth subsystem is to be kept as
upstream has it where it can be, with the NimBLE host API it uses provided
over the ACI, rather than rewritten; the agent decides the shape of that
layer. The NimBLE HCI path and the fake controller stay available by
configuration.

## Consequences
- The chip owns the GATT database and its handles, the pairing and its
  bonds (up to 12, in the stack's own data page), the MTU and the link's
  procedures; PebbleOS's layer is translated onto them.
- 261127-204244-01 is closed by this record.
- Work follows in milestones: the ACI transport and advertising; a first
  read and write of PebbleOS's own characteristics from a BlueZ central;
  the rest of the server, notifications, MTU and connection parameters;
  the GATT client (ANCS, AMS); pairing and bonding; power; the Pebble
  app's handshake if a test client exists.
