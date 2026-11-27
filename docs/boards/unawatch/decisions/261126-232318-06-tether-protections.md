# What the stub refuses

- Status: Accepted
- Date: 2026-11-26 23:23 UTC
- Decided by: maintainer

## Context
The bootloader and UNA's libc below `0x08060000` are the way back to the
stock firmware and the recovery menu; the option bytes can make the chip
unusable; the PMIC's settings decide how the cell charges.

## Decision
The stub erases and programs only the slot, from `0x08080000`, and only on
a 4 MiB part, honouring SWAP_BANK; writes by poke or fill to the code region
below `0x10000000`, the flash controller, its own RAM and the USB controller
are refused. It never writes the option bytes or the PMIC. I2C writes beyond
a register address to the PMIC, the fuel gauge and the USB-PD controller are
refused unless the request says unsafe. Experiments are read-only by default,
and may write the eMMC only within the GUI region, sectors 409600 to 614399.

## Consequences
No command can make the watch unrecoverable through the bootloader menu.
Experiments run privileged and could still do anything: the rules for them
are conventions, checked in review.
