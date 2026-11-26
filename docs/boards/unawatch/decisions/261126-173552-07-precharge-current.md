# Pre-charge at 8 mA and 4 mA

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
Below VBAT_LOW (2.5 V) the PCA9420 pre-charges at ICHG_LOW, below VBAT_DEAD
(1.9 V) at ICHG_DEAD, defaults 8 mA and 4 mA, which the data sheet says are
MTP-programmable, so they may differ on the part. The stock firmware leaves
them.

## Decision
Write the data-sheet defaults explicitly: ICHG_LOW 8 mA (0.04C), ICHG_DEAD 4
mA, dead timer 5 s, each capped by the one above it.

## Consequences
A deeply discharged cell is brought back gently, whatever the part's MTP
holds.
