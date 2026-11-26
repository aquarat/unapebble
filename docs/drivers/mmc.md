# MMC

An MMC is an eMMC soldered to the board, as a block device of 512-byte
blocks in its user data area. The class powers the card up for the first
access and down again once it has been idle for `CONFIG_MMC_IDLE_OFF_MS`,
serializes accesses, and retries a failed one once after power cycling the
card. Writes return once the card has programmed them. The flash class reaches
a range of a card as NOR flash through the MMC NOR window driver
(`pebble,mmc-nor`, see {doc}`flash`).

| | |
| --- | --- |
| Directory | `drivers/mmc` |
| Header | `pbl/drivers/mmc.h` |
| Kconfig | `CONFIG_MMC`, selected by the drivers and code that use it |
| Role | `MMC` |
| API | {apidoc}`API reference <drivers_mmc>` |

## Drivers

| Driver | Compatible | Kconfig |
| --- | --- | --- |
| STM32 SDMMC, an eMMC on four data lines, polled, internal DMA | `st,stm32-sdmmc2` | `CONFIG_MMC_STM32_SDMMC` |
