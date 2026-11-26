# Sample the battery test on the battery service's polls

- Status: Accepted
- Date: 2026-11-26 19:08 UTC
- Decided by: agent

## Context
The test needs a sample about a minute apart for days, the watch mostly idle
in STOP2. The battery service already wakes to read the gauge every 60 s
unplugged and 30 s plugged in, and at once on plugging in or out.

## Decision
The battery service's poll hands its reading to the test, which samples when
55 s have passed, or at once when plugging in or out, the empty battery or the
heavy load changes. No timer of its own. A power-off or restart handler, run
first, takes a last sample, so the low battery power-off's 30 s countdown is
logged.

## Consequences
No extra wake-ups; the samples are 60 s apart. A sample costs one more I2C
transfer for the charger's status when plugged in, and a log write.
