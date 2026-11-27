# UNA Watch (unawatch)

## Overview

:::{div} board-card

```{thumbnail} images/watchface-tictoc.webp
```

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
battery: its charger and its fuel gauge, and the motion and environment
sensors: the accelerometer, the magnetometer and the barometer. Bluetooth,
the GNSS, the heart rate sensor and USB come next; see
[](#unawatch-status). The first port, with its storage in the embedded flash,
boots on a watch, and so does the firmware with the storage on the eMMC,
started from the tether slot: it ran 11 minutes without a reset.

{doc}`hardware` describes the hardware as the stock kernel drives it, with
the sources of each detail.

## Photos

PebbleOS on a UNA Watch, started from the tethered stub's slot. The buttons
are Up and Down on the left, Select (also the power button) and Back on the
right; the bezel's START and STOP labels are the stock firmware's.

```{thumbnail} images/watchface-tictoc.webp
:title: The TicToc watchface.
```

```{thumbnail} images/launcher-settings.webp
:title: The launcher, charging on USB.
```

```{thumbnail} images/launcher-music.webp
:title: The launcher, scrolled.
```

```{thumbnail} images/launcher-watchfaces.webp
:title: The launcher, scrolled further.
```

```{thumbnail} images/launcher-battery-test.webp
:title: The launcher on the Battery test app.
```

```{thumbnail} images/fully-charged.webp
:title: Fully charged, plugged in.
```

```{thumbnail} images/health-prompt.webp
:title: The Health app, which asks for the phone app.
```

The tethered stub, which stands in for a debug probe (see {doc}`tether`):

```{thumbnail} images/tether-stub.webp
:title: The stub, connected over USB at high speed.
```

## Hardware

- **SoC**: ST STM32U5A5QJ, an Arm Cortex-M33 at 160 MHz with single-precision
  FPU and DSP extension, 4 MiB flash and 2.5 MiB SRAM, TrustZone off,
  confirmed on a watch. Some sources mention the STM32U595QI, with 2 MiB of
  flash, for early watches.
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
`0x20270000`. The flash below it is left alone. The embedded flash holds the
code and the system resources, no storage:

| Flash | Size | Contents |
| --- | --- | --- |
| `0x08000000`–`0x08027fff` | 160 KiB | Stock bootloader (`bootloader`) |
| `0x08028000`–`0x0805ffff` | 224 KiB | Shared libc of the stock kernel (`una-libc`) |
| `0x08060000`–`0x0827ffff` | 2176 KiB | PebbleOS (`code`) |
| `0x08280000`–`0x083fffff` | 1536 KiB | System resources (`system-resources`), read in place |

The board's partitions are in `boards/unawatch/unawatch.dts`, where `code` is
the rest of the flash, which the bootloader installs to;
`pebble/boards/unawatch.overlay` splits it between the code and the resources.
The image, `pebbleos_resources.bin`, is the code, erased flash up to
`0x08280000`, then the resources, about 3061 KiB in all, of the 3712 KiB the
bootloader takes. The code takes about 1617 KiB in a release build and 1649
KiB in a debug one, Moddable XS and the battery test included, leaving some
500 KiB; the resources take 885 KiB of their 1536 KiB. The images need the 4
MiB part, which the watch has.

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
| `0x0000000` | 8 MiB | Free: the system resources of earlier builds |
| `0x0800000` | 4 × 8 KiB | `safe-firmware` (only protected), `mfg-info`, `tzinfo`, `shared-prf-storage` |
| `0x0900000` | 4 MiB | Core dumps (`scratch`), which take the 2424 KiB of RAM in use |
| `0x0d00000` | 512 KiB | Flash logs (`debug-db`), as large as they go |
| `0x1000000` | 16 MiB | Filesystem (PFS, `filesystem`) |

The bootloader does not touch any of it, so **settings, apps and logs survive
an install**, and a switch to the stock firmware and back.

The eMMC (`drivers/mmc/stm32_sdmmc.c`) runs on four data lines, at 40 MHz in
High Speed from the 80 MHz PLL1 Q, as the stock firmware does. Its supply,
EMMC_EN, is switched off once it has been idle for 2 s
(`CONFIG_MMC_IDLE_OFF_MS`) and on again by the next access: about 100 ms to
come up (36 ms for the supply, 42 ms of CMD1 busy), and the card's first
write after a power-up takes 110-125 ms, later ones 2-4 ms
(decision 261127-130125-03). The flash (`drivers/flash/mmc_nor.c` with `drivers/flash/blk_nor.c`)
has NOR semantics on the 512-byte blocks: a write reads its blocks, clears the
bits and writes them back before it returns, and an erase writes blocks of
0xFF. Writes are reliable writes, which a power loss leaves either done or not.
Nothing is held in RAM that a reset would lose; the 16 blocks last read or
written in part are cached for reads.

### System resources

The system resources come with the code, in the same `.ota`: the bootloader
writes them to `0x08280000` with the image, and the firmware reads them there
in place (`CONFIG_RESOURCES_IN_CODE_FLASH`, with `CONFIG_RESOURCES_IN_IMAGE`
and `CONFIG_MMAP_RESOURCES`), as the first port did. They are one bank, which
nothing writes: put bytes and the console's imaging have no resource bank to
write to. At boot the firmware checks their version and CRC; without them, as
after flashing `pebbleos.bin` alone, it logs the version it needs and powers
the watch off: there is no recovery firmware to go to, and a reset would only
start it again. Copying them from a `.pbpack` on the update drive
(`CONFIG_RESOURCES_FROM_VOLUME`), as earlier builds did, is no longer used.

## Boot

The bootloader jumps to the image with interrupts masked and its clocks and
peripherals as it used them. The SoC masks and clears every interrupt, resets
the bus masters that could still write to RAM, moves the system clock to
MSIS and turns the instruction cache on. The clock controller then selects
the SMPS and voltage range 1, and runs the CPU and every bus at 160 MHz from
PLL1 on MSIS at 48 MHz, with 4 flash wait states. The system timer is LPTIM4
on the 32.768 kHz clock; LPTIM1 and LPTIM3 are left to the display. Idle, the
CPU waits in Stop 2 (see [](#unawatch-power)).

At boot the board drives the enables of the buzzer, the GNSS, the reset of
the BLE module and, without its driver, the eMMC low; the barometer's LDO is a
regulator, which drives its enable low until the barometer's driver powers it. Debug
builds without the USB developer access drive USB_DAM (PB15) high to route
the USART3 console to the USB-C connector
(`CONFIG_BOARD_UNAWATCH_CONSOLE_DAM`); with it, the console is on USB and
USB_DAM stays low, as the stub has it (decision 261127-134824-03).

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
percentage from the gauge (`CONFIG_BATTERY_SOC_GAUGE`). The stock firmware
charging to 4.40 V says the cell is a 4.4 V (LiHV) one. PebbleOS charges it to
4.36 V, which the charger's 0.75% accuracy keeps at or below 4.393 V, never
above what the stock firmware asks, for about 1 to 2% of the capacity; exactly
4.40 V is a devicetree change away. The charge is set for the cell's life
rather than its last few percent:

| | PebbleOS | Stock firmware |
| --- | --- | --- |
| Charge current | 100 mA (0.5C) | 150 mA |
| Charge voltage | 4.36 V | 4.40 V |
| Termination | 14 mA, 6.4 min top-off | 10 mA, 19.2 min top-off |
| Pre-charge, dead battery | 8 mA, 4 mA | PMIC defaults |
| Safety timers | 5 h fast, 30 min pre-charge | 9 h fast |
| Recharge | 240 mV below the charge voltage | 140 mV |
| Charge temperature | 0 to 45 degrees C at the gauge, 3 degrees of hysteresis | stops above 45 degrees C ambient or 55 at the gauge |
| Input current limit | 255 mA | 255 mA |

The gauge runs the stock firmware's EZ model, register for register, for a
cell charged above 4.25 V (ModelCFG.VChg = 1). The model is loaded only after
a power-on reset of the gauge or when its registers hold another model, so
switching firmware keeps what it learned of the cell. Charging starts off at every boot
and is allowed once the gauge reads a temperature in range; a gauge that
cannot be read keeps it off. Unplugged, the watch shuts down below 1% or at
an averaged 3.35 V, as the stock firmware does; it shows 5% until the gauge
first answers. `CONFIG_CHARGER_PCA9420_CHARGE_LIMIT_MV` stops the charge short of
full, at a lower voltage, for a longer life still, 4100 for about 75 to 80%.
The charge voltage holds while PebbleOS runs: whether a watch that is off
charges, and to what, is not known yet.

The interrupt of the PMIC, on PG7 with the STUSB4500's alert, reports plugging
in and out; the battery service also polls every 30 s plugged in, 60 s
otherwise. The STUSB4500 is left on its factory configuration. The reasons for
each value are in the [decision log](decisions/index.md), from
261126-173552-01.

## Battery capacity test

The **Battery test** app measures the charge the cell gives from a full
charge, to 4.36 V, to the low battery power-off at 3.35 V, its usable
capacity, and the
charge it takes back. It counts with the gauge's coulomb counter (QH, 0.5 mAh a
count) and, as a cross-check, an integral of the gauge's averaged current;
both are shown. It samples on the battery service's polls, a minute apart,
logs to the filesystem on the eMMC and carries on across the power-off, so it
keeps running when the app is closed. It only reads the gauge and the
charger: the charge voltage and the charger's protections stay as they are.
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

(unawatch-power)=

## Power

PebbleOS's kernel is tickless: when nothing runs, the CPU waits in the
deepest idle state no driver blocks that is worth entering before the next
timeout. On the STM32U5 that is Stop 2 (`soc/st/stm32u5/idle.c`, decision
261127-153346-01): every clock but the LSE and the LSI stops, the SRAMs are
kept, and LPTIM4, the system timer, and the VCOM's LPTIM1 and LPTIM3 run on
in the SmartRun domain; the timer, the buttons and the PMIC wake the CPU,
which then relocks PLL1. The CPU stays in Sleep, on PLL1, while the USB is
powered (with the developer access connected), during an I2C transfer and
during an eMMC power-up or transfer. `sys cpustats` gives the time in each
state since it was last asked, and what blocks each.

### Measured draw

Measured on a watch on its fuel gauge with `power measure` (decision
261127-150636-01): plugged in, the charger's input suspended so that the
system runs from the battery, the USB developer access held off but where
said, a debug build from the tether slot, the battery at 4.38 to 4.40 V and
30 degrees C, windows of 5 to 10 minutes. The gauge's Current, sampled every
2 s, has a spread of about 4 mA whatever the load, so a 10 minute mean is
good to about 0.25 mA; its AvgCurrent and, over long windows, its coulomb
counter agree with it.

| State | Sleep only (before) | Stop 2 |
| --- | --- | --- |
| TicToc, a minute-tick face, display static | 5.9 to 6.0 mA, 99% in Sleep | 0.9 to 1.0 mA, 98% in Stop 2 |
| Launcher, static | 6.3 mA | |
| TicToc, the eMMC kept powered and idle (`CONFIG_MMC_IDLE_OFF_MS=0`) | | 0.9 to 1.1 mA: the idle card is below what the gauge resolves |
| An animation (Bouncing Box demo, about 15 frames a second) | | 10.5 mA, CPU 100% busy |
| USB developer access connected, at high speed | 17.0 mA (launcher) | 19.1 mA (TicToc): Stop blocked by the USB |

The tethered stub draws 31.5 mA: it polls, with the USB at high speed and
the frontlight on. A frame costs about 0.6 mA s (63 ms of CPU), so a second-tick face would
draw about 1.6 mA, estimated from the animation rather than measured: no
built-in face ticks every second.

### Battery life

On the 200 mAh of UNA's `main.h` (280 mAh by a comment in `Pmic.cpp`),
from the draws above:

| | Draw | 200 mAh | 280 mAh |
| --- | --- | --- | --- |
| Minute-tick face, idle, before | 6 mA | 1.4 days | 1.9 days |
| Minute-tick face, idle, Stop 2 | 1.0 mA | 8 days | 12 days |
| Second-tick face (estimated) | 1.6 mA | 5 days | 7 days |
| USB developer access connected | 17 to 19 mA | 11 hours | 15 hours |

Use of the watch adds to it: an animation draws 10 mA, and the frontlight,
not measured, more.
A slot build forces the developer access on (`CONFIG_DEV_USB_FORCE`), so on
battery it keeps the USB powered and stays out of Stop; an installed debug
build follows VBUS, and a release build has no USB.

Where the rest of the 1 mA goes is not known yet: the CPU in Stop 2 should
take tens of microamps, so most of it is the board (the PMIC's regulators,
the panel, the modules' standby). The next wins are there, and in the
display, whose frames are clocked out on the CPU for 63 ms each.

## Display

The 240 × 240 memory-in-pixel panel shows the centre of the 260 × 260
framebuffer of the gabbro platform, so that apps for the Pebble Round 2 run
unchanged, losing a 10 pixel ring at the edge; a platform of its own would
need apps built for it. The display is `jdi-parallel-gpio`
(`drivers/display/jdi_gpio.c`): the CPU clocks every frame out on the GPIO
as the stock firmware does, the panel turned a quarter round, with the
timings of the stock driver. A frame takes 63 ms of CPU, every update the
whole frame, clocked out by a thread of the driver, `Display`, at the
background work queue's priority, so that animations take only the CPU left
over (decision 261127-130125-01): clocked on KernelMain, the charging
animation starved SysWorkq until the task watchdog reset the watch. The panel, `sharp,ls012b7dd06a` on the memory-in-pixel
panel driver, takes its VCOM from LPTIM1 (FRP) and its inverse from LPTIM3
(XFRP), at 30.7 Hz on the LSE. The frontlight is on PF3, on or off.

## Buttons and haptics

| Button | Pin | Key |
| --- | --- | --- |
| Bottom right | PD8 | Back |
| Top left | PD4 | Up |
| Top right, power | PD15 | Select |
| Bottom left | PA5 | Down |

Back and Select held for 5 s reset the watch at once, whatever runs as long
as PebbleOS's timers do (`pebble/system/reset_keys.c`, decision
261127-134824-08); a slot build then starts the tether stub. With Up held as well, PebbleOS
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
python3 tools/una/pack_ota.py build-una-fw/pebbleos_resources.bin --max-size 0x3a0000 \
    -v "$(git describe --tags --always)" -o unapebble.ota
```

or `pbl configure --board unawatch -DCONFIG_RELEASE=y && pbl build`. The
build writes `pebbleos.bin`, the code, `system_resources.pbpack`, the
resources, and `pebbleos_resources.bin`, the two at their addresses, which is
what gets packed. The packer takes up to 3712 KiB, the end of the 4 MiB
flash, its default.

### CI and releases

Development happens in a Forgejo repository, which push-mirrors every branch
and tag to [GitHub](https://github.com/aquarat/unapebble). GitHub is read-only:
the mirror overwrites anything pushed or merged there (decision
261127-144548-03).

- **Forgejo**: `.forgejo/workflows/unawatch.yml` builds the release firmware
  and the tether stub on each push to main and `v*` tag, and publishes the
  `.ota`, ELF, HEX, log dictionary, tether files and `SHA256SUMS` as the
  rolling prerelease `unawatch-latest` and a release per `v*` tag. Forgejo
  runs only `.forgejo/workflows` when it exists, never `.github/workflows`.
- **GitHub**: `.github/workflows/unawatch.yml` builds the same with the same
  `docker/setup.sh`, on pushes to main and on pull requests, and publishes
  GitHub Releases on the tags the mirror pushes, never moving one: the
  prerelease `unawatch-latest` when Forgejo moves that tag after its build,
  so both rolling releases are of the same commit, and a release per `v*`
  tag (decision 261127-144548-02). Upstream's builds of its own boards and
  its unit tests run there too (decision 261127-144548-01).

## Installing

1. Turn the watch off, then hold the power button (top right) for 4 s as it
   turns on: the bootloader menu appears.
2. Connect USB: the update drive appears. Copy the `.ota`, the only file
   needed, to its root, under a name ending in lowercase `.ota`.
3. Eject the drive, unplug, and choose **Continue**. The bootloader checks
   the file, erases the flash from `0x08060000`, writes the image with its
   resources, deletes the `.ota` and starts it.

A `.pbpack` left on the drive by an earlier build is no longer used, and may
be deleted. The battery must be above about 10% for the bootloader to look
for updates. The image can also be flashed over SWD:
`pebbleos_resources.bin` at `0x08060000`, with any probe; `pebbleos.bin`
alone leaves the resources out, and a firmware without them powers off.

## Recovering

The bootloader runs first on every boot and only checks the stack pointer of
the image, so a firmware that does not boot does not stop it. Hold the power
button for 4 s as the watch turns on, and install UNA's own `.ota` the same
way. PebbleOS only writes to the GUI region of the eMMC, which the stock
firmware does not use; its update drive and files are left alone.

## Tethered stub and USB developer access

{doc}`tether` describes a small firmware, installed like any image, that
stands in for a debug probe: over the USB-C connector, `tools/una/tether/tether.py`
reads and writes memory and registers, drives the I2C buses and the GPIO,
runs experiments loaded into SRAM, and flashes a PebbleOS image linked for
the slot after it (`CONFIG_BOARD_UNAWATCH_TETHER_SLOT`) and starts it,
without the bootloader menu. PebbleOS's debug and slot builds have the
console, the shell with `reboot`, and the same protocol on USB, behind a
setting (`CONFIG_DEV_USB`): {doc}`tether` too.

(unawatch-status)=

## Status

| | |
| --- | --- |
| **Runs on a watch** | The first port (`b697864b4`): clocks, display and frontlight, buttons, storage and resources in the embedded flash; the watch has the 4 MiB STM32U5A5 |
| **Runs on a watch, from the tether slot** | The current firmware, linked for the slot: storage on the eMMC (with the write timeout, decision 261127-123202-01, and the display thread, 261127-130125-01), the system resources read in place from the embedded flash, the PCA9420 charger and MAX17262 fuel gauge (charge registers read back at 4.36 V while it runs), the boot trace, and the USB developer access ({doc}`tether`); 11 minutes and 5 minutes without a reset, the watchface ticking |
| **Built, not yet run on a watch** | The same firmware installed as one `.ota` through the bootloader (it differs only in its address); the Lab; the Battery test app (it is in the launcher, but no test has run); the USB Developer setting in a normal install |
| **Runs on a watch, from the tether slot: the sensors** | The BMI270 accelerometer (FIFO and interrupt, shakes), the BMM350 magnetometer and the MS5837 barometer, read on the watch lying still (see [](#unawatch-sensors)); not yet moved: steps, shakes, the compass's calibration and the X and Y signs need someone to move the watch |
| **Stand-ins, which nothing feeds** | Ambient light, microphone and die temperature are simulators |
| **Off** | Bluetooth runs on the fake controller, as on boards without a radio: no phone connection, so apps and updates only come with the image. No heart rate, GNSS or buzzer; USB only for development |
| **Unknown** | Whether the haptics work: their rail and driver have not been checked on the watch. The panel is round, and the computed round mask fits it (see the photos) |

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
- **Resources**: read in place from `0x08280000`; the log says `Valid system
  resources found!`. A watch that turns itself off after the splash did not
  find them, which only an image flashed without them should do.
- **Clocks**: PLL1 now also gives 80 MHz on Q for the eMMC, which the SoC
  start-up turns off first, whatever the bootloader left running.
- **Battery**: `charger` and `gauge` in the shell, and `battery status`,
  show what the drivers read. Check that the charge stops at about 4.36 V and
  14 mA (`gauge` voltage and current), that the gauge's temperature is
  plausible, that plugging in and out shows at once (the PMIC interrupt) and
  that the percentage follows the stock firmware's. A watch that shows 5% and
  does not charge cannot read its gauge.
- **CPU time**: each display update takes 63 ms of busy CPU, on the
  `Display` thread at priority 1; an animation runs at about 15 frames a
  second and takes the CPU left over. Idle, the CPU is in Stop 2 about 98%
  of the time, unless the USB is connected (`sys cpustats`).
- **Code space**: code of 1617 KiB in release and 1649 KiB in debug builds,
  of the 2176 KiB before the resources. The battery test takes about 6 KiB
  (`CONFIG_BATTERY_TEST`).

(unawatch-sensors)=

## Sensors

The motion and environment sensors have PebbleOS drivers, which its own
services use: the accelerometer service for apps, shakes, health's step
counting and activity, the compass and the pressure service. The reasons are
in the decision log, from 261127-194845-01, the measurements in
{doc}`hardware`, sections 2.6.1 and 2.7.1.

| Sensor | Bus | Driver | How it runs |
| --- | --- | --- | --- |
| BMI270 accelerometer | I2C4 `0x68`, INT2 on PA1 | `drivers/imu/bmi270` | Low power mode, 4 samples averaged, ±4 g, advanced power save; samples through the FIFO at its watermark interrupt, 12.5 to 100 Hz in batches of up to 128 (the accelerometer service's 125 at 25 Hz: an interrupt every 5 s); shakes from the feature engine's any-motion, which runs the part at 50 Hz; no double tap. Bosch's configuration file is loaded at boot |
| BMM350 magnetometer | I2C4 `0x14` | `drivers/imu/bmm350` | Suspended unless sampled; sampled in its normal mode, read at its output data rate and compensated with the calibration in its OTP. The interrupt, on PC2, is not used |
| MS5837-02BA01 barometer | I2C2 `0x76` | `drivers/pressure/ms5837.c` | Powered at boot through its LDO (ALTIMETER_EN, PG0, a `regulator-fixed`): 20 ms off, 10 ms on; a read converts the pressure and the temperature at OSR 4096, 9 ms each |

The axes are PebbleOS's: X to 3 o'clock, Y to 12 o'clock, Z out of the face;
the accelerometer gives the gravity vector, z = -1000 mg face up. The heart
rate sensor and the GNSS are not covered here.

In the shell (debug builds):

| Command | |
| --- | --- |
| `imu accel read`, `imu accel info` | One sample; the rate, batch and detections in use |
| `imu accel stream SECONDS [INTERVAL_US [BATCH]]` | Takes the samples from the accelerometer service for a while; alone, shows the batches, rate, shakes and per axis the mean, noise and range; `stop` ends it |
| `bmi270` | The part's mode and the driver's counts: interrupts, FIFO reads, samples, overruns, shakes, watchdog services, bus errors |
| `imu mag read`, `imu mag info` | One sample, in milligauss, sampling briefly if nothing samples |
| `pressure` | One conversion, in pascals |

Checks that need someone to move the watch: steps counted while walking with
it, shakes (a flick of the wrist, `bmi270` counts them), the X and Y signs
(3 o'clock down reads x = +1000 mg, 12 o'clock down y = +1000 mg), and the
compass, its calibration (turning the watch round every axis) and its bearing.

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
| Copy resources | The matching pack into the earlier eMMC builds' resource bank 0, now free space, compared with the file |
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
