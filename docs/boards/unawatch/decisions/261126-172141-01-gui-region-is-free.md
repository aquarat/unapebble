# Rely on the eMMC GUI region: the bootloader leaves it alone

- Status: Accepted
- Date: 2026-11-26 17:21 UTC
- Decided by: agent

## Context
261126-170227-02-storage-on-emmc puts the storage in the GUI region (sectors
409600 to 614399) once the bootloader binary is checked for any access to it.

## Decision
The check found none, so the storage goes there. Bootloader 0.1.4 reaches the
eMMC only through its `flash_diskio`, and only for drives 0 (UPD) and 1 (SYS):
FatFs maps no other drive, USB mass storage is given drive 0, the eMMC object
is referenced nowhere else, and the SDMMC erase commands are not linked in. The
addresses are in hardware.md, section 3.4.

## Consequences
PebbleOS data survives installs and a round trip to the stock firmware. A
future bootloader or stock firmware that starts using GUI would destroy it;
other bootloader versions are unchecked.
