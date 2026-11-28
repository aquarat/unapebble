# Set the RTC from the GNSS time

- Status: Superseded by 261128-103913-01-clock-from-gnss-outside-workouts.md
- Date: 2026-11-27 19:45 UTC
- Decided by: agent

## Context
The watch has no phone connection yet (Bluetooth runs on the fake
controller), so nothing sets its clock: it keeps what the RTC has, from the
stock firmware or a manual setting. The receiver gives UTC with each RMC
once it has decoded the time from a satellite, before a position: indoors on
the watch, it had the time 3.5 to 4.5 minutes after power-up, with two to
four satellites in view and no fix. Before that it reports 1980-01-05 or
-06, which the driver drops. The time of an RMC is that of its epoch, which
the sentence follows by the length of the burst; that is far finer than the
RTC's second.

## Decision
Proposed: when an epoch carries a time and either a fix or the same time
from two consecutive epochs, and the RTC differs from it by more than 2 s,
set the RTC (`pbl_realtime_set`) from it, at most once an hour, from the
location service rather than the driver, and log the step. Not when the
phone sets the time, once it can: the phone's would win.

## Consequences
A watch that has been out of charge comes back to the right time after any
GNSS use, the sport apps' runs among them. A wrong receiver time would set a
wrong clock: requiring a fix or two agreeing epochs, and a date after 2020,
guards against its defaults. The time zone still comes from elsewhere.
