# Limit the USB input to 255 mA

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
The PCA9420's VIN current limit defaults to 425 mA; the stock firmware sets
255 mA (222 mA minimum, `Pmic.cpp:134`).

## Decision
VIN_ILIM 255 mA, as the stock firmware. The other TOP_CNTL settings (input
UVLO, ASYS source, power-down, watchdog) are left as they are.

## Consequences
The 100 mA charge and the watch's load fit under it; a load peak beyond it is
taken from the battery. Any USB port can supply it.
