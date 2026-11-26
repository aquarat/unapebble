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
unit, the buttons, the display, the haptics, storage on the eMMC and the
battery: its charger and its fuel gauge. Bluetooth, USB and the sensors come
next; see
[](#unawatch-status). The first port, with its storage in the embedded flash,
boots on a watch; the images with the storage on the eMMC loop at boot, which
the experiments firmware, [the Lab](#unawatch-lab), is for finding out.

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
alone, `pebbleos.bin`: a release build takes about 1619 KiB and a debug one
1651 KiB, Moddable XS and the battery test included. The code partition reaches the end of the 4 MiB
flash, but `tools/una/pack_ota.py` refuses an image over 1664 KiB unless told
otherwise, so that it also installs on the 2 MiB STM32U595 of early watches.
Moving the storage and the resources to the eMMC freed the whole second bank,
2048 KiB: the code partition grew from 1664 KiB to 3712 KiB, and the image no
longer carries 905 KiB of resources, though one for a 2 MiB part still has to
stay within 1664 KiB.

The firmware takes SRAM1 to SRAM5 up to `0x2025dc00`. The 1 KiB above it is
the boot trace's record (`boot_trace`, see [](#unawatch-boot-trace)), and the
72 KiB above that, up to `0x20270000`, is where the stock kernel keeps its
crash dumps and logs across resets, a reserved memory region left alone.

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

At boot the board drives the enables of the buzzer, the barometer, the GNSS,
the reset of the BLE module and, without its driver, the eMMC low, and, outside release builds,
USB_DAM (PB15) high to route the console to the USB-C connector
(`CONFIG_BOARD_UNAWATCH_CONSOLE_DAM`).

The firmware leaves the rails of the PCA9420 PMIC alone: the ones the display
and the haptics need are on in its power-on mode, as the bootloader, which
drives the display too, runs on them ({doc}`hardware`, section 4.2). It only
configures the PMIC's charger (see [](#unawatch-battery)). The stock firmware
also sets its ON-key long press, on Select, to 16 s, which the PMIC keeps
until it loses power.

(unawatch-battery)=

## Battery and charging

The 200 mAh cell is charged by the PCA9420's linear charger
(`drivers/charger/pca9420.c`) and measured by the MAX17262 fuel gauge
(`drivers/gauge/max17262.c`), both on I2C1; the battery service takes the
percentage from the gauge (`CONFIG_BATTERY_SOC_GAUGE`). Until the cell is
identified, the owner caps its charge at 4.20 V, which every lithium cell
takes; the stock firmware charges to 4.40 V, which suits only a high-voltage
cell. The charge is set for the cell's life rather than its last few percent:

| | PebbleOS | Stock firmware |
| --- | --- | --- |
| Charge current | 100 mA (0.5C) | 150 mA |
| Charge voltage | 4.20 V | 4.40 V |
| Termination | 14 mA, 6.4 min top-off | 10 mA, 19.2 min top-off |
| Pre-charge, dead battery | 8 mA, 4 mA | PMIC defaults |
| Safety timers | 5 h fast, 30 min pre-charge | 9 h fast |
| Recharge | 240 mV below the charge voltage | 140 mV |
| Charge temperature | 0 to 45 degrees C at the gauge, 3 degrees of hysteresis | stops above 45 degrees C ambient or 55 at the gauge |
| Input current limit | 255 mA | 255 mA |

The gauge runs the stock firmware's EZ model for a cell charged to 4.20 V
(ModelCFG.VChg = 0, where the stock firmware has 1), so that the end of each
charge is its 100% and it learns the capacity to 4.20 V. The model is loaded
only after a power-on reset of the gauge or when its registers hold another
model; as the stock firmware's differs, switching to it makes it reload its
own and forget what the gauge learned. Charging starts off at every boot
and is allowed once the gauge reads a temperature in range; a gauge that
cannot be read keeps it off. Unplugged, the watch shuts down below 1% or at
an averaged 3.35 V, as the stock firmware does; it shows 5% until the gauge
first answers. `CONFIG_CHARGER_PCA9420_CHARGE_LIMIT_MV` stops the charge short of
full, at a lower voltage, for a longer life still. The cap holds while
PebbleOS runs: whether a watch that is off charges, and to what, is not known
yet.

The interrupt of the PMIC, on PG7 with the STUSB4500's alert, reports plugging
in and out; the battery service also polls every 30 s plugged in, 60 s
otherwise. The STUSB4500 is left on its factory configuration. The reasons for
each value are in the [decision log](decisions/index.md), from
261126-173552-01.

## Battery capacity test

The **Battery test** app measures the charge the cell gives from a 4.20 V
charge to the low battery power-off at 3.35 V, its usable capacity, and the
charge it takes back. It counts with the gauge's coulomb counter (QH, 0.5 mAh a
count) and, as a cross-check, an integral of the gauge's averaged current;
both are shown. It samples on the battery service's polls, a minute apart,
logs to the filesystem on the eMMC and carries on across the power-off, so it
keeps running when the app is closed. It only reads the gauge and the
charger: the charge cap and the charger's protections stay as they are.
`CONFIG_BATTERY_TEST`, on for the watch only.

To run it:

1. Open **Battery test** and press Select to start. Plug the watch in; the
   test waits for the charger to end the charge (14 mA, then the 6.4 minute
   top-off). From empty, about 2.5 to 3 hours at 100 mA.
2. When it shows **Full**, choose the load with Select: **Normal** (the
   watch used as usual; close the app) or **Heavy** (frontlight on and the
   CPU busy 40 ms in 50, only while the app stays in front). Unplug: the
   discharge starts at the unplug. It runs until the watch powers itself off
   at the low battery level, as usual: days with the normal load (1 mA
   lasts about 8 days of a 200 mAh cell), hours with the heavy one. The
   heavy load stops at 2%, when the app is left, or when plugged in. Plugging
   in ends the discharge; one that did not reach empty is marked so.
3. Plug in and hold the power button to power on. The test resumes, records
   the discharge from its last samples and logs the recharge until the charge
   ends, another 2.5 to 3 hours.
4. The results stay on screen, and in `battery test status`, until a new
   test (hold Select) or `battery test cancel`.

The results: **Out**, the charge delivered from full to the power-off, by
QH and by the integral (sum), the time and the average current, the
temperature range and the load (Normal, Heavy or Mixed); the same as
percentages of the 200 mAh of `main.h` and the 280 mAh a comment in
`Pmic.cpp` gives; **In**, the charge put back from the end of the discharge,
including what the watch used while off; the gauge's FullCapRep and
FullCapNom at the end, what it learned, with their values at the first full
in brackets, its design capacity and its cycle count. QH and the sum should agree within a few percent under the heavy
load; a reset of the gauge in between is reported, the sum standing in for
it.

In the shell (debug builds):

| Command | |
| --- | --- |
| `battery test status` | The phase or the results, the log's size, the charge bridged over gaps by QH |
| `battery test log` | The log, as CSV |
| `battery test start` | Start a new test |
| `battery test cancel` | Cancel the test and delete its log and results |

The CSV has a line a sample (every second or fourth one once the log of
8192 records has filled and been thinned out): `time` (UTC seconds),
`v_mv`, `avg_v_mv`, `i_ua` and `avg_i_ua` (positive discharging),
`soc_cpct` (RepSOC in hundredths of a percent), `repcap_uah`,
`fullcaprep_uah`, `qh` (raw count), `temp_mc`, `flags` (1 plugged in, 2
charge done, 4 empty, 8 heavy load on, 16 QH reset), `phase` (1 charge, 2
full, 3 discharge, 4 recharge, 5 done), `loads` (seen in the discharge: 1
normal, 2 heavy), and `out_uah` and `sum_uah`, the charge out of the battery
since full by QH and by the integral. The reasons are in the decision log,
from 261126-190847-01.

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

## Tethered stub

{doc}`tether` describes a small firmware, installed like any image, that
stands in for a debug probe: over the USB-C connector, `tools/una/tether/tether.py`
reads and writes memory and registers, drives the I2C buses and the GPIO,
runs experiments loaded into SRAM, and flashes a PebbleOS image linked for
the slot after it (`CONFIG_BOARD_UNAWATCH_TETHER_SLOT`) and starts it,
without the bootloader menu.

(unawatch-status)=

## Status

| | |
| --- | --- |
| **Runs on a watch** | The first port (`b697864b4`): clocks, display and frontlight, buttons, haptics, storage and resources in the embedded flash; the watch has the 4 MiB STM32U5A5 |
| **Loops at boot on a watch** | The storage and the system resources on the eMMC (`4ff00ebca`), and so main; the cause is being looked for with the Lab (decision 261126-232719-15) |
| **Built, not yet run on a watch** | The eMMC driver on its own, the PCA9420 charger and MAX17262 fuel gauge, the boot trace (TAMP 30 and 31; the stock firmware uses 0 to 23, PebbleOS 24 to 29), the Lab |
| **Stand-ins, which nothing feeds** | Accelerometer, ambient light, microphone and die temperature are simulators: no steps |
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
- **Battery**: `charger` and `gauge` in the shell, and `battery status`,
  show what the drivers read. Check that the charge stops at about 4.20 V and
  14 mA (`gauge` voltage and current), that the gauge's temperature is
  plausible, that plugging in and out shows at once (the PMIC interrupt) and
  that the percentage follows the stock firmware's. A watch that shows 5% and
  does not charge cannot read its gauge.
- **CPU time**: each display update takes about 40 ms of busy CPU,
  preemptible.
- **Code space**: images of 1619 KiB in release and about 1651 KiB in debug
  builds, of the 1664 KiB a 2 MiB part takes. The battery test takes about
  6 KiB (`CONFIG_BATTERY_TEST`).

(unawatch-boot-trace)=

## Boot trace

Without a console, `CONFIG_BOOT_TRACE` (`subsys/boot_trace`) keeps across a
reset where the firmware was and how it ended: the last breadcrumb, an area
and a step, in TAMP backup register 30, a boot count and the image's number
in 31, and in the RAM record at `0x2025dc00` the last 40 breadcrumbs with their
uptime, the first CPU fault (PC, LR, CFSR, HFSR, BFAR, MMFAR, thread), the
first panic (file, line, message), values noted along the way, and the reset
flags and reboot reason the run started with. The next boot keeps the record
of the run before for the Lab. The registers survive any reset and installing
another image; the RAM record survives resets and, as the bootloader's stack
is below it, should survive an install too: the Lab says whether it found it.
A power loss clears the RAM. `CONFIG_BOOT_TRACE_BOOT_PATH` adds breadcrumbs to
main()'s stages, the MMC class and driver, the NOR window, PFS's mount and
format, the system resources' check and copy, and the battery drivers' init.

(unawatch-lab)=

## Lab firmware

The experiments firmware starts as the first port did, which boots: storage in
the embedded flash, the resources in the image, the eMMC unused at boot, no
battery drivers, the clock tree without PLL1 Q. Its **Lab** app runs
experiments on demand, each step named on screen and in the boot trace before
it runs. Build it, and pack it with the resources, for 4 MiB parts:

```shell
BOARD=unawatch/lab BUILD_DIR=build-lab docker/build.sh
python3 tools/una/pack_ota.py build-lab/pebbleos_resources.bin --max-size 0x3a0000 \
    -v "$(git describe --tags --always)" -o unapebble_lab.ota
```

Install the `.ota` alone. The Lab opens at boot on **Last boot**, the report
of the run before: the reset flags (IWDG, software, pin, BOR/power-on, ...),
the reboot reason, the last breadcrumb and which image wrote it, and from the
RAM record where it stopped and when, its fault and panic, its notes and last
steps. PCs and LRs are to be looked up in the ELF of that image. Back shows
the experiments; Select runs one, Up and Down scroll, Select again runs it
again. Those that change something ask for a second Select.

| Experiment | What it does |
| --- | --- |
| Last boot | The report above |
| Clocks | PLL1 and its outputs, the eMMC's kernel clock, VOS, flash latency |
| B's clocks | At runtime, the CPU on MSIS, PLL1 set up with Q at 80 MHz, back on PLL1, the eMMC on it, as B at boot |
| eMMC on HSI48 | The Lab's eMMC clock, 24 MHz default speed, 48 MHz High Speed |
| eMMC step by step | On the registers: supply, 400 kHz, CMD0, CMD1 (OCR), CID, RCA, CSD, select, EXT_CSD (size, card type, partition config, power classes, reliable writes), four lines, default speed then High Speed each checked, reads at 409600, a write and read back at 600000, power off |
| eMMC via driver | The same through the MMC class and driver the eMMC builds use, and the idle power-off |
| eMMC supply cut | The supply cut under the driver as the eMMC builds' board did at boot: does the driver recover? |
| NOR window | The NOR emulation on scratch blocks: erase, program, read back, timed for a 16 MiB format |
| Update drive | fat_ro's mount of the update drive, its root files, the packs and the version check, no copy |
| Copy resources | The matching pack into the eMMC builds' resource bank 0, compared with the file |
| B's storage | Read only: their resource banks, core dump storage and filesystem page headers |
| B's logs | Read only: the newest generation of their flash log, the last boot of an eMMC build |
| Battery | The gauge and the charger read raw, without their drivers: what the PMIC charges to |
| Test: fault, assert, watchdog, reset, power off | Each ends the run a way, to check what the next boot's report shows |

The Lab writes to the eMMC only in its GUI region: its tests at sectors
600000 and up, past the eMMC builds' window, and the resource copy into their
bank 0. As with the first port, no charger driver caps the charge: do not
leave the watch charging on it.

To follow an eMMC build that loops, install the traced one (breadcrumbs on its
boot path), let it loop, hold the power button for 4 s as it restarts to get
the bootloader menu, install the Lab and read **Last boot**, then **B's
logs** and **B's storage**.

## Decisions

The reasons behind the port's design are recorded in the
[decision log](decisions/index.md). Read it before changing something it
covers, and add a record for each new decision.

```{toctree}
:hidden:

hardware.md
tether.md
decisions/index.md
```
