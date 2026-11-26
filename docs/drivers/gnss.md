# GNSS receiver

A GNSS receiver reports one fix per navigation epoch while started, 1 Hz in
practice, to the listener a consumer registered, the location service
(`pebble/services/location/`). Epochs without a position are reported too,
so the consumer can tell the receiver is searching. Protocol handling, such
as NMEA or a vendor's binary protocol, stays in the driver: the class only
sees decoded fixes (`struct pbl_gnss_fix`: position, altitude, speed,
course, HDOP, accuracy, satellites, UTC time, fix quality).

| | |
| --- | --- |
| Directory | `drivers/gnss` |
| Header | `pbl/drivers/gnss.h` |
| Kconfig | `CONFIG_GNSS`, enabled by the board |
| Role | `GNSS` |
| API | {apidoc}`API reference <drivers_gnss>` |

A driver implements three operations: `init` (optional, leave the receiver
powered down), `start(interval_ms)` (power up and report every epoch with
`pbl_gnss_report_fix()`, holding the device lock) and `stop`.

## Drivers

| Driver | Compatible | Kconfig |
| --- | --- | --- |
| Simulator | | `CONFIG_GNSS_SIM` |

The simulator replays a built-in hilly loop of about 3.1 km at running
pace, with a stop, after `CONFIG_GNSS_SIM_TIME_TO_FIX` seconds of searching.
With `CONFIG_GNSS_SHELL`, `gnss signal 0|1` takes the signal away and
`gnss hold 0|1` stands still, for testing auto-pause and the fallbacks.
