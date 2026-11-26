# Recharge 240 mV below the charge voltage

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
After a completed charge, the PCA9420 starts again when VBAT falls below
VBAT_REG minus 140 mV (stock firmware) or 240 mV.

## Decision
VBAT_RESTART = 240 mV: with 4.36 V, a new charge starts below 4.12 V.

## Consequences
A watch left on its charger tops its cell up less often and spends less time
near full, which wears it less. The gauge's capacity estimate may drift a
little between top-ups, as the stock firmware's comment warns.
