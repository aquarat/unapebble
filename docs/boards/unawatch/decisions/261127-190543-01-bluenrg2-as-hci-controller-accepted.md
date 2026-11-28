# Bluetooth through NimBLE, with the BlueNRG-2 as an HCI controller in link-layer-only mode, accepted

- Status: Accepted
- Date: 2026-11-27 19:05 UTC
- Decided by: maintainer

## Context
261127-152213-01-bluenrg2-as-hci-controller proposed three ways to bring
Bluetooth to the watch: (a) PebbleOS's NimBLE host on the STM32, the
BlueNRG-2 as a plain HCI controller in link-layer-only mode on the DTM
firmware it already runs; (b) a driver for ST's ACI, with the stack on the
chip, under PebbleOS's Bluetooth layer; (c) others, such as reflashing the
chip. Recorded after the fact, from the maintainer's brief for the work.

## Decision
Option (a). The maintainer also approved live connection tests, the
connection test the proposal asked for included.

## Consequences
The consequences listed in 261127-152213-01 hold. The chip is not
reflashed. Work follows in milestones: the HCI transport over the chip's
SPI protocol, wired into NimBLE as on the other boards; advertising, a
connection, GATT discovery and an ATT read and write; then the Pebble
protocol over GATT (PPoGATT) as far as it is solid.
