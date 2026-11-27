# The DTM passes no ACL data in link-layer-only mode: how to go on

- Status: Proposed
- Date: 2026-11-27 20:42 UTC
- Decided by: agent (proposal; the maintainer decides)

## Context
Option (a) (261127-190543-01-bluenrg2-as-hci-controller-accepted) rests on
the BlueNRG-2's DTM handing ACL data to the host in link-layer-only mode.
The proposal said so from ST's DTM source and its drivers, untried. Tried
on the watch with the NimBLE transport (261127-191927-01) and a BlueZ
central (hardware.md, 2.5.1):

- **Link-layer-only mode on** (`aci_hal_write_config_data(0x2C, 1, 1)`
  first after the start, then `aci_gatt_init` and `aci_gap_init`, as
  STM32duinoBLE does; config 0x2C reads 1): advertising, connection
  complete and a data length update, but no ACL packet from the chip in
  22 s, no vendor event, nothing but HCI events; the host's first ACL
  packet (a Service Changed indication) got no Number Of Completed
  Packets. The central's service discovery timed out. Four runs, the same.
- **Link-layer-only mode off**, the rest the same: the central discovered
  services in 1 s, the chip's own (GAP and GATT, from `aci_gatt_init` and
  `aci_gap_init`), read three characteristics and wrote the Service
  Changed CCCD. So the peer's ACL reaches the chip and its stack answers.
- With it on, the chip's stack is off and the ACL goes nowhere.

The same was reported on ST's community forum for the stock DTM with the
mode set ("connect ... but the list of gatt services are missing"), and
ST's answer there is a DTM built with its `LL_ONLY` option. The mode set by
command apparently turns the chip's host off without routing ACL to the
SPI in a DTM not built for it (inferred; the source is ST's).

The brief rules out reflashing the chip and sending `HCI_Reset`.
STM32duinoBLE, which works on the BlueNRG-M2SP, writes the mode after the
chip has restarted from an `HCI_Reset`, but asks for a reflashed DTM
first; whether the order matters is not known. Since the experiment of
2026-11-27 (mode 7), the updater starts that made `HCI_Reset` dangerous are
explained by the SPI lines driven during the reset, so an `HCI_Reset`
with the lines released straight after its Command Complete might restart
the chip normally (inferred, untried).

## Options
1. **Try the STM32duinoBLE order once, with approval**: `HCI_Reset`, the
   lines released at once, then the mode. Low cost, the recovery is known;
   it needs the maintainer to lift the rule for one experiment.
2. **Reflash the DTM built with `LL_ONLY`**, through its SPI updater or the
   ROM bootloader: ST's sources and tools, under ST's licences; the
   maintainer's call, and a risk to the module.
3. **Option (b)**, ST's ACI with the stack on the chip, under PebbleOS's
   Bluetooth layer: the chip already serves GATT over the air.
4. Stop at what works: advertising and connections, no data.

## Decision
Proposed: 1, then 2 or 3 by its outcome. The transport stays as it is
meanwhile; with the fake controller one setting away.

## Consequences
Until then, the watch advertises and accepts connections but carries no
data: no GATT, no pairing, no PPoGATT.
