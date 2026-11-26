# EMMC_EN is the eMMC driver's: the board no longer drives it at boot

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: agent

## Context
The board's device is the last to come up (`.i_devices` of B's ELF:
`s_board` after `dt_emmc_storage`). By then the NOR window had powered the
card and identified it; the board then drove EMMC_EN low, cutting the card
while the MMC class still took it for powered, the SDMMC pins still in their
default state with their pull-ups feeding it. The first access of every boot
failed and relied on the class's retry after a power cycle; a retry that
fails too ends in the flash class's assert.

## Decision
The board drives EMMC_EN only without the SDMMC driver, which configures it
inactive at its own init.

## Consequences
A candidate for B's loop, tested two ways: the Lab's "eMMC supply cut"
repeats the cut under the driver; image F (fix and breadcrumbs) against
image E (breadcrumbs only).
