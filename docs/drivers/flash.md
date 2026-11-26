# Flash

A flash is addressed by offsets from its first byte. Writes only clear bits,
so a range is erased first; the class erases with as few sector and
subsector erases as it can, skipping blank ones. `drivers/flash/README.md`
describes the class core, its coredump mode, which relies on no OS service,
and how NOR parts are described.

| | |
| --- | --- |
| Directory | `drivers/flash` |
| Header | `pbl/drivers/flash.h` |
| Kconfig | `CONFIG_FLASH`, enabled by the application |
| Role | `FLASH` |
| Sample | {doc}`../samples/drivers/flash` |
| API | {apidoc}`API reference <drivers_flash>` |

## Drivers

| Driver | Compatible | Kconfig |
| --- | --- | --- |
| SPI NOR | `jedec,spi-nor` | `CONFIG_FLASH_SPI_NOR` |
| QEMU External Flash | `qemu,pebble-extflash` | `CONFIG_FLASH_QEMU` |
| STM32U5 embedded flash, its second bank, writes cached and programmed in whole 16-byte words | `st,stm32u5-flash-controller` | `CONFIG_FLASH_STM32U5` |
| MMC NOR window, a range of an {doc}`mmc` as NOR flash, blocks read, changed and written back | `pebble,mmc-nor` | `CONFIG_FLASH_MMC_NOR` |
| POSIX External Flash | — | `CONFIG_FLASH_POSIX` |
