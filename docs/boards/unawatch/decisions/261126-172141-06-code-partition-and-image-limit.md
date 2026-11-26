# The code partition takes the rest of the flash; images stay within 2 MiB parts

- Status: Accepted
- Date: 2026-11-26 17:21 UTC
- Decided by: agent

## Context
With storage and resources on the eMMC, nothing else uses the second bank of
the embedded flash. The owner wants an image that fits a 2 MiB part and more
flash for code. Whether watches ship with the 4 MiB STM32U5A5 or the 2 MiB
STM32U595 is not confirmed; the bootloader takes an image up to the flash size
less 384 KiB.

## Decision
The `code` partition runs from `0x08060000` to the end of the 4 MiB flash
(3712 KiB), so the link never limits the code below what a 4 MiB part holds.
`tools/una/pack_ota.py` keeps refusing, by default, an image over 1664 KiB, the
limit on a 2 MiB part, and CI packs with the default: a release that outgrows
2 MiB parts fails there rather than reaching a watch that would refuse it.
`--max-size 0x3a0000` packs for 4 MiB parts only.

## Consequences
Release images are 1611 KiB and debug ones 1642 KiB today, 53 KiB and 22 KiB
under the 2 MiB-part limit. Using more means deciding to drop 2 MiB parts.
