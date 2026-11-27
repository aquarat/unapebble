# Flash and start PebbleOS from the stub

- Status: Accepted
- Date: 2026-11-26 23:23 UTC
- Decided by: agent

## Context
The maintainer asked for fast PebbleOS iteration over USB if it could be done
cleanly. PebbleOS links at `CONFIG_FLASH_OFFSET`, from the `code` partition,
and sets VTOR from its own vector table; its start-up already resets what a
bootloader leaves running.

## Decision
`CONFIG_BOARD_UNAWATCH_TETHER_SLOT` links PebbleOS at `0x08080000` (3584
KiB), through board defaults of `CONFIG_FLASH_OFFSET` and
`CONFIG_FW_FLASH_SIZE`. `tether.py flash` erases, programs and checks such
an image in the slot; `tether.py boot`, or Up on the watch, starts it as the
bootloader would: USB disconnected, interrupts masked, VTOR and MSP from the
image. No other PebbleOS code changes.

## Consequences
A PebbleOS build can be tried in about 15 s over USB, and the stub stays
installed: every reset comes back to it. Slot images are not `.ota` files.
PebbleOS takes its resources from the eMMC as usual.
