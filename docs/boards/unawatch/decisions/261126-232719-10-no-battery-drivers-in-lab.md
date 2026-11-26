# No battery drivers in the Lab; its battery chips read raw

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: agent

## Context
The charger and gauge drivers are on main, not in A, and have not run on a
watch. The Lab must boot first of all. Without the charger driver, the PMIC
keeps the charge voltage the last firmware set: 4.40 V after the stock
firmware.

## Decision
The Lab has neither driver (A's charger stand-in). Its Battery experiment
reads the MAX17262 and the PCA9420 over I2C1 without writing: voltage,
currents, state of charge, temperature, and the charge voltage and current
the PMIC holds.

## Consequences
As with A, the watch should not be left charging on the Lab. The Battery
experiment shows what the PMIC would charge to. The eMMC images (E, F) carry
the drivers as main does, charging to 4.20 V.
