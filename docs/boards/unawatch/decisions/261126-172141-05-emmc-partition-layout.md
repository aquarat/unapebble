# Storage layout in the first 32 MiB of the GUI region

- Status: Accepted
- Date: 2026-11-26 17:21 UTC
- Decided by: agent

## Context
100 MiB of GUI region are available (261126-172141-01); Pebbles used 4 to 32
MB of NOR in all. PFS scans every 4 KiB page header at boot, one block read
each, and keeps a byte of RAM a page. The flash log takes fewer than 256 pages
of 4 KiB. A core dump holds the 2424 KiB RAM region.

## Decision
The flash is a 32 MiB window from 200 MiB into the eMMC, the rest of the
region left free for later. In it: two system resource banks of 4 MiB (the
resources are 0.9 MiB), `safe-firmware` (only protected: there is no recovery
firmware), `mfg-info`, `tzinfo` and `shared-prf-storage` of 8 KiB each as
before, `scratch` of 4 MiB for one core dump, `debug-db` of 512 KiB, the most
the flash log takes, and the filesystem of 16 MiB. Every partition is in whole
subsectors, the filesystem in whole sectors. The embedded flash keeps no
storage: its driver is off for the board, and the code partition takes the
rest of it (261126-172141-06).

## Consequences
Boot scans 4096 PFS pages, a fraction of a second. Growing a partition later
means moving the ones after it, which loses their data, unless it takes the
free space after the window.
