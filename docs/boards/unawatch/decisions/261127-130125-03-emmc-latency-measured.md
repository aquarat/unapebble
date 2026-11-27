# eMMC latency measured: keep the 2 s idle power-off

- Status: Accepted
- Date: 2026-11-27 13:01 UTC
- Decided by: agent

## Context
The task watchdog resets of the eMMC firmware (261127-130125-01) raised the
question whether the eMMC was too slow under PebbleOS: the idle power-off
(261126-172141-03) making every log flush pay a power-up and the slow first
write, maybe holding the log or MMC mutex meanwhile; the polled driver's
sleeps; the NOR layer's read-modify-write of every small write. A temporary
build counted, through the tether, the MMC class's power-ups and transfers,
the flash log's flushes and the display's frames, and logged them every 15 s
with the threads' CPU shares.

Runs on the watch, all booted from the tether slot (commit b7bc058f1 plus the
temporary counters, then this branch):

1. Counters, no other change, 300 s then a timed restart
   (261127-130125-02): no stall. The charging animation ran only some seconds
   (the charge read 100%).
2. Counters and a temporary change keeping the charging animation running,
   frames on KernelMain: SysWorkq not fed for 6.8 s, preempted in
   `pbl_mutex_unlock()`; task watchdog reset at 11 s. This is the failure of
   the first run with the write timeout fixed.
3. As 2, frames on the display thread: 300 s, no stall, timed restart.
4. This branch, no counters or animation change, the watchface ticking:
   661 s, then the timed restart (`kernel 0x5` at 661019 ms); no task
   watchdog, fault or panic in the boot record or the flash log.

Measured in runs 1 and 3, the card on four lines at 40 MHz:

| | |
| --- | --- |
| Power-up, supply to transfer state | 92-124 ms, mostly 99: 36 ms for the supply, 42 ms of CMD1 busy, 13 ms to read EXT_CSD |
| First write after a power-up | 108-124 ms (the stub measured 130-140 ms) |
| Other writes, a NOR write of a log record | 2.0-4.0 ms, a block read and a reliable write |
| Reads | at most 2-4 ms; 11,000 at boot, PFS and resources |
| Wait for the MMC mutex | at most 1 ms |
| A log flush after 2 s of quiet | 225-245 ms, at most 327 ms, on SysWorkq |

## Decision
Keep the idle power-off at 2 s (`CONFIG_MMC_IDLE_OFF_MS`). The eMMC did not
cause the stalls: writes are as fast under PebbleOS as from the stub, every
access is bounded well below the watchdog's 10 s, and the mutexes are held
for a flush at most a third of a second, by SysWorkq. What the power-off
costs, about 210 ms of a powered card per idle spell (power-up and the slow
first write), is to be weighed against the idle card's current once currents
can be measured.

## Consequences
A log line written after a quiet spell takes a quarter of a second to reach
the card, on SysWorkq. Frequent small writes cycle the card's power that
often. The timeout stays the one option to tune; with logging every few
seconds, a longer one would keep the card powered between flushes.
