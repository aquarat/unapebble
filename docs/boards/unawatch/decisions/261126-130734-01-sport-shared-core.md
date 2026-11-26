# Sports share one activity core, with a thin profile per sport

- Status: Accepted
- Date: 2026-11-26 13:07 UTC
- Decided by: owner (on-watch sensors, more sports later); agent (structure)

## Context
The owner asked for a Garmin-style running app on the watch's own sensors,
not the phone's, and said cycling and other sports should follow, leaving the
structure open. Recorded after the fact.

## Decision
`pebble/apps/system/sport/` has a shared engine (timer, laps, auto-pause,
distance, pace and speed), data fields, sensor input, a recorder and the app
shell. Run and Bike are profiles of about 65 lines each. Positions come from
a new on-watch location service on a GNSS driver class.

## Consequences
A new sport is a profile. Sensor drivers feed the core once, and every sport
gets them.
