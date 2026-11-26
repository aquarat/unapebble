# Boot trace kept in TAMP 30 and 31 and in 1 KiB of RAM below the stock crash region

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: agent

## Context
Breadcrumbs must survive a reset, and ideally the bootloader installing
another image, so that the Lab can show where an eMMC build stopped. The
stock firmware uses TAMP backup registers 0 to 23 (DR3 the bootloader's
power-off flag), PebbleOS 24 to 29 (boot bits, reboot reason, the last
app's slot). The STM32U5 has 32.

## Decision
`CONFIG_BOOT_TRACE` (`subsys/boot_trace`) keeps the last breadcrumb in
register 30 and a boot count, with the image's number, in 31. The rest goes
in a RAM record: the last 40 breadcrumbs with their uptime, the first fault,
the first panic, noted values, and the reset flags and reboot reason the run
started with, in `boot_trace` (`0x2025dc00`, 1 KiB), reserved in the board's
tree below the stock kernel's crash region. The bootloader's stack starts at
`0x201f0000` (the first word of `UnaBootloader_0.1.4.hex`) and grows down,
so it should not reach either; the record is kept apart at the next boot,
before anything else, with the RCC reset flags before they are cleared.

## Consequences
- Software resets, faults, asserts and watchdog resets: both kept.
- Installing an image through the bootloader: the registers are kept (the
  backup domain is only reset with a power loss of VBAT, and the bootloader
  keeps the stock firmware's time there); the RAM should be (inferred from the
  bootloader's stack): the Lab says whether it found it ("RAM record: kept").
- A power loss, or an empty battery: the RAM record is lost, the registers
  only if VBAT is lost too.
- The stock firmware overwrites the RAM record when it runs.
