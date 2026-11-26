# Measure the battery from a full charge to the low battery power-off and back

- Status: Accepted
- Date: 2026-11-26 19:08 UTC
- Decided by: agent

## Context
The owner asked for an on-watch test of the cell's capacity at the 4.20 V cap
(261126-185344-01): charge to full, discharge to the firmware's own power-off
(261126-173552-15), recharge on the next boot. The cell's 200 or 280 mAh is
unknown (hardware notes, open question 8).

## Decision
Four phases, kept across resets in a state file: charge, until the charger
ends its charge (the PCA9420 reports done, after the 14 mA termination and the
top-off); full, until unplugged; discharge, until plugged in again; recharge,
until the charger is done again. The charge is counted out of the battery
from the last sample at full, which the discharge starts again from at the
unplug: the battery stands idle in between. The discharge ends with the last
sample before it was plugged in, the last before the power-off, and counts as
complete if that sample had the battery empty (below 1% or at 3.35 V). The
recharge is counted from there, so it includes what the watch used while off.
The full detection is the charger's, not the gauge's: the gauge's FullCapRep,
FullCapNom and cycles at the second full are shown next to the counted charge,
as what it learned.

The test only reads the gauge and the charger: it never changes the charge
settings or enables charging, so the 4.20 V cap, the temperature window
(261126-173552-10) and the failure behaviour (261126-173552-14) hold
throughout.

## Consequences
Plugging in during the discharge ends it, marked incomplete. The result is
the usable capacity from 4.20 V to the 3.35 V power-off, not the cell's rated
one, and depends on the load (VEmpty's voltage drop).
