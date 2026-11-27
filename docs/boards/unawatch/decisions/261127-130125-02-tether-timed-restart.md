# Restart a tethered test run into the stub after a set time

- Status: Accepted
- Date: 2026-11-27 13:01 UTC
- Decided by: agent

## Context
While PebbleOS runs from the tether slot, the stub and its USB port are gone:
its boot trace and flash log can only be read once the watch resets into the
stub. A run that does not fail never does, and the maintainer, who would have to
press buttons, wants to stay out of the loop. PebbleOS has no button combo
that resets; the stub's Back and Select only work in the stub.

## Decision
`CONFIG_BOARD_UNAWATCH_TETHER_REBOOT_S` (0, off, by default; only with
`CONFIG_BOARD_UNAWATCH_TETHER_SLOT`) makes the board restart the watch with
`pbl_reboot()` that long after boot, from a delayed work item on the high
priority work queue, which a stalled SysWorkq or busy KernelMain do not hold
up. The restart goes through the bootloader into the stub, which then reads
the RAM boot record (a `kernel 0x5` breadcrumb at the set time) and the flash
log on the eMMC.

## Consequences
A run's end is known in advance: the stub's port coming back earlier means
the run reset by itself. The restart takes a core dump when a software failure
is still flagged from an earlier run (`die.c`), which shows as a `kernel 0x7`
breadcrumb right after the `0x5`. Not for release images: it is only there in
slot builds.
