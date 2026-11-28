# The BlueNRG-2 HCI transport: one thread, the reset answered, the start checked

- Status: Accepted
- Date: 2026-11-27 19:19 UTC
- Decided by: agent

## Context
Measured on the watch (hardware.md, 2.5.1): `HCI_Reset`, or a reset soon
after a start, sends the BlueNRG-2 into its updater, where it answers
nothing; the start event's reason says which; a start from RESET held low
for 2 s with the SPI pins left as the stub found them (undriven) starts
normally, a long reset with the pins driven does not. Link-layer-only mode
is in RAM and has to be set after each start. NimBLE sends `HCI_Reset`
whenever its host synchronises, and waits 2 s for each command. The chip
signals with IRQ both that it is ready for a write and that it has data.

## Decision
`third_party/nimble/transport/hci_bluenrg2.c` (`NIMBLE_HCI_BLUENRG2`):

- **One thread** does every SPI transaction; the host's commands and ACL
  packets are queued to it. It reads whenever IRQ is high, before a write
  too, and sleeps on the IRQ's rising edge otherwise. The edge's interrupt
  masks itself until the thread has looked at the line.
- **The host's `HCI_Reset` is never sent.** The transport answers it with
  a Command Complete, once the chip is up; several while it starts are
  answered once.
- **Start-up**, in the thread, from `ble_transport_ll_init()`: RESETN low
  with the chip select an input, the SPI pins in their analog sleep state;
  the chip select driven high; RESETN released; the start event awaited
  3 s. Reason 1 (normal) is set up: `aci_hal_write_config_data(0x2C, 1, 1)`
  first, then `aci_gatt_init` and `aci_gap_init` (all four roles,
  no privacy), as STM32duinoBLE does on this chip, the firmware details
  logged, the identity address read
  (261127-193700-01-identity-from-config-0x80). Anything else is retried,
  each time from 2 s in reset; after three failures it waits 20 s and
  tries again, without stopping the watch. At boot the first hold is cut
  by the time the chip has already been in reset since its device came
  up.
- **A start the chip makes by itself** (its start event arriving later)
  drops the queue, starts the chip again as above and sends the host a
  Hardware Error, so that it resets and synchronises again.
- **No public address.** The DTM reports `02:80:E1:00:00:00` for
  `HCI_Read_BD_ADDR`, the same on every part: the transport zeroes it, so
  the host takes the identity address as a random static one.
- ISO is refused; vendor events are dropped.
- 1.25 MHz on the bus, as the stock firmware clocks the chip.

## Consequences
Measured since (hardware.md, 2.5.1): with the SPI lines undriven, even a
5 ms reset straight after a start starts the chip normally, so the 2 s
hold is a margin rather than a need.

The host's view of the controller is right after the first start; after a
host reset without a chip restart, state the host does not clear itself
(event masks, data length) carries over, as the reset is not real. The
start-up costs about 2 s after the boot's first 2 s, before Bluetooth
synchronises.
