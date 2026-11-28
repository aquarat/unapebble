# ST's update flow tried for the patched DTM: it does not start either; patching stops

- Status: Accepted
- Date: 2026-11-28 00:45 UTC
- Decided by: agent

## Context
After 261128-003746-01-dtm-acl-patch, the coordinator asked, within the
maintainer's approval (261128-001034-01), whether the integrity reference
is made by the updater during an official update, and whether the range
checked, perhaps set by the length at +0x18 of the vector table, leaves
room for another approach.

ST's updater source (version 6, the version the chip reports): setting
the BLUE flag (0xFC25) only writes the word 0x424C5545 at 0x10042014;
erasing it (0xFC24) writes 0; at boot the updater checks only the RAM
flag, the BLUE flag and IO3. Nothing there computes or stores a CRC.

## Decision
Tried on the watch, each time restored by the boot pin (MOSI high) and
checked to start normally (tether experiment bluenrg2, mode 13 added):

- **The official flow:** BLUE flag erased, the DTM's first page erased and
  written with its flag blank, the patched pages written, all read back
  and CRC-checked, the flag set (read back 0x424C5545), restart: **no
  start**, as before.
- **The range:** a word written in blank flash just after the code
  (0x10062cc0) and at the end of that page (0x10062ffc): starts normally,
  as the code page at 0x10068000 did. The length word at +0x18 set to 0,
  with the original code and with the patch: **no start**: the first page
  is covered too.

So the check covers the code and the vector table, length word included,
and nothing after the code; its reference is in no readable flash, and is
not made by the update flow. Every hook point is in the checked code, so
no placement of the added code helps. Writing stops here, as asked.

## Consequences
The chip is on its original DTM, verified by a full read. What remains:
reading the updater or ROM by SWD or the ROM UART bootloader (not to be
opened without the maintainer), or option (b), ST's ACI.
