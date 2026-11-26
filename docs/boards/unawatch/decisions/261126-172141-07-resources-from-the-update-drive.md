# Resources copied from the update drive when their version is missing

- Status: Accepted
- Date: 2026-11-26 17:21 UTC
- Decided by: agent

## Context
261126-170227-03-resources-on-emmc moves the resources to the eMMC and leaves
how they get there to this decision. The bootloader only installs the code. An
`.ota` carrying the resources for the firmware to copy would still need 2.5
MiB, more than a 2 MiB part takes; the bootloader checks the whole file and
deletes it, so nothing can ride after the image either. There is no Bluetooth
yet for PebbleOS's own resource updates. The firmware checks its resources'
version and CRC at boot, and without them reboots into a recovery firmware the
watch does not have, in a loop.

## Decision
The user copies the release's `.pbpack` next to the `.ota` on the update
drive. At boot, when neither resource bank holds the version the firmware was
built with, it mounts the update drive read-only (`lib/fat_ro`: exFAT, as UNA
formats it, and FAT32 or FAT16, as a computer might, root directory only, with
or without an MBR), takes the first file whose manifest has that version,
whatever its name, copies it into the bank that is empty or holds the oldest
version, and checks it as any bank. Matching on the manifest instead of the
name saves parsing long names, and picks the right pack among several. With no
matching resources the firmware logs the version it needs and powers off,
rather than resetting into a recovery firmware.

## Consequences
Installing takes two files instead of one, and the first start of a new
version a few seconds more. Installing the same build again, or going back to
the previous one, copies nothing. The reader costs about 2 KiB of code; the
image is code only and fits a 2 MiB part. A missing or wrong `.pbpack` turns
the watch off after the splash; copying the right one recovers it.
