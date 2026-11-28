# An activity's time from the GNSS when the receiver knows it

- Status: Accepted
- Date: 2026-11-28 09:29 UTC
- Decided by: agent

## Context
Nothing sets the watch's RTC without a phone; it keeps what the user or the
stock firmware set, and may be wrong by minutes or, after a dead battery,
years. FIT timestamps are UTC, and services place and de-duplicate
activities by them. The receiver reports UTC once it has decoded it from a
satellite, before a fix; the driver drops its pre-2020 defaults.
261127-194525-04 proposes setting the RTC from it, which would move the
clock under a running workout_service, which times workouts on the RTC.

## Decision
The recorder writes a time record with the GNSS's UTC second and the RTC's
at the first epoch whose time agrees, to the second, with the epoch before
it plus one, and again every 10 minutes. The FIT encoder takes the
activity's UTC from the last time record (GNSS time less the record's
offset from the start), and the RTC's start where there is none, which the
listing marks. The RTC itself is not set here: 261127-194525-04 stays
proposed.

## Consequences
An activity with any GNSS time is placed right whatever the RTC says, and so
are its records. Without any, its times are the RTC's. The file names keep
the RTC's start, which is only a name.
