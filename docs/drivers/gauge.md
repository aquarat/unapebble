# Fuel gauge

A fuel gauge is a chip that measures the battery and estimates its state of
charge itself. A reading gives the state of charge, the voltage and its
average, the current and the temperature. The battery service takes the
state of charge from it with `CONFIG_BATTERY_SOC_GAUGE`, the default on a
board with a gauge, and allows charging only while its temperature is between
0 and 45 degrees C.

| | |
| --- | --- |
| Directory | `drivers/gauge` |
| Header | `pbl/drivers/gauge.h` |
| Kconfig | `CONFIG_GAUGE`, enabled by the board |
| Role | `GAUGE` |
| API | {apidoc}`API reference <drivers_gauge>` |

## Drivers

| Driver | Compatible | Kconfig |
| --- | --- | --- |
| Maxim MAX17262 | `maxim,max17262` | `CONFIG_GAUGE_MAX17262` |
