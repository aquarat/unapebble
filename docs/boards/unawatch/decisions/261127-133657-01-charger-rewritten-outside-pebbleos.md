# Find what sets the charge voltage to 4.40 V outside PebbleOS

- Status: Proposed
- Date: 2026-11-27 13:37 UTC
- Decided by: agent

## Context
A one-file build (`55b699bf6`, slot build) ran 301 s on the watch with no
fault and restarted into the tether stub as planned. PebbleOS's charger
driver had written CHG_CNTL1 to 6 and read them back without error (no
`CHG_CNTL` errors in the flash log). Read over I2C from the stub
afterwards, CHG_CNTL1 to 4 still held PebbleOS's values (0x14 0x0e 0x08
0x04: 100 mA, 14 mA, 8 mA, 4 mA), but CHG_CNTL5 read 0x68 (VBAT_REG
4.40 V) where PebbleOS writes 0x66 (4.36 V), and CHG_CNTL6 0x15 where it
writes 0x95. While plugged in, the gauge read 4.399 to 4.404 V.

So something between PebbleOS's reset and the stub sets the charge
voltage back to 4.40 V, most likely the stock bootloader on every boot.
4.40 V is what the stock firmware charges to, so it is not outside the
cell's use so far, but the 4.36 V of
261127-131711-04-charge-to-4v36 only holds while PebbleOS runs.

## Decision
Find the writer: the bootloader's I2C writes to 0x61 (disassembly), and
the registers read while PebbleOS runs (through the developer USB access
once it exists) and in the stub after a cold power-on. Then decide whether
the stub should also set 4.36 V, and how to treat the bootloader's value.

## Consequences
Until then, a watch left in the stub or the bootloader menu while plugged in
charges to 4.40 V, as with the stock firmware.
