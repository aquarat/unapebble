# Restored subscriptions read back from the chip

- Status: Accepted
- Date: 2026-11-28 11:36 UTC
- Decided by: agent

## Context
The chip keeps a bonded peer's CCCDs and sets them again when the link is
encrypted with the bond's keys, without an event. NimBLE reports a bonded
peer's subscriptions as subscribe events with the restore reason, from its
own store, and PebbleOS's reversed PPoGATT service starts on one.

## Decision
After the encryption change of a bonded link, each CCCD of the services
added is read with `aci_gatt_read_handle_value`, and a set one is reported
as a subscribe event with the restore reason. The host's store keeps no
CCCD records of its own: the chip's are the ones that decide what it
sends.

## Consequences
Read back on the watch at every bonded reconnection (Gadgetbridge
subscribes to none of the watch's characteristics, so all read 0); a set
one is not yet seen, which needs a central that subscribes and reconnects
bonded, such as BlueZ, paired again.
