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
unit, the buttons, the display, the haptics and storage in the embedded flash.
Bluetooth, USB, the eMMC, the sensors and the power management come next; see
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
`0x20270000`. The flash below it is left alone. The CPU executes from the
first of the two 2 MiB banks, and the firmware stores its data in the second
one, which it can program and erase while the first keeps running:

| Flash | Size | Contents |
| --- | --- | --- |
| `0x08000000`–`0x08027fff` | 160 KiB | Stock bootloader (`bootloader`) |
| `0x08028000`–`0x0805ffff` | 224 KiB | Shared libc of the stock kernel (`una-libc`) |
| `0x08060000`–`0x081fffff` | 1664 KiB | PebbleOS (`code`) |
| `0x08200000`–`0x082fffff` | 1024 KiB | System resources (`system-resources-bank-0`) |
| `0x08300000`–`0x08301fff` | 8 KiB | Second resource bank, for updates over Bluetooth: unused |
| `0x08302000`–`0x0830bfff` | 5 × 8 KiB | `safe-firmware`, `scratch`, `mfg-info`, `tzinfo`, `shared-prf-storage` |
| `0x0830c000`–`0x0830ffff` | 16 KiB | Free |
| `0x08310000`–`0x083dffff` | 832 KiB | Filesystem (PFS, `filesystem`) |
| `0x083e0000`–`0x083fffff` | 128 KiB | Flash logs (`debug-db`) |

The firmware partitions are in `pebble/boards/unawatch.overlay`, each a whole
number of 8 KiB pages so that none shares a page with another. A release build
takes about 1606 KiB of the code partition and a debug one 1637 KiB, Moddable
XS included; the resources about 885 KiB of theirs.

This needs the STM32U5A5QJ's 4 MiB. A 2 MiB part fits the code alone: the
flash driver refuses to start on it, and its bootloader refuses the `.ota`,
which is larger than the 1664 KiB it then allows.

The firmware takes SRAM1 to SRAM5 up to `0x2025e000`. The 72 KiB above, up to
`0x20270000`, is where the stock kernel keeps its crash dumps and logs across
resets, a reserved memory region left alone.

### Embedded flash as storage

A 16-byte word of the embedded flash can only be programmed once after its 8
KiB page is erased, while PebbleOS's storage writes bytes and clears bits of
bytes it wrote before, on 4 KiB subsectors, as on a NOR flash. The driver,
`drivers/flash/stm32u5.c` with `drivers/flash/ecc_nor.c`, keeps the last four
pages written in RAM and programs a word once all of it is written, a second
later, or ten minutes later for the last word of a run of appends, which the
next append usually completes. Clearing bits of a programmed word, or erasing
half a page, erases the page and programs it again. A reset or power-off from
the firmware programs everything first; a hard fault does as it saves its
core dump. A watchdog reset or a power loss loses up to the last second of
writes, and a power loss during a page erase may lose that page. Core dumps
go to the 8 KiB `scratch` partition, which only holds their first 4 KiB.

The ICACHE caches reads of the flash: the driver invalidates it after each
change. The system resources are read in place (`CONFIG_MMAP_RESOURCES`).

The bootloader erases from `0x08060000` to the end of the flash before it
installs an image, so **every install empties the filesystem and the logs**:
settings, apps and watchfaces installed later, and the timeline start again.
The system resources go in with the image instead: the build appends them at
their partition, in `pebbleos_resources.bin`, which is what is packed.

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
python3 tools/una/pack_ota.py build-una-fw/pebbleos_resources.bin \
    -v "$(git describe --tags --always)" --max-size 0x3a0000 -o unapebble.ota
```

or `pbl configure --board unawatch -DCONFIG_RELEASE=y && pbl build`. The
build writes `pebbleos.bin`, the code alone, and `pebbleos_resources.bin`,
the code then the system resources at `0x08200000`. `--max-size 0x3a0000`
lets the packer take up to the end of a 4 MiB flash. The
`.forgejo/workflows/unawatch.yml` workflow does the same and publishes the
`.ota`.

## Installing

1. Turn the watch off, then hold the power button (top right) for 4 s as it
   turns on: the bootloader menu appears.
2. Connect USB: the update drive appears. Copy the `.ota` to its root, under a
   name ending in lowercase `.ota`.
3. Eject the drive, unplug, and choose **Continue**. The bootloader checks
   the file, erases the flash from `0x08060000`, writes the image, deletes the
   file and starts it.

The battery must be above about 10% for the bootloader to look for updates.
The image can also be flashed over SWD: `pebbleos_resources.bin` at
`0x08060000`, with the `probe-rs` runner or any probe, which leaves the rest
of the flash alone.

## Recovering

The bootloader runs first on every boot and only checks the stack pointer of
the image, so a firmware that does not boot does not stop it. Hold the power
button for 4 s as the watch turns on, and install UNA's own `.ota` the same
way. Nothing PebbleOS writes reaches the eMMC, where the update drive is.

(unawatch-status)=

## Status

| | |
| --- | --- |
| **Built, not yet run on a watch** | Display and frontlight, VCOM on LPTIM1/LPTIM3, buttons, haptics, storage in the embedded flash, retained registers (TAMP 24 to 29; the stock firmware uses 0 to 23) |
| **Stand-ins, which nothing feeds** | Accelerometer, ambient light, microphone, die temperature and charger are simulators: no steps, the battery always reads 100% and unplugged |
| **Off** | Bluetooth runs on the fake controller, as on boards without a radio: no phone connection, so apps and updates only come with the image. No magnetometer, heart rate, barometer, GNSS, buzzer, USB or eMMC |
| **Unknown** | Whether the panel is round (its round mask is a computed circle); whether the panel and haptics rails are on at power-on as expected |

Things to watch on a first boot:

- **Display**: the image turned or mirrored, or the colors swapped, would
  come from the rotation (`rotation = <90>` of the panel) or the data pins;
  a blank panel from VCOM, the PMIC rails or the timing. The conversion of a
  line is checked against the stock driver's by
  `tests/drivers/display/test_jdi_gpio_line.c`.
- **Storage**: the flash driver programs the embedded flash for the first
  time on a watch. It refuses to start on a part of other than 4 MiB or with
  swapped banks, and only ever changes the second bank. An error while
  programming stops the firmware; a word torn by a power loss reads with an
  ECC error, which faults: install the `.ota` again.
- **CPU time**: each display update takes about 40 ms of busy CPU,
  preemptible.
- **Code space**: the code partition is 96.5% full in release, 98.4% in
  debug builds.
## Decisions

The reasons behind the port's design are recorded in the
[decision log](decisions/index.md). Read it before changing something it
covers, and add a record for each new decision.

```{toctree}
:hidden:

hardware.md
decisions/index.md
```
