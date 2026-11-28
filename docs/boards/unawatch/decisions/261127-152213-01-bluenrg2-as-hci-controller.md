# Bluetooth through NimBLE, with the BlueNRG-2 as an HCI controller in link-layer-only mode

- Status: Superseded by 261127-190543-01-bluenrg2-as-hci-controller-accepted
- Date: 2026-11-27 15:22 UTC
- Decided by: agent (proposal; the maintainer decides)

## Context
The board runs on the fake controller (261126-130659-03). The radio is a
BlueNRG-2 on SPI1. Stock UNA drives it through ST's closed host library,
which we cannot use (ST SLA0055). PebbleOS's Bluetooth layer
(`subsys/bluetooth`, `pebble/services/bluetooth`) is written directly on
the NimBLE host API: about 200 calls in 18 files, plus NimBLE types in its
headers. It reaches a controller through a NimBLE HCI transport
(`third_party/nimble/transport`, for example `hci_uart.c` or
`hci_sf32lb.c`).

Measured on the watch (hardware.md, 2.5.1):

- The chip runs ST's standard DTM firmware v3.2.0 (SPI), with BLE stack
  2.1c and its SPI updater.
- It accepts the standard HCI LE controller commands, including
  advertising, which was seen on air.
- It switches to link-layer-only mode with
  `aci_hal_write_config_data(0x2C, 1)`. In that mode the stack on the chip
  is off and ACL data goes to the host. Zephyr and STM32duinoBLE drive
  BlueNRG-2 parts this way.
- The controller has 251-byte data length, privacy and the P-256 and
  DHKey commands. It has no 2M PHY.
- `HCI_Reset`, or a reset pulse soon after a start, sends it into its
  updater, where it answers no command. A known sequence brings it back to
  a normal start every time, but why it works is not established.

## Options
**(a) NimBLE host on the STM32, the BlueNRG-2 as its controller in
link-layer-only mode, on the firmware it already has.** The work:

- An STM32U5 SPI driver: the tree has none.
- A NimBLE transport for the BlueNRG-2 SPI protocol. It frames the
  packets, reads on the IRQ, resets the chip by its pin and answers the
  host's `HCI_Reset` itself. It writes link-layer-only mode after every
  start, and retries a start that ends in the updater.
- An identity address: the chip has no public one, and the module's
  static random address is at config 0x80.

The Bluetooth layer itself is unchanged. That keeps LE Secure Connections
with numeric comparison, NimBLE's bond store, MTU 247 and PPoGATT as on
other boards. The estimate is about 2 to 3 weeks: 3 days for SPI and the
transport, then bring-up with the Pebble app (pairing, reconnection,
throughput, power).

**(b) Our own driver for ST's ACI, with the stack on the chip, under
PebbleOS's Bluetooth layer.** PebbleOS has no Bluetooth driver interface
any more: its layer is NimBLE. This would mean either rewriting
`subsys/bluetooth` and the services on ACI, or writing a NimBLE-shaped
shim over ACI. Either way the chip would own the GATT database (its own
handles), the bonds (12, stored in its own flash), the pairing flow and
the MTU (247). Stack 2.1 supports secure connections and bonding, so it
can be done. The estimate is about 2 to 3 months, followed by permanent
divergence from upstream PebbleOS, and a dependence on ST's on-chip stack
for every fix.

**(c) Other.**

- Reflashing the chip is not needed: the firmware it has does (a).
  Possible ways exist: the DTM's SPI updater, which can be recovered
  through its BLUE flag, and the ROM UART bootloader on IO7 = PA9. ST's
  images are under ST's licences.
- No open link layer exists for the BlueNRG-2.
- Staying on the fake controller means no phone.

## Decision
Proposed: (a).

Before the transport is written, a connection test should confirm that ACL
data reaches the host in link-layer-only mode. Connectable advertising, a
central connecting, and an ATT exchange over raw HCI from a tether
experiment would do. This needs the maintainer's go-ahead, as it is more
radio activity than the brief advertising test done so far.

## Consequences
- Bluetooth on the UNA Watch takes the same path as on other boards. ST
  code is neither linked nor copied; ST's public documents and
  open-licensed drivers are the references.
- The transport owns the chip's quirks: no `HCI_Reset`, a check of each
  start's reason with the recovery sequence of hardware.md 2.5.1, and
  link-layer-only mode written after each start. Knowing what PA8 drives
  on the module (schematic) would let the reset be designed rather than
  found by trial. The transport also uses no
  host-to-controller ACL flow control (as in Zephyr).
- Only 1M PHY: throughput is lower than on boards with a 2M PHY.
- The ST updater stays in the chip. The ACI path (b) stays open if (a)
  fails on something only the chip's stack can do.
