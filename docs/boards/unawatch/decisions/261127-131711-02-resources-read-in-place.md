# Read the resources in place in the embedded flash

- Status: Accepted
- Date: 2026-11-27 13:17 UTC
- Decided by: agent

## Context
With the resources in the `.ota` (261127-131711-01-resources-in-the-ota), the
bootloader writes them to the embedded flash with the code. The firmware
could read them there, as the first port did, or copy them once into a
resource bank on the eMMC and read them from there, as the builds with the
`.pbpack` did. Supersedes
261126-172141-07-resources-from-the-update-drive.

## Decision
Read them in place. `CONFIG_RESOURCES_IN_CODE_FLASH`, on when the devicetree
has a `system-resources` partition and the resources are in the image, gives
the system resource store one bank, that partition of the memory-mapped
flash the code runs from: reads are copies from memory, the check is the
legacy CRC over memory, and pointers into it are served as read-only bytes
(`CONFIG_MMAP_RESOURCES`, selected), the bank an MPU region readable by apps.
The flash role, the eMMC, keeps no resource bank. Nothing writes the bank:
put bytes and the console's imaging get no bank to write to, and clearing it
does nothing. Without valid resources, as after flashing `pebbleos.bin` alone,
the firmware logs it and powers off, as there is no recovery firmware.
`CONFIG_RESOURCES_FROM_VOLUME` is no longer on for the board: its code stays,
an option, and `lib/fat_ro` stays for the Lab.

## Consequences
Simplest and most robust: no copy and no second place for the resources to
be missing from, no version matching at boot, no eMMC traffic for fonts and
images, the first port's way, which booted on a watch, and zero-copy reads
that save RAM. Resources cannot be updated without the code, which is how
they arrive anyway. The resources take embedded flash that code could use
(261127-131711-03-embedded-flash-layout).
