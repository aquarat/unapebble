# An MMC driver class, with a polled STM32 SDMMC driver for the eMMC

- Status: Accepted
- Date: 2026-11-26 17:21 UTC
- Decided by: agent

## Context
PebbleOS had no driver for an eMMC, nor a class for block devices. The flash
storage needs one range of the card, and the resource copy reads another (the
update drive), so both go through one device that serializes them.

## Decision
A new `mmc` class (`pbl/drivers/mmc.h`, role `MMC`) presents the card as
512-byte blocks: it powers the card up on use and down when idle, serializes
accesses, and retries a failed transfer once after a power cycle (three
power-up attempts each). Like the flash class it has a coredump mode without
OS services.

`drivers/mmc/stm32_sdmmc.c` drives the eMMC on SDMMC1 as the stock firmware
does: 1.8 V sector-mode OCR, identification at 400 kHz on one line, the power
class, four data lines, then High Speed at 40 MHz from PLL1 Q at 80 MHz if the
card has it, checked by reading EXT_CSD again, with a fallback to 20 MHz
default speed. Transfers are polled, spinning briefly and then sleeping, with
the host's internal DMA moving the data (no FIFO overruns, the reason the
stock driver raises its priority); unaligned or non-SRAM buffers bounce
through one block. CMD23 precedes multi-block transfers, so no CMD12 is needed
but on errors. Writes are reliable writes (enhanced, or single blocks on cards
with only the legacy kind), and wait for the card to leave programming. EMMC_EN
is a GPIO of the driver rather than a regulator, because the coredump path has
to power the card without the regulator's mutex and sleeps.

The CPU is kept out of Stop mode during each access. The SoC start-up now turns
PLL1 off after moving the system clock to MSIS, so that the clock tree can
enable PLL1 Q whatever the bootloader left running.

## Consequences
Nothing of it has run on hardware. Interrupts and DMA completion could replace
the polling later without changing the class. Only cards addressed in
sectors (over 2 GB) are supported.
