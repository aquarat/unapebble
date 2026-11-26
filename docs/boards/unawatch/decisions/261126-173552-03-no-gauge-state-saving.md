# Do not save the gauge's learned registers

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
UG6595 suggests saving the learned registers (RComp0, TempCo, FullCapRep,
Cycles, FullCapNom) and restoring them after a power-on reset of the gauge.

## Decision
Not done. The gauge keeps them itself as long as it has the battery; it only
loses them when the battery is removed or completely drained. After a battery
replacement the saved values would describe another cell, and PebbleOS's
filesystem is emptied by every install anyway.

## Consequences
After a battery removal the gauge relearns the cell over a few cycles, as with
the stock firmware. No storage or flash is spent on it.
