# Shut down below 1% or at 3.35 V

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
PebbleOS's battery monitor enters low power mode at
`SERVICE_BATTERY_LOW_POWER_PERCENT` (2%) and shuts down at 0%. The stock firmware
powers off below 1% RepSOC, since the gauge may stall around 0.2%, or at an
averaged 3.35 V for a gauge whose SOC never falls (`Power.hpp:286-299`).

## Decision
Unplugged, the battery service reports 0% below 1% or at an averaged 3.35 V
or less, the averaged voltage so a load spike does not count. The battery
monitor then shuts the watch down after its 30 s.

## Consequences
The same floor as the stock firmware, above VEmpty's 3.40 V under load and
well above the PCA9420's ASYS UVLO (2.7 V), so the cell is not
over-discharged.
