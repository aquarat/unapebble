# Report the gauge's capacities and coulomb counter in its reading

- Status: Accepted
- Date: 2026-11-26 18:55 UTC
- Decided by: agent

## Context
The battery capacity test needs what the MAX17262 counts and learns: RepCap,
FullCapRep, FullCapNom, DesignCap, Cycles, AvgCurrent and the coulomb counter
QH. The gauge class reading (261126-173552-01) had only the state of charge,
the voltages, the current and the temperature.

## Decision
`struct pbl_gauge_reading` gains the averaged current, the remaining, full,
nominal full and design capacities, the cycles (in % of a cycle), the raw
16-bit coulomb counter with its uAh per count, and the number of power-on
resets of the gauge the driver has handled since boot, after each of which the
counter starts again. Fields a gauge cannot give read 0. The MAX17262 reads
Status to FullCapNom in one transfer instead of Status to AvgVCell, and QH in a
second.

QH is a capacity register: 5.0 uVh over the sense resistor, the internal
one's 10 mOhm making 0.5 mAh a count, the LSB UNA's own driver uses for it
(`MAX17262.cpp`, `kCapScaler`) and the hardware notes give the capacity
registers. It counts up while charging and is read as a signed 16-bit count
that wraps. Cycles is taken at 1% a count. The data sheet could not be
fetched again to check the QH and Cycles entries themselves; the capacity
test integrates the current in software as a cross-check, which would show a
wrong QH LSB as a factor between the two.

## Consequences
One more I2C transfer and 22 more bytes in each battery poll, every 30 or
60 s. The battery service and the `gauge` shell command see the capacities.
