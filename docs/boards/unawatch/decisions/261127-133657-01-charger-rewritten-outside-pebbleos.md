# Find what sets the charge voltage to 4.40 V outside PebbleOS

- Status: Accepted
- Date: 2026-11-27 13:37 UTC
- Decided by: owner (keep re-applying 4.36 V at boot); agent (finding)

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
PebbleOS keeps applying its own 4.36 V at every boot, as the charger driver
already does (written with charging off, read back, and checked on every
poll); the owner judged that the safer course. The bootloader's I2C writes
to 0x61 are traced for interest, to confirm what sets 4.40 V.

## Consequences
A watch left in the stub or the bootloader menu while plugged in charges to
4.40 V, as with the stock firmware; PebbleOS returns it to 4.36 V when it
starts.

## Finding (2026-11-27, from the bootloader's disassembly)
Confirmed: bootloader 0.1.4 rewrites the charger on every boot, in
`Pmic::init` (BL@0x08003D6C, called at BL@0x08001786), before the power-off
check and the menu, with or without USB ({doc}`../hardware`, section 9.6).
Between an unlock and a lock of CHG_LOCK, read-modify-write: CHG_CNTL5 bits
5:0 to 0x28, 4.40 V, bit 6 (VBAT_RESTART, 240 mV) kept, which makes 0x66
0x68 (BL@0x08003DB6); CHG_CNTL6 bit 7 (NTC_RES_SEL) cleared, the timers
kept, which makes 0x95 0x15 (BL@0x08003D9C); CHG_CNTL1 to 0x14, 100 mA,
PebbleOS's value too (BL@0x08003DAC); CHG_CNTL0 NTC_EN cleared; TOP_CNTL3
bits 1:0 to 3. CHG_CNTL2 to 4 are never written. 0x15 and 0x95 differ only
in the thermistor bias, unused with NTC off.

Before those writes (BL@0x08003D1E), with no charger input, CHG_CNTL6 bit 7
set and no reset cause latched in RESET_MON (0x71 bits 4:0), the bootloader
puts the PMIC in ship mode, taking bit 7 for a PMIC at its power-on
defaults. PebbleOS writes bit 7 set (0x95), so a reset on battery that the
PMIC does not latch may leave the watch looking dead until USB or a press
of the power button (inferred, not tried). Writing CHG_CNTL6 with bit 7
clear (0x15) would avoid it; that is for the owner to decide.
