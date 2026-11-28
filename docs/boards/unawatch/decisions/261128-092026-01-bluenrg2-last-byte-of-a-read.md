# The last byte of each SPI read from the BlueNRG-2: one dummy byte more

- Status: Superseded by 261128-100132-01-spi-crcsize-erratum
- Date: 2026-11-28 09:20 UTC
- Decided by: agent

## Context
With the ACI transport, values a BlueZ central wrote came to the host with
their last byte cut to its two low bits: `09` as `01`, `fe 77` as `fe 03`,
`0a` as `02`, in both the attribute-modified and the write-permit events.
Read in two parts, the last byte of the first part was cut instead: the
opcode `fd01` of a Command Complete came as `0101`. At 625 kHz instead of
1.25 MHz, the same two bits. So the last byte of every transfer of the
STM32's SPI (`drivers/spi/stm32.c`) comes back with only its two low bits;
which side of the bus causes it is not known (the driver's end of transfer
is the likeliest, inferred). The tether experiment's bit-banged SPI read
the chip's whole flash back with the updater's CRCs matching, so the chip
sends those bytes right.

It explains three earlier findings:

- Config 0x80, the module's static random address, read `ce 45 ec e6 ce
  01`: its last byte is `0xcd`. The address is `CD:CE:E6:EC:45:CE`, a valid
  static random address as it is, as the stock firmware expects
  (261127-193700-01 set the two top bits of `0x01`, giving
  `C1:CE:E6:EC:45:CE`).
- `aci_gap_get_bonded_devices` listed each address with its most
  significant octet `0x00` (`0x9c` cut), and removing a bond by that
  address did nothing.
- A status as the last byte of a Command Complete was cut too:
  `aci_gap_is_device_bonded`'s "not found" (`0x5c`) came as success.
- Disconnections all came with reason `0x03`: `0x13` cut, and so on.
- The Command Status whose opcode read `0x0016` (261127-194000-01) is
  `0x2016` with its last byte cut.

## Decision
`bnrg_read()` reads one byte more than the chip has, and drops it: that
byte is the one cut. The chip gives no data for it and takes nothing from
its next packet (no framing error in any run since). The 5-byte header's
last byte is the high byte of the length to read, which is never above 3.
Writes are not affected: the chip takes the commands' last bytes right.
Both transports share it (`bluenrg2_spi.c`). The bond addresses are
compared whole again.

## Consequences
- The watch's address changes from `C1:CE:E6:EC:45:CE` to the module's
  own `CD:CE:E6:EC:45:CE`: a central bonded with the old one has to pair
  again.
- The cause in the SPI driver, or in the bus, is still to be found; other
  devices on that driver would see the same.
