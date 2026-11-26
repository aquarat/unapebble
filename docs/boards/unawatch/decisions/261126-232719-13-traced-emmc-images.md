# Two traced eMMC images: E as B, F with the EMMC_EN fix

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: agent

## Context
When an eMMC build loops, the Lab, installed next, can show where it stopped
if that build left breadcrumbs.

## Decision
`CONFIG_BOOT_TRACE_BOOT_PATH` adds breadcrumbs to main()'s stages, the MMC
class and driver, the NOR window, PFS, the resources and the battery drivers.
E is main's eMMC configuration with them, before the EMMC_EN fix (image 2,
`emmc-b`); F is the same after it (image 3, `emmc-fix`). Both are debug
builds, with the battery drivers as main has them. They are built with
`-DCONFIG_BOOT_TRACE=y -DCONFIG_BOOT_TRACE_BOOT_PATH=y` and the image's name
and number.

## Consequences
E loops as B if the cause is in the eMMC path, and the Lab then shows where.
F booting where E loops points at the supply cut. The battery drivers are a
difference from B; their init has breadcrumbs too.
