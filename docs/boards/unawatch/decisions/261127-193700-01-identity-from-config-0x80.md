# The identity address: the BlueNRG-2's config 0x80, as a static random address

- Status: Accepted
- Date: 2026-11-27 19:37 UTC
- Decided by: agent

## Context
The BlueNRG-2 has no public address (hardware.md, 2.5.1). Without an
identity address NimBLE makes up a random static one at each start, so the
watch's address changed at every boot, which bonding does not survive.
`aci_hal_read_config_data(0x80)` returns six bytes, on this watch
`ce 45 ec e6 ce 01`, least significant first (`01:ce:e6:ec:45:ce`). The
stock firmware reads the same offset and requires the top two bits of the
last byte set; here they are clear: ST's stack, which does not run in
link-layer-only mode, makes the address of these bits (inferred).

## Decision
The identity address is the 48 bits at config 0x80 with the two top bits
set, as the Core specification defines a static random address (Vol 6,
Part B, 1.3.2.1): `C1:CE:E6:EC:45:CE` on this watch.
`NIMBLE_ID_ADDR_BLUENRG2` gives it to the host, which sets it as its
random address; a read that fails, or a random part of all zeros or all
ones, leaves the host to make one up.

## Consequences
The watch keeps its address across boots and firmware builds, and each
module has its own. Whether it is the address the stock firmware
advertises with is not known.
