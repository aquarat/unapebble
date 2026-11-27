# Why B may loop: hypotheses and the experiment that decides each

- Status: Proposed
- Date: 2026-11-26 23:27 UTC
- Decided by: agent

## Context
Read in the code, no console on the watch. B's packs are the same as the
current tree's (resources CRC 0x910f3f55, 592 resources).

## Decision
The hypotheses, most likely first, each with what decides it:

1. The board cuts EMMC_EN after the card came up
   (261126-232719-12-emmc-en-left-to-driver); a failed retry asserts in
   `pbl_flash_read()`. Lab "eMMC supply cut"; E's trace (MMC read failed,
   panic "Flash read failed"); F boots.
2. The card does not come up under PebbleOS (timing, clock, bus width, High
   Speed): the flash device fails, no resources are found, the watch powers
   off through the bootloader. Lab "eMMC step by step" and "eMMC via driver",
   on HSI48 and on B's clocks; E's trace (sdmmc steps, notes OCR and STA).
3. The first boot's 16 MiB PFS format over an un-erased GUI region: about
   256 erases of 64 KiB and 4096 header writes, tens of seconds; anything that
   resets meanwhile starts it again on every boot. Lab "NOR window" times it,
   "B's storage" counts the formatted pages; E's trace (pfs format, nor erase
   at, with uptime).
4. No resources found, or the power-off through the bootloader not staying
   off: Lab "Update drive" (the pack and its version), "Test: power off";
   E's trace (res MISSING, kernel power off).
5. A fault in the new code: E's fault record, PC and LR, with E's ELF.
6. The PLL1 change: unlikely (261126-232719-09); Lab "B's clocks".
7. The bootbits change: only the version Settings and the shell show; not
   a cause.

## Consequences
The maintainer runs the Lab's experiments, then E, then the Lab again; the
results decide between them. This record is superseded when the cause is
known.
