# Leave the PMIC's rails, modes, ship mode and watchdog alone

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
The PCA9420 also supplies the system rails, which the board runs on as they
come up. PebbleOS powers off through the bootloader (backup register 3). The
PCA9420 has a ship mode and a watchdog that can turn charging off when not
kicked, but it resets the system (SYSRSTn) when it expires.

## Decision
The charger driver writes only the charger registers, CHG_CNTL7's THM_REG,
TOP_CNTL0's VIN_ILIM and the interrupt masks (the stock firmware's values,
VIN and CHG_OK). No mode, rail, ship mode or watchdog writes. The ON key
long-press time is left as the stock firmware set it.

## Consequences
The boot sequence and the rails are as before. Power-off stays the
bootloader's; a watch off on its charger charges as the PCA9420 was last
configured.
