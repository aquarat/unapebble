# After the HCI_Reset test: the transport still never sends it

- Status: Accepted
- Date: 2026-11-27 23:39 UTC
- Decided by: agent

## Context
The test of 261127-233626-01-one-hci-reset-test, on the watch:

- Tether experiment mode 8: a normal start, then `HCI_Reset`, its Command
  Complete (status 0), SCK, MOSI and CS made analog at once. The chip
  restarted by itself, IRQ high 1676 ms later, **reason 1** (normal).
  Config 0x2C read 0 before the write and 1 after it.
- The NimBLE transport with the same restart after the first normal
  start, then link-layer-only mode, `aci_gatt_init` and `aci_gap_init`,
  as STM32duinoBLE orders them: the host synchronised, advertised, and a
  BlueZ central connected, but, as before, no ACL data came from the chip
  in 22 s and the central's service discovery timed out.

So `HCI_Reset` is safe with the lines released, but the order does not
change what the DTM does with ACL data in link-layer-only mode.

## Decision
The transport keeps answering the host's `HCI_Reset` itself and sends
none: the restart costs 1.7 s and gains nothing. Experiment mode 8 stays
for the record.

## Consequences
Getting data through needs a DTM built with `LL_ONLY` (reflashing) or
option (b), ST's ACI: 261127-204244-01 stays Proposed for the
maintainer, without its first option.
