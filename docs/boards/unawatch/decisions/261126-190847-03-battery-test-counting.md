# Count with the gauge's QH, cross-checked by an integral of its averaged current

- Status: Accepted
- Date: 2026-11-26 19:08 UTC
- Decided by: agent

## Context
The MAX17262 counts charge in QH (0.5 mAh a count, 261126-185538-01) all the
time it has the battery, also while the watch is off. A software integral of
current samples a minute apart is independent of QH's LSB, but misses what
happens between samples and while off.

## Decision
The counted charge is the change of QH, taken sample to sample as a signed
16-bit difference so that it wraps. A software integral of AvgCurrent
(trapezoidal) runs next to it as a cross-check. Over a gap of more than 10
minutes (the watch off, or a clock change) the integral takes QH's change
instead, and the total bridged is reported. After a power-on reset of the
gauge, which restarts QH, the integral stands in for that step and the
results say so.

## Consequences
Both totals are shown; they should agree within a few percent under a steady
load. A factor between them would point at a wrong QH LSB; a bursty load makes
the integral the less accurate one.
