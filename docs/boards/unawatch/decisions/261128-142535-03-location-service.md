# A kernel location service: per-task use, an 8-epoch history, no SDK API

- Status: Accepted
- Date: 2026-11-28 14:25 UTC
- Decided by: agent

## Context
Recorded after the fact: decided 2026-11-26 with the GNSS class
(261126-130734-01). Stock PebbleOS has no positioning on the watch. The GNSS
class hands each epoch to one listener on the driver's thread; system apps
poll once a second from their tick, which can land between two epochs or
miss one.

## Decision
`pebble/services/location/` (`pbl/services/location.h`) is that listener. It
keeps the last 8 epochs. Clients read the ones after their cursor
(`location_service_get_cursor()`, `location_service_read()`), so a 1 Hz
poll loses none, and get a status (unavailable, off, searching, fix, the
last fix older than 3 s counting as searching). The receiver runs at 1 Hz
while any task has started the service. Starts are counted per task, and the
process manager stops a task's use when it exits, so a crashed app cannot
leave the receiver on. Subscriptions and the history have separate locks,
so the listener never waits on a task starting or stopping the receiver.
System apps call it directly; there is no syscall or SDK API.

## Consequences
Third-party apps can't use the watch's GNSS. Adding that later is a syscall
and SDK change over this API. Each starting task costs a bit in a bitset, and
the history costs 8 fixes of RAM.
