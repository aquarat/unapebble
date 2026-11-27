# The STM32 pin controller takes a port's clock once

- Status: Accepted
- Date: 2026-11-27 19:33 UTC
- Decided by: agent

## Context
The first builds with the BlueNRG-2 transport panicked a few seconds after
Bluetooth came up, in `clk.c` (a use count at 255), the boot trace showed.
`drivers/pinctrl/stm32.c` called `pbl_clk_enable()` on a pin's GPIO port
for every pin it configured, and never disabled it: the port keeps the pin
configured. A driver that selects pin states at each use, as the SPI
driver does (261127-191406-01-stm32-spi-driver) and the eMMC driver does at
each power-up (default, then sleep at power-off), adds a reference each
time, until the count overflows. For the SPI that was about 64 transfers.

## Decision
The pin controller enables a port's clock only when it is not enabled yet.

## Consequences
Pin states can be selected as often as needed. The eMMC driver's switches
no longer count towards the overflow either: at five of its pins on port
C, each power cycle of the card added ten references, so a watch would
have panicked after about 25 power cycles of the eMMC (inferred; not seen
on a watch, whose runs so far stayed short).
