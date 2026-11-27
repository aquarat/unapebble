# Cap the charge through the battery's charge voltage, so the gauge takes 4.20 V as full

- Status: Superseded by 261127-131711-05-gauge-model-for-4v36
- Date: 2026-11-26 18:53 UTC
- Decided by: agent

## Context
The 4.20 V cap (261126-185344-01) could be set as the battery's
`constant-charge-voltage-max-microvolt` or with
`CONFIG_CHARGER_PCA9420_CHARGE_LIMIT_MV=4200`
(261126-173552-12). The limit option is for stopping short of full: it raises
the gauge's FullSOCThr to 95% so the gauge does not learn the early end as the
capacity, and keeps ModelCFG.VChg = 1 (a cell charged above 4.25 V). Charged
to 4.20 V on that model, the gauge would read about 85% at the end of each
charge, never detect it as full, and not learn the capacity; the battery test
could not compare its own count with what the gauge learned. Maxim sets VChg
by the charge voltage: 0 for 4.2 V, 1 above 4.25 V (UG6597).

## Decision
The devicetree's charge voltage is 4.20 V (VBAT_REG code 0x1E). The gauge
model follows from it: VChg = 0, FullSOCThr stays at Maxim's 80%, so the
end of a 4.20 V charge is the gauge's full, 100%, and FullCapRep learns the
capacity to 4.20 V. The limit option stays off, for a future maintainer choice of
stopping short of full. 4.20 V is the nominal setting: with the charger's
0.75% it may regulate at up to 4.23 V, within the usual 4.20 V +/- 50 mV of a
standard cell.

## Consequences
The gauge's model registers change (ModelCFG 0x0000 instead of 0x0400): on the
first boot of this firmware the gauge refreshes its model, keeping what it
learned, and converges on the new full over a cycle or two. The stock firmware
finds a model that is not its own and reloads it, resetting what was learned,
and so does PebbleOS's next refresh after a switch back. Recharge, 240 mV
below, starts at 3.96 V.
