# Use TAMP backup registers 24 to 29 for PebbleOS's retained state

- Status: Accepted
- Date: 2026-11-26 13:06 UTC
- Decided by: agent

## Context
PebbleOS keeps a few values across resets in RTC backup registers. The stock
firmware and bootloader use registers 0 to 23 (DR3 is the bootloader's
power-off flag). Recorded after the fact.

## Decision
PebbleOS uses registers 24 to 29 only.

## Consequences
Switching between PebbleOS and the stock firmware doesn't confuse the
bootloader.
