# Set the clock from the GNSS, but never under a running workout

- Status: Accepted
- Date: 2026-11-28 10:39 UTC
- Decided by: agent

## Context
261127-194525-04 proposed setting the RTC from the GNSS's time, from the
location service, and left it open. Recording workouts on the watch showed
what an unset clock costs: the watch here reads January 2010, and PebbleOS's
activity service, which counts steps and gives the Sport app its cadence,
waits for a time after 2020 before it starts, so with no phone it never
does; the Workout and Sport apps then also faulted on the uninitialised
service (fixed separately). The Sport app's journal keeps the GNSS's time of
its own (261128-092921-05), but the workout_service it runs alongside times
the workout on the RTC, and would see a step in the clock as minutes or
years of workout. Supersedes 261127-194525-04.

## Decision
The location service (`CONFIG_SERVICE_LOCATION_SET_CLOCK`) sets the clock,
with `clock_set_time()` and its time change event, from the receiver's UTC
when it differs from the clock by more than 2 s: from an epoch with a fix, or
one whose time follows the epoch before by a second, the driver having
dropped the receiver's pre-2020 defaults. At most once an hour, from the
system work queue, and not while workout_service has a workout running. A
phone setting the time later sets the same UTC.

## Consequences
A watch with any GNSS use gets the right time: on a sport's ready screen
before the workout starts, which also starts the activity service, so steps
and cadence come with it. A clock that the receiver's time would correct
during a workout waits until it ends. The time zone still comes from
elsewhere. A receiver with a wrong time would set a wrong clock: the checks
are those the proposal gave.
