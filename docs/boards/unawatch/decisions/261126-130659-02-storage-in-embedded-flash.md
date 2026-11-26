# Keep PebbleOS storage in the embedded flash

- Status: Superseded by 261126-170227-02-storage-on-emmc
- Date: 2026-11-26 13:06 UTC
- Decided by: agent

## Context
PebbleOS needs NOR-style flash for its filesystem (PFS), the debug log and
crash dumps. There was no SDMMC driver for the eMMC. Recorded after the fact.

## Decision
These live in the second bank of the STM32U5's 4 MiB embedded flash: small
partitions at 0x08300000, PFS at 0x08310000 (832 KiB), debug-db at
0x083E0000 (128 KiB), behind a write-buffering layer (`ecc_nor`) because the
embedded flash is written in ECC-protected quadwords.

## Consequences
The UNA bootloader erases everything from 0x08060000 to the end of flash on
each install, so all PebbleOS data is lost on every update.
