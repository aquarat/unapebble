# Keep the sport apps off on the watch until its sensors have drivers

- Status: Superseded by 261127-194525-03-gnss-on-the-watch-sport-still-off.md (for `CONFIG_GNSS`)
- Date: 2026-11-26 13:07 UTC
- Decided by: agent

## Context
The sport apps need GNSS (AG3335M), and benefit from the barometer and heart
rate, none of which have drivers yet. Code flash is also nearly full.
Recorded after the fact.

## Decision
`CONFIG_APP_SPORT`, `CONFIG_GNSS` and `CONFIG_SERVICE_LOCATION` default to n.
They are on for the QEMU and native emulator boards, with a simulated GNSS.

## Consequences
The apps are tested in the emulator only. Turning them on for the watch waits
for the GNSS driver.
