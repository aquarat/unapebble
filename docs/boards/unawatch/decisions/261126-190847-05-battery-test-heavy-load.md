# A heavy load of the frontlight and a duty-cycled busy loop, in the app only

- Status: Accepted
- Date: 2026-11-26 19:08 UTC
- Decided by: agent

## Context
The heavy discharge needs a known, roughly steady load that cannot hang the
watch, trip its watchdogs or hold off the low battery power-off. The app
thread runs above the background work queue, which the battery service and
the power-off run on, and the task watchdog throttles an app that starves it.

## Decision
While the Battery test app is in front, discharging, unplugged and above the
low power percentage, the frontlight is on and the app's thread spins 40 ms in
every 50 ms, from an app timer, so the queue and the buttons get the rest. It
stops when the app is left, at the low power percentage, or when plugged in.
No vibration: it adds little steady load and wears the motor.

## Consequences
The low power mode at 2% and the power-off below 1% run as usual, at a lighter
load. A discharge where the app was left reads as a mixed load.
