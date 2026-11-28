# eMMC timeouts read the status after the clock

- Status: Accepted
- Date: 2026-11-28 12:45 UTC
- Decided by: agent

## Context
While testing workouts, one write to the flash log area (block 437028, in
`debug-db`) failed with `CMD23 lost`, then went through on the retry that the
MMC class makes after a power cycle of the card (261127-123202-01). `CMD23
lost` is `prv_cmd()` in `drivers/mmc/stm32_sdmmc.c` giving up after 10 ms
without the SDMMC reporting the command sent, answered or timed out. The
hardware cannot take that long: a command without an answer times out after
64 card clocks (160 us at 400 kHz, 1.6 us at 40 MHz), and CMD23 has no busy
signal; nothing else on the bus was running, as the MMC class holds its lock
across the whole transfer. The loop read the status, then the clock, and gave
up if the clock was past the timeout:

- a thread preempted between the two for more than 10 ms (interrupts, or any
  thread of higher priority: the flash log is written from whatever thread
  logs, the system work queue at priority 1 among them, below the app and
  KernelMain) finds the command ended meanwhile, but has already read the
  status from before;
- the same holds for the data wait (1 s for a write) and the busy wait after
  it, which also sleep between polls.

Stop 2 is not the cause: the driver blocks it during a power-up and each
transfer, and the cycle counter it times with counts in Sleep. Nor is the pin
controller's clock (261127-193344-01): it only ever enables a port's clock, and
the SDMMC's own clock is the driver's. A card busy period shows as busy in
SEND_STATUS, not as a lost command.

## Decision
Every wait reads the clock first, then the status, and gives up only on a
status read after the deadline: a preempted thread sees what the hardware did
meanwhile. A command that is really lost logs its status register, and notes
it in the boot trace (`PBL_BT_NOTE_SDMMC_STA`) as data errors already do. The
retry after a power cycle stays, for real failures.

## Consequences
A preemption no longer turns into a failed transfer, a 130 ms power cycle of
the card and a second write. A command that does not end within its time,
read after it, still fails as before. The cause was inferred from the code
and the one log line; the failure has not been reproduced on demand.
