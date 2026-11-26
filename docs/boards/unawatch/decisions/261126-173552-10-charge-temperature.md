# Allow charging only between 0 and 45 degrees C, from the gauge

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
Charging a lithium cell below 0 degrees C plates lithium, and above 45 degrees
C ages it fast. The PCA9420's JEITA control needs a thermistor on its TS pin;
the stock firmware turns it off "to detect the battery" (`Pmic.cpp:94`), so
there is none. The stock firmware only stops charging above 45 degrees C
ambient (barometer) or 55 degrees C at the gauge, and never for cold
(`ChargeThermalMonitor.hpp:122-127`). The MAX17262's temperature is its own die,
beside the cell.

## Decision
NTC_EN stays off. The battery service allows charging only while the gauge
reads 0 to 45 degrees C: it stops at 0 or 45 and resumes at 3 or 42 degrees C.
Charging starts off at boot and waits for a reading. Three failed readings in
a row count as an unknown temperature and stop it. The decision is applied at
every update, every 30 s while plugged in, and the shell's forced enable
cannot override it (`battery_charger_set_inhibited()`). The PCA9420's own die
regulation is lowered to 80 degrees C (THM_REG), the lowest setting.

## Consequences
Stricter than the stock firmware. The gauge die reads the board, which the
charger warms: in a hot place charging may pause before the cell itself
reaches 45 degrees C, which errs on the safe side. While PebbleOS does not run
(off, or in the bootloader), only the PCA9420's own limits apply, as with the
stock firmware.
