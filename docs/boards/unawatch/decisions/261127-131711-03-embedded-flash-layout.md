# Embedded flash: 2176 KiB of code, then 1536 KiB of resources

- Status: Accepted
- Date: 2026-11-27 13:17 UTC
- Decided by: agent

## Context
The resources, 885 KiB today, go back into the embedded flash
(261127-131711-02-resources-read-in-place), which holds no storage any more.
The bootloader takes an image of up to 3712 KiB from `0x08060000`, the flash
size less 384 KiB. Release code is about 1.6 MiB. The resources may be placed
at a fixed partition or right after the code; the first port had them at
`0x08200000`, which leaves code 1664 KiB, less than a debug build is about to
need. Supersedes 261126-172141-06-code-partition-and-image-limit, and
261126-172141-05-emmc-partition-layout for the resource banks.

## Decision
A fixed partition: `code` from `0x08060000` to `0x0827ffff`, 2176 KiB, and
`system-resources` from `0x08280000` to the end of the flash, 1536 KiB, both
in `pebble/boards/unawatch.overlay`. The board's devicetree keeps `code` as
the whole rest of the flash, which the bootloader installs to. The image is
`pebbleos_resources.bin`: the code, erased flash up to `0x08280000` and the
resources. `tools/una/pack_ota.py` now takes up to 3712 KiB by default;
`--max-size 0x1a0000` still checks an image for a 2 MiB part. The slot image of
the tether stub links from `0x08080000` up to the same resources, 2048 KiB.
On the eMMC the two resource banks, its first 8 MiB, become free space; the
other partitions keep their offsets, and so what they hold. The experiments
firmware keeps the first port's layout.

## Consequences
About 500 KiB of room for code and 650 KiB for resources. Every `.ota` is about
3 MiB, the gap included, whatever the code's size: the bootloader writes a
little more on each install. Moving the boundary later is a devicetree
change, with nothing on the watch to migrate.
