# Measure the draw with the charger's input suspended

- Status: Accepted
- Date: 2026-11-27 15:06 UTC
- Decided by: agent

## Context
The maintainer asked for the watch's power to be measured, before and after
making PebbleOS use the SoC's low-power modes, without unplugging it: it is
plugged into a Mac, which carries its console and tether port. Plugged in,
the PCA9420 runs the system from VIN, so the MAX17262, which measures the
battery's current, does not see what the system draws.

The PCA9420 data sheet (Rev. 4.2, section 8.3 and table 18) gives TOP_CNTL1
(`0x0A`) bits 5:4, ASYS_INPUT_SEL: 00 ASYS from VBAT or VIN, whichever is
higher (the default), 01 from VBAT only, 10 from VIN only, 11 disconnected
(test only). ASYS is the input of the bucks, LDO2 and, through VBAT_BKUP,
LDO1, and of the linear charger (table 3), so with 01 the whole system runs
from the battery and nothing charges, VIN connected. A power cycle or a reset
of the PMIC restores 00; an MCU reset does not, nor does the bootloader,
which writes only the charger, TOP_CNTL3 and the rails (hardware.md, 9.6).
VIN_ILIM has no suspend setting: its lowest limit is 85 mA.

Tried on the watch: with 01 the gauge read 14 to 16 mA of discharge with the
USB developer access connected, where it read about 1 mA of charge before,
the console and the tether port kept working, and the register and
CHG_CNTL0 read back 00 and charging allowed afterwards.

## Decision
The charger class has an optional `set_input_suspend` operation; the PCA9420
driver implements it by turning charging off (CHG_EN, as when charging is not
allowed) and then setting ASYS_INPUT_SEL to 01, and the reverse. The charge
voltage, currents, timers, temperatures and VIN_ILIM are not touched. The
driver's init writes 00, so any restart of PebbleOS puts the input back.
This adds TOP_CNTL1's ASYS_INPUT_SEL to the registers 261126-173552-16 lets
the driver write; the rails and modes stay untouched.

The `power measure SECONDS [usb] [LABEL]` shell command
(`CONFIG_BATTERY_POWER_MEASURE`, on with the USB developer access) suspends
the input for a window of 1 to 30 minutes, holds the USB developer access
off unless `usb` is given, keeps the gauge out of hibernate (where it
measures only every 5.6 s), averages the gauge's Current and AvgCurrent
every 2 s after 30 s of settling, with the spread of Current, counts its
coulomb counter (QH) and the CPU time per idle state, then restores
everything; `power` prints the last eight results. It refuses below 30% or
outside 0 to 45 degrees C and ends early below 15%. The restart and
power-off handlers end a window first.

## Consequences
The draw can be measured on a plugged-in watch, the USB still connected, and
the console reads the results once the USB comes back. A fault or watchdog
reset in the middle of a window leaves the input suspended until PebbleOS
starts again: in a slot build the stub runs from the battery meanwhile and
does not charge it. The gauge's current is quantized at 156.25 uA, so a mean
below a few hundred microamps rests on the averaging of its noise; QH counts
0.5 mAh and only cross-checks long windows. Plugged in with the input
normal, the battery current swings by several milliamps either way, so the
gauge's offset cannot be read there: the numbers are as the gauge gives
them.
