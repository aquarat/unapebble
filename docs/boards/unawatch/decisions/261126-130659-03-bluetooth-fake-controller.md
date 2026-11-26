# Bluetooth on the fake controller until a BlueNRG-2 driver exists

- Status: Accepted
- Date: 2026-11-26 13:06 UTC
- Decided by: agent

## Context
The watch's radio is an ST BlueNRG-2 on SPI, which PebbleOS doesn't support.
Recorded after the fact.

## Decision
The board uses PebbleOS's fake Bluetooth controller, as boards without a
radio do.

## Consequences
No phone connection: no notifications, no app installs from the phone, no
phone-side updates. Apps come only with the firmware image.
