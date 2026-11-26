# UNA Watch (unawatch)

## Overview

:::{div} board-card

:Board: `unawatch`
:SoC: ST STM32U5A5
:::

The UNA Watch is a modular smartwatch on an ST STM32U5A5. PebbleOS runs on it
in place of the stock UNA kernel, started by the stock bootloader, which stays
in flash with the shared libc of the stock kernel so that the stock firmware
can be put back.

The port runs the full firmware, on the gabbro platform of the Pebble Round
2: the SoC with its clocks, console, system timer, RTC, watchdog, RNG and CRC
unit, the buttons, the display, the haptics and storage on the eMMC.
Bluetooth, USB, the sensors and the power management come next; see
[](#unawatch-status). None of it has run on a watch yet.

{doc}`hardware` describes the hardware as the stock kernel drives it, with
the sources of each detail.

## Hardware

- **SoC**: ST STM32U5A5QJ, an Arm Cortex-M33 at 160 MHz with single-precision
  FPU and DSP extension, 4 MiB flash and 2.5 MiB SRAM, TrustZone off. Early
  watches may carry the STM32U595QI, with 2 MiB of flash.
- **Clocks**: 16 MHz crystal (USB PHY only), 32.768 kHz external clock
- **Display**: 240 × 240 reflective colour memory-in-pixel LCD, 6 bits per
  pixel, on a parallel JDI/Sharp interface, with a frontlight
- **Storage**: 4 GB eMMC on SDMMC1
- **USB**: USB OTG high-speed on USB-C, with USB Power Delivery (STUSB4500)
- **Power**: NXP PCA9420 PMIC with charger, Maxim MAX17262 fuel gauge,
  200 mAh battery
- **Haptics**: TI DRV2625, and a buzzer
- **Bluetooth**: ST BlueNRG-2 module, on SPI1
- **GNSS**: Airoha AG3335M module, on LPUART1
- **Sensors**: Bosch BMI270 and BMM350, PixArt PAH8316LS heart rate,
  TE MS5837 pressure
- **Buttons**: four, Back (PD8), Up (PD4), Select (PD15, also the power
  button) and Down (PA5)
- **Debug**: SWD; the USART3 console (PC4, 921600 baud) on the USB-C
  connector, through a Type-C debug accessory

## Memory

The stock bootloader starts the image at `0x08060000`, from the stack pointer
and reset vector there, provided the stack pointer is in SRAM, at or below
`0x20270000`. The flash below it is left alone. The embedded flash only holds
code:

| Flash | Size | Contents |
| --- | --- | --- |
| `0x08000000`–`0x08027fff` | 160 KiB | Stock bootloader (`bootloader`) |
| `0x08028000`–`0x0805ffff` | 224 KiB | Shared libc of the stock kernel (`una-libc`) |
| `0x08060000`–`0x083fffff` | 3712 KiB | PebbleOS (`code`) |

The partitions are in `boards/unawatch/unawatch.dts`. The image is the code
alone, `pebbleos.bin`: a release build takes about 1611 KiB and a debug one
1642 KiB, Moddable XS included. The code partition reaches the end of the 4 MiB
flash, but `tools/una/pack_ota.py` refuses an image over 1664 KiB unless told
otherwise, so that it also installs on the 2 MiB STM32U595 of early watches.
Moving the storage and the resources to the eMMC freed the whole second bank,
2048 KiB: the code partition grew from 1664 KiB to 3712 KiB, and the image no
longer carries 905 KiB of resources, though one for a 2 MiB part still has to
stay within 1664 KiB.

The firmware takes SRAM1 to SRAM5 up to `0x2025e000`. The 72 KiB above, up to
`0x20270000`, is where the stock kernel keeps its crash dumps and logs across
resets, a reserved memory region left alone.

### Storage on the eMMC

The storage is on the 4 GB eMMC, in the GUI region of UNA's layout, which
neither UNA's firmware nor its bootloader uses ({doc}`hardware`, sections 3.3
and 3.4). The board gives the flash its first 32 MiB, from 200 MiB into the
eMMC (`emmc_storage` in `unawatch.dts`), and `pebble/boards/unawatch.overlay`
lays it out, in offsets from there:

| Offset | Size | Contents |
| --- | --- | --- |
| `0x0000000` | 4 MiB | System resources (`system-resources-bank-0`) |
| `0x0400000` | 4 MiB | System resources of another version (`system-resources-bank-1`) |
| `0x0800000` | 4 × 8 KiB | `safe-firmware` (only protected), `mfg-info`, `tzinfo`, `shared-prf-storage` |
| `0x0900000` | 4 MiB | Core dumps (`scratch`), which take the 2424 KiB of RAM in use |
| `0x0d00000` | 512 KiB | Flash logs (`debug-db`), as large as they go |
| `0x1000000` | 16 MiB | Filesystem (PFS, `filesystem`) |

The bootloader does not touch any of it, so **settings, apps and logs survive
an install**, and a switch to the stock firmware and back.

The eMMC (`drivers/mmc/stm32_sdmmc.c`) runs on four data lines, at 40 MHz in
High Speed from the 80 MHz PLL1 Q, as the stock firmware does. Its supply,
EMMC_EN, is switched off once it has been idle for 2 s
(`CONFIG_MMC_IDLE_OFF_MS`) and on again, with 35 ms to come up, by the next
access. The flash (`drivers/flash/mmc_nor.c` with `drivers/flash/blk_nor.c`)
has NOR semantics on the 512-byte blocks: a write reads its blocks, clears the
bits and writes them back before it returns, and an erase writes blocks of
0xFF. Writes are reliable writes, which a power loss leaves either done or not.
Nothing is held in RAM that a reset would lose; the 16 blocks last read or
written in part are cached for reads.

### System resources

The bootloader only installs the code, so the system resources come from the
update drive instead, where they are copied next to the `.ota`
(`pebble/resource/resource_storage_volume.c`, `CONFIG_RESOURCES_FROM_VOLUME`).
At boot, when neither resource bank holds the resources the firmware was built
with, it looks in the root directory of the update drive (exFAT, FAT32 or
FAT16) for a file whose manifest has their version, whatever its name, copies
it into the bank with no resources or the oldest, and checks its CRC. A
firmware that finds the resources of its version already there, as after
installing the same build again or going back to the one before, copies
nothing. Without them, it logs the version it needs and powers the watch off:
there is no recovery firmware to go to, and a reset would only start it again.

## Boot

The bootloader jumps to the image with interrupts masked and its clocks and
peripherals as it used them. The SoC masks and clears every interrupt, resets
the bus masters that could still write to RAM, moves the system clock to
MSIS and turns the instruction cache on. The clock controller then selects
the SMPS and voltage range 1, and runs the CPU and every bus at 160 MHz from
PLL1 on MSIS at 48 MHz, with 4 flash wait states. The system timer is LPTIM4
on the 32.768 kHz clock; LPTIM1 and LPTIM3 are left to the display.

At boot the board drives the enables of the eMMC, the buzzer, the barometer,
the GNSS and the reset of the BLE module low, and, outside release builds,
USB_DAM (PB15) high to route the console to the USB-C connector
(`CONFIG_BOARD_UNAWATCH_CONSOLE_DAM`).

The firmware writes nothing to the PCA9420 PMIC: the rails the display and
the haptics need are on in its power-on mode, as the bootloader, which drives
the display too, runs on them ({doc}`hardware`, section 4.2). The stock
firmware also sets its ON-key long press, on Select, to 16 s, which the PMIC
keeps until it loses power.

## Display

The 240 × 240 memory-in-pixel panel shows the centre of the 260 × 260
framebuffer of the gabbro platform, so that apps for the Pebble Round 2 run
unchanged, losing a 10 pixel ring at the edge; a platform of its own would
need apps built for it. The display is `jdi-parallel-gpio`
(`drivers/display/jdi_gpio.c`): the CPU clocks every frame out on the GPIO
as the stock firmware does, the panel turned a quarter round, with the
timings of the stock driver. A frame takes about 40 ms of CPU, every update
the whole frame. The panel, `sharp,ls012b7dd06a` on the memory-in-pixel
panel driver, takes its VCOM from LPTIM1 (FRP) and its inverse from LPTIM3
(XFRP), at 30.7 Hz on the LSE. The frontlight is on PF3, on or off.

## Buttons and haptics

| Button | Pin | Key |
| --- | --- | --- |
| Bottom right | PD8 | Back |
| Top left | PD4 | Up |
| Top right, power | PD15 | Select |
| Bottom left | PA5 | Down |

Back and Select held for 5 s reset the watch. With Up held as well, PebbleOS
asks for its recovery firmware, which the UNA bootloader does not have: the
watch just resets. Holding Back quits the app, as on every
Pebble. The DRV2625 on I2C1 drives the ERM in real-time playback, open loop,
at the stock firmware's voltages.

## Power off

The watch powers off through its bootloader: power-off sets backup register
3 (`RTC_BKP_DR3`) and resets the SoC, and the bootloader, seeing it, powers
the watch off again unless the power button is held. The power button has to
be released first, which power-off waits for. Holding it powers the watch on.

## Building

In the build container ({doc}`/getting_started/index`, `docker/README.md`):

```shell
BOARD=unawatch BUILD_DIR=build-una-fw docker/build.sh -DCONFIG_RELEASE=y
python3 tools/una/pack_ota.py build-una-fw/pebbleos.bin \
    -v "$(git describe --tags --always)" -o unapebble.ota
cp build-una-fw/system_resources.pbpack unapebble.pbpack
```

or `pbl configure --board unawatch -DCONFIG_RELEASE=y && pbl build`. The
build writes `pebbleos.bin`, the image, and `system_resources.pbpack`, the
resources that go with it. The packer refuses an image that would not fit a
2 MiB part; `--max-size 0x3a0000` lets it take up to the end of a 4 MiB one.
The `.forgejo/workflows/unawatch.yml` workflow does the same and publishes the
`.ota` and the `.pbpack`.

## Installing

1. Turn the watch off, then hold the power button (top right) for 4 s as it
   turns on: the bootloader menu appears.
2. Connect USB: the update drive appears. Copy the `.ota` and the `.pbpack`
   of the same build to its root, the `.ota` under a name ending in lowercase
   `.ota`.
3. Eject the drive, unplug, and choose **Continue**. The bootloader checks
   the file, erases the flash from `0x08060000`, writes the image, deletes the
   `.ota` and starts it. The firmware then copies the resources from the
   `.pbpack`, a few seconds behind the splash screen, the first time it
   starts. The `.pbpack` stays on the drive, and may stay there.

If the watch turns off right after the splash, the resources of its build are
not on the drive: copy that `.pbpack` and start again. The battery must be
above about 10% for the bootloader to look for updates. The image can also be
flashed over SWD: `pebbleos.bin` at `0x08060000`, with the `probe-rs` runner
or any probe, which leaves the rest of the flash alone; the resources still
come from the update drive.

## Recovering

The bootloader runs first on every boot and only checks the stack pointer of
the image, so a firmware that does not boot does not stop it. Hold the power
button for 4 s as the watch turns on, and install UNA's own `.ota` the same
way. PebbleOS only writes to the GUI region of the eMMC, which the stock
firmware does not use; its update drive and files are left alone.

(unawatch-status)=

## Status

| | |
| --- | --- |
| **Built, not yet run on a watch** | Display and frontlight, VCOM on LPTIM1/LPTIM3, buttons, haptics, the eMMC with the storage and the system resources on it, retained registers (TAMP 24 to 29; the stock firmware uses 0 to 23) |
| **Stand-ins, which nothing feeds** | Accelerometer, ambient light, microphone, die temperature and charger are simulators: no steps, the battery always reads 100% and unplugged |
| **Off** | Bluetooth runs on the fake controller, as on boards without a radio: no phone connection, so apps and updates only come with the image. No magnetometer, heart rate, barometer, GNSS, buzzer or USB |
| **Unknown** | Whether the panel is round (its round mask is a computed circle); whether the panel and haptics rails are on at power-on as expected |

Things to watch on a first boot:

- **Display**: the image turned or mirrored, or the colors swapped, would
  come from the rotation (`rotation = <90>` of the panel) or the data pins;
  a blank panel from VCOM, the PMIC rails or the timing. The conversion of a
  line is checked against the stock driver's by
  `tests/drivers/display/test_jdi_gpio_line.c`.
- **Storage**: the eMMC driver runs for the first time on a watch. A card
  that does not come up is power cycled and tried three times; a transfer
  that fails is tried again after a power cycle. Then the flash class stops
  the firmware with an error. Watch the log for `Card init failed`, a
  fallback from High Speed, or `Window past the ... blocks of the card`.
- **Resources**: the first start after an install copies about 900 KiB from
  the update drive; the log says `Copying ... bytes of resources`. A watch
  that turns itself off after the splash did not find them.
- **Clocks**: PLL1 now also gives 80 MHz on Q for the eMMC, which the SoC
  start-up turns off first, whatever the bootloader left running.
- **CPU time**: each display update takes about 40 ms of busy CPU,
  preemptible.
- **Code space**: images of 1611 KiB in release and 1642 KiB in debug
  builds, of the 1664 KiB a 2 MiB part takes.
## Decisions

The reasons behind the port's design are recorded in the
[decision log](decisions/index.md). Read it before changing something it
covers, and add a record for each new decision.

```{toctree}
:hidden:

hardware.md
decisions/index.md
```
