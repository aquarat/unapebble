# The GNSS driver on in the watch's firmware, the location service and sport apps still off

- Status: Superseded by 261128-092921-01-standalone-gps-workouts.md (for the location
  service and the sport apps)
- Date: 2026-11-27 19:45 UTC
- Decided by: agent

## Context
261126-130734-02 kept `CONFIG_GNSS`, `CONFIG_SERVICE_LOCATION` and
`CONFIG_APP_SPORT` off on the watch until its sensors had drivers. The GNSS
now has one (261127-194525-02), measured on the watch; the barometer and the
heart rate, which the sport apps also use, are another agent's work, and the
apps themselves have not run on the watch. The driver adds about 10 KiB of
code and 5 KiB of RAM; the debug slot image is 1692 KiB of its 2048.

## Decision
`CONFIG_GNSS` defaults to y on the board, but in the experiments firmware,
with its shell commands wherever there is a shell. The receiver stays off
until something starts it: the `gnss` command for now. The location service
and the sport apps stay off, as 261126-130734-02 has them; switching them on
is the recorder's decision.

## Consequences
Every watch build carries the driver, which costs nothing while the receiver
is off (the board's draw measured at 0.8 mA with it off, as before). The
part of 261126-130734-02 about `CONFIG_GNSS` is replaced; the rest stands.
