# An experiments firmware: the board's lab variant

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: owner (asked for it); agent (form)

## Context
Without a console, finding why B loops takes experiments whose results show
on screen, from a firmware that boots. A boots.

## Decision
`BOARD=unawatch/lab` is a board variant (`unawatch.yml`), whose
`pebble/boards/unawatch_lab.{conf,overlay,roles.yml}` start the firmware as A:
storage in the embedded flash with A's partitions, the code partition back to
1664 KiB, the resources in the image (`CONFIG_RESOURCES_IN_IMAGE`,
`pebbleos_resources.bin`), no NOR window on the eMMC, no battery drivers and
A's clock tree. It adds the boot trace and the Lab app. `BOARD_UNAWATCH_LAB`,
from the variant, turns off what a `.conf` cannot override without Kconfig
aborting on a value assigned twice: the board's defaults
(`boards/unawatch/Kconfig.defconfig`, `pebble/Kconfig`) now set the embedded
flash driver off, the gauge, the resources from the update drive, the
battery test and Moddable XS on, unless it is set. The default build's
configuration is unchanged.

## Consequences
One option builds it: `BOARD=unawatch/lab`. Packed with
`--max-size 0x3a0000`, for 4 MiB parts. What A lacks, the Lab drives on
demand.
