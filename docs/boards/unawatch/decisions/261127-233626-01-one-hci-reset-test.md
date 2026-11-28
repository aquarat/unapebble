# One HCI_Reset test, the SPI lines released at once

- Status: Accepted
- Date: 2026-11-27 23:36 UTC
- Decided by: maintainer

## Context
261127-204244-01-no-acl-in-link-layer-only-mode proposed, first, trying
the order STM32duinoBLE uses on the BlueNRG-M2SP: `HCI_Reset`, after which
the chip restarts by itself, then link-layer-only mode. `HCI_Reset` had
been ruled out, as it started the chip's updater (hardware.md, 2.5.1);
since then a restart with the SPI lines undriven was found to start the
chip normally.

## Decision
The maintainer approved the test: `HCI_Reset` with SCK, MOSI and CS
released straight after its Command Complete, the recovery sequence (2 s
in reset with the lines undriven, then a short pulse) ready; a start in
the updater is recovered, not retried blindly. If the chip restarts
normally, link-layer-only mode is written and ACL data checked: a BlueZ
connection, GATT discovery and an ATT read and write through NimBLE.
Nothing is written to the chip's flash.

## Consequences
The outcome decides between the other options of 261127-204244-01, which
otherwise stays Proposed.
