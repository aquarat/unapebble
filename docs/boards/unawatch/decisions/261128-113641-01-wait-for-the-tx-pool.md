# Notifications and writes without response wait for the chip's transmit pool

- Status: Accepted
- Date: 2026-11-28 11:36 UTC
- Decided by: agent

## Context
Under PPoGATT load the chip refuses notifications and writes without
response with insufficient resources (`0x64`) while its transmit pool is
full, and raises `aci_gatt_tx_pool_available_event` once it has room.
NimBLE's API has no such event: its callers, PPoGATT among them, retry on
timers of 20 to 200 ms. The rate from the watch was 1.5 kB/s, and a write
once waited 2 s for its Command Complete.

## Decision
The link thread signals the event; `ble_gatts_notify_custom()` and
`ble_gattc_write_no_rsp_flat()` wait for it after a refusal, up to 100 ms,
three times, before failing as NimBLE would. On NimBLE's host thread they
fail at once, as before: waiting there would hold up every other event of
the chip. A command's buffer is copied by the link thread before
it is written, so that a caller that gave up cannot change it.

## Consequences
14 kB/s through the watch's PPoGATT service, 13 kB/s through the phone's,
12 kB/s for a Gadgetbridge screenshot; no stall seen since. A caller may
block up to 300 ms in those two functions.
