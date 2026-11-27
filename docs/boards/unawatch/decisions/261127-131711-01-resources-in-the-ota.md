# One .ota again: the system resources come with the image

- Status: Accepted
- Date: 2026-11-27 13:17 UTC
- Decided by: maintainer

## Context
261126-170227-03-resources-on-emmc moved the resources to the eMMC so that
the image would fit a 2 MiB part, which was not ruled out, and
261126-172141-07-resources-from-the-update-drive had the user copy a
`.pbpack` next to the `.ota`. The watch has since been confirmed to carry
the 4 MiB STM32U5A5, and the two-file install proved fragile on it: the
`.pbpack` went missing from the drive and the firmware powered off at boot.
Supersedes 261126-170227-03-resources-on-emmc. The maintainer chose to carry
everything in the `.ota` again.

## Decision
The `.ota` carries the system resources with the code, as the first port's
did (`CONFIG_RESOURCES_IN_IMAGE`, `pebbleos_resources.bin`, packed with
`--max-size 0x3a0000`). The storage stays on the eMMC: the filesystem, the
flash logs and the core dumps. How the firmware reads the resources is in
261127-131711-02-resources-read-in-place.

## Consequences
Installing is one file again. Images need the 4 MiB part, which the watch has.
Settings, apps and logs still survive installs.
