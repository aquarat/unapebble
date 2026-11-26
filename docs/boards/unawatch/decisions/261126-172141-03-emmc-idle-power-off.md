# Power the eMMC off after 2 s idle

- Status: Accepted
- Date: 2026-11-26 17:21 UTC
- Decided by: agent

## Context
The stock firmware cuts EMMC_EN after 2 s without access, unless USB is
attached. A powered, idle eMMC draws on the order of 100 µA; the watch has a
200 mAh battery. Powering up again takes 35 ms plus identification. The
flash class's `pm_suspend` hooks are for parts that wake in microseconds.

## Decision
The `mmc` class powers the card down once it has had no access for
`CONFIG_MMC_IDLE_OFF_MS`, 2000 by default, from a delayed work item, and up
again on the next access. The card is only powered down in the transfer
state, after its writes are programmed: the NOR layer holds nothing back, so
nothing needs flushing first. Powered down, the SDMMC pins go analog so that
their pull-ups do not feed the card. Restart and power-off power it down
cleanly. In a coredump the card stays powered.

## Consequences
An access after an idle spell takes some tens of milliseconds more. Frequent
small accesses (a watchface's minute update reading a font) may cycle the
card's power once a minute; the timeout is one Kconfig option to tune once
current can be measured.
