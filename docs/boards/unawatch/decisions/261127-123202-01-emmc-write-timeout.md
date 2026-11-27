# Give eMMC writes a 1 s data timeout

- Status: Accepted
- Date: 2026-11-27 12:32 UTC
- Decided by: agent

## Context
F (the eMMC firmware with the EMMC_EN fix) still looped on the watch. Its
boot record, read through the tether stub, ended with `Flash erase failed:
-116` (-ETIMEDOUT) and SDMMC status note `0x19000008`: CMD25, data timeout.
The card initialised, High Speed and four data lines included, and reads
worked. `emmc_write_test`, run through the tether, then showed every write
mode working (CMD24, CMD23 + CMD25, reliable writes; 1 bit at 400 kHz, 4
bits at 20 MHz, High Speed at 40 MHz), but the first write after the card is
powered up takes 130 to 140 ms; later ones take under a millisecond. The
driver gave a data command 100 ms + 2 ms a block in software (a 250 ms data
timer in hardware), so the first write after each power-up always timed out,
and the retry, which powers the card again, timed out the same way.

## Decision
Reads keep a 250 ms data timer; writes get 1000 ms. The software timeout of
a data command is the data timer + 100 ms + 2 ms a block, so it can't fire
before the hardware does.

## Consequences
With the fix, linked for the tether slot and booted from it, PebbleOS
formatted its 16 MiB filesystem on the eMMC (about 30 s at first boot) and
mounted it. The card's slow first write recurs after every idle power-off
(2 s, CONFIG_MMC_IDLE_OFF_MS): a cost in latency, and maybe energy, to weigh
against the idle power-off.
