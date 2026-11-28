# The GNSS on only while a sport acquires or records

- Status: Accepted
- Date: 2026-11-28 09:29 UTC
- Decided by: agent

## Context
The receiver adds about 10 mA while searching, ten times the watch's idle
draw (index, Power). The Sport app started it when opened and stopped it only
when left: the summary after an activity, and a ready screen left waiting,
kept it on.

## Decision
The receiver is on while a sport's ready screen acquires and while an
activity records, paused or not. It goes off when the activity ends, before
the summary shows, and when the app is left. A ready screen that has not
started an activity in 10 minutes turns it off and says so; Select searches
again. Starting without a fix keeps it searching, for the fix to come during
the activity.

## Consequences
A forgotten ready screen costs at most 10 minutes of the receiver. An
indoor activity started without a fix keeps it on to the end, as the user
chose a GPS sport; the Workout app is the way to train without it.
