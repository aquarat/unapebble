# The Lab writes to the eMMC only in its GUI region

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: maintainer (rule); agent (blocks)

## Context
The update drive, SYS and the user drive belong to UNA's firmware and
bootloader. The eMMC builds keep their storage in GUI's first 32 MiB
(sectors 409600 to 475135).

## Decision
Every write of the Lab goes through one check: sectors 409600 to 614399 only.
Its write tests use its own scratch blocks at 600000, past the eMMC builds'
window, and restore what they overwrite; the NOR test erases 128 KiB there.
The only write into the eMMC builds' window is the resource copy into their
bank 0, on demand. Reads of the update drive go through `fat_ro`, read only.

## Consequences
The Lab leaves what B wrote (logs, filesystem, core dumps) readable, and
leaves UNA's data alone.
