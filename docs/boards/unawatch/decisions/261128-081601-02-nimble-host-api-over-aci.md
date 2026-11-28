# NimBLE's host API over the BlueNRG-2's ACI, in place of NimBLE's host

- Status: Accepted
- Date: 2026-11-28 08:16 UTC
- Decided by: agent

## Context
Under 261128-081601-01-aci-on-the-chip-accepted the chip's own stack carries
Bluetooth. PebbleOS's Bluetooth layer calls about 70 distinct NimBLE host
functions (51 in `subsys`, the rest in `pebble/services/bluetooth`), takes
NimBLE's types in its own headers (`ble_addr_t`, `ble_store_value_sec`,
`ble_gap_event`), defines its GATT services as NimBLE `ble_gatt_svc_def`
tables, and receives `os_mbuf` chains in its callbacks.

## Decision
- **Shape.** In this build NimBLE's host is replaced, not wrapped: a layer
  in `third_party/nimble/aci` implements the host API PebbleOS uses (GAP,
  GATT server and client, security, identity, the host's start and stop)
  on ACI commands and events. NimBLE's public headers stay, so PebbleOS
  compiles unchanged, as do the parts of NimBLE that do not depend on its
  host's internals: the porting layer (`os_mbuf`, memory pools, the event
  queue, `nimble_port.c`), UUIDs, advertising data encoding, `ble_hs_mbuf`,
  the store front end and its RAM store with PebbleOS's persistence, and
  the BAS and DIS services. GAP and GATT service, `ble_store_util` and the
  host proper are ours.
- **Configuration.** `NIMBLE_ACI_BLUENRG2`, a choice next to the HCI
  transports, the default on the UNA Watch. The HCI transport
  (`NIMBLE_HCI_BLUENRG2`) and the fake controller stay selectable. The SPI
  protocol and the chip's start-up are shared by both, in
  `transport/bluenrg2_spi.c`.
- **Threads.** One thread owns the SPI, as in the HCI transport. Commands
  are synchronous: the caller waits for the Command Complete or Status.
  Events are queued to NimBLE's host thread, whose callbacks run where
  NimBLE's do, so PebbleOS's threading assumptions hold.
- **Start and stop.** The chip is held in reset while Bluetooth is off and
  restarted at each start of the host, which gives a fresh GATT database:
  services cannot be removed one by one as PebbleOS re-registers them.
- **GATT server.** The services registered with `ble_gatts_add_svcs()` are
  added to the chip at each start, in order; the chip gives the handles,
  which are written to each `val_handle` and kept in a table that maps the
  chip's handles back to the definitions and their access callbacks. Reads
  go to the callback through the read-permit request, writes through the
  write-permit request (or the attribute-modified event where no answer is
  possible), CCCD writes become subscribe events, notifications go out
  with `aci_gatt_update_char_value_ext`. The chip's own GAP and GATT
  services stand in for NimBLE's.
- **Advertising** goes out as standard HCI LE commands, as NimBLE sends
  them; the chip's stack takes the connections it makes.
- **Security** is the chip's: its security manager is set from NimBLE's
  configuration (secure connections only, MITM, display yes/no, bonding),
  numeric comparison goes to PebbleOS as NimBLE's passkey event, the bonds
  are kept by the chip. The ACI gives no keys out, so PebbleOS's store
  holds a record per bond, without keys, made at pairing and reconciled
  with the chip's bond list at each start.
- **References.** ST's ACI headers (X-CUBE-BLE2, SLA0055) give the
  commands and events; they are read for the interface only and nothing
  of them is copied or linked, as before.

## Consequences
- `subsys/bluetooth`, its services and clients, PPoGATT and
  `pebble/services/bluetooth` stay as upstream has them; what is NimBLE
  specific in them works through the layer.
- The layer must follow NimBLE's semantics where PebbleOS depends on them
  (event order, return codes, listeners called newest first); unit tests
  cover the translation that is independent of the chip.
- Behaviour that NimBLE's host has and the chip's stack lacks, such as
  rejecting a pairing request before it starts, needs a substitute or is
  lost; each case is recorded as it is met.
- The chip's stack writes its bonds in its own flash data page, outside the
  checked image; nothing else of the chip's flash is written.
