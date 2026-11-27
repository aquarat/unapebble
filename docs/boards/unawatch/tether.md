# UNA Watch: tethered stub

The tethered stub is a small bare-metal firmware that stands in for the debug
probe the watch has no connector for. Installed like any image, it runs from
`0x08060000`, shows its state on the display and presents a serial port on the
USB-C connector, through which `tools/una/tether/tether.py` reads and writes
memory and registers, runs I2C transfers and GPIO, loads experiments into SRAM
and runs them, and flashes and starts a PebbleOS image without going through
the bootloader menu each time.

It lives in `tools/una/tether`: the stub in `stub/`, example experiments in
`experiments/`, the host tool `tether.py`, the TCP bridge `bridge.py`, and a
host build of the stub's protocol code in `sim/` that the tests run against.
None of it has run on a watch yet. The reasons behind it are in the
[decision log](decisions/index.md), from 261126-232318-01.

## Building

In the build container, from the repository root:

```shell
docker/build.sh -- make -C tools/una/tether          # stub and experiments
docker/build.sh -- make -C tools/una/tether test     # host tool tests
```

This writes `tools/una/tether/build/tether.ota` (the stub, packed by
`tools/una/pack_ota.py`), `tether.elf`, and `build/experiments/*.bin`. The
stub takes about 21 KiB of flash. The `.forgejo/workflows/unawatch.yml`
workflow builds it too and publishes `unapebble_tether_<version>.ota`, its
ELF and `unapebble_tether_<version>_host.tar.gz` (the host tools, the
experiments and their sources) with each release.

## Installing

1. Turn the watch off, then hold the power button (top right) for 4 s as it
   turns on: the bootloader menu appears.
2. Connect USB and copy the stub's `.ota` to the root of the drive that
   appears. No `.pbpack` is needed.
3. Eject the drive, unplug, and choose **Continue**. The bootloader installs
   the stub and starts it.

From then on the watch starts the stub every time it is reset or turned on.
To go back to PebbleOS or the stock firmware, install its `.ota` the same
way; the stub only ever writes the flash above `0x08080000`, so the
bootloader, UNA's libc and the bootloader menu are always there.

## On the watch

The frontlight comes on and the screen shows, from the top:

| Line | |
| --- | --- |
| **Tether v1** | and the build's version |
| **USB wait**, **USB HS...**, **USB HS ok**, **HS open**, **USB idle**, **USB err N** | the USB state: waiting for a host, enumerating, configured (green), port opened by a program, suspended or unplugged, or bring-up failed at step N (red) |
| `cmds N err N` | commands run, and failed |
| the last command | e.g. `READ 20000000`, `RUN 20020031`, with `!` when it failed |
| a note | text sent with `tether.py note` |
| **no fault**, or the last fault (red) | its kind, program counter and CFSR |
| `reset by ...` (red) | a fault the stub could not recover from, which reset it |
| the clock | 160 MHz, or 48 MHz if PLL1 failed; `FS` when full speed was forced |
| **Up: boot slot** or **slot empty** | whether a PebbleOS image is in the slot |

The frontlight blinks off briefly with each command, which shows activity
even if the display stays blank. The screen is only redrawn when the USB is
quiet, as a frame takes about 35 ms of CPU.

| Buttons | |
| --- | --- |
| Back and Select held 2 s | reset (the stub starts again) |
| Up, pressed and released | start the PebbleOS image in the slot |
| Down held while the stub starts | USB at full speed only, should high speed not work |
| any button | after an experiment drew on the screen: the stub's own screen again |

USB bring-up steps, for **USB err N**: 1 the HSE crystal did not start,
2 the core's AHB never went idle, 3 its soft reset did not end, 4 it did not
enter device mode.

## Connecting

The stub is a USB CDC-ACM device, `1209:0001` (the pid.codes test ID), named
"UNA Watch tether" with the chip's unique ID as serial number: on Linux
`/dev/ttyACM*` (and `/dev/serial/by-id/usb-unapebble_UNA_Watch_tether_*`), on
macOS `/dev/cu.usbmodem*`. No driver is needed. `tether.py` finds it by its
USB IDs on Linux (sysfs) and macOS (`ioreg`), by name otherwise; `--port` or
`$TETHER_PORT` names it. PebbleOS's two ports (see
[](#unawatch-dev-usb)) are `1209:0002`: `tether.py` takes whichever of the
stub's port and PebbleOS's tether port is there.

```shell
python3 tools/una/tether/tether.py info
```

### From another machine

When the watch is plugged into a Mac and the experiments run elsewhere, the
Mac serves the port on TCP with `bridge.py` (Python 3, standard library only;
copy `bridge.py`, `tether.py` and `console.py` together). Port 4500 carries
the tether protocol, of the stub or of PebbleOS, whichever runs; port 4501
PebbleOS's console:

```shell
# on the Mac: only reachable through an SSH tunnel
python3 bridge.py
# or on the network, with no authentication at all
python3 bridge.py --listen 0.0.0.0:4500
```

and the other machine connects with `--tcp` (or `$TETHER_TCP`), through the
tunnel or directly:

```shell
ssh -N -L 4500:127.0.0.1:4500 -L 4501:127.0.0.1:4501 mac.local &
python3 tools/una/tether/tether.py --tcp 127.0.0.1:4500 info
python3 tools/una/tether/console.py --tcp 127.0.0.1:4501 'battery status'
```

The bridge serves one client at a time, a new one replacing the last, and
opens the port again when the watch comes back after a reset, so a client
stays connected across `tether.py reset`. `socat` does the same job:
`socat TCP-LISTEN:4500,reuseaddr,fork FILE:/dev/cu.usbmodemXXXX,raw,echo=0`.

(unawatch-dev-usb)=

### PebbleOS's USB developer access

With **Settings > System > Debugging > USB Developer** on (always on in a
slot build), PebbleOS is a USB device too, `1209:0002`, "UNA Watch
PebbleOS", with two serial ports: the console, logs and shell, on interface
0 ("UNA Watch PebbleOS 0", on Linux `/dev/serial/by-id/*UNA_Watch_PebbleOS*-if00`,
on macOS the `cu.usbmodem…1`), the tether protocol on interface 2 (`…-if02`,
`cu.usbmodem…3`). It is built in debug and slot builds
(`CONFIG_DEV_USB`), not in release `.ota` builds, and powered only while the
watch is plugged in (`CONFIG_DEV_USB_FOLLOW_VBUS`), but in slot builds. While
it is on, anyone with a cable can read and write the memory: decision
261127-134824-01.

The tether protocol is the stub's, from the stub's own `proto.c`, but for
`run`, `call`, `flash`, `boot`, which PebbleOS refuses as unsupported:
`tether.py flash` against PebbleOS resets it first and flashes through the
stub, which a slot build's reset returns to. `info` says `firmware
PebbleOS …`. Writes to the system control block and the MPU
(`0xe000ed00`–`0xe000edff`) are refused on top of the stub's refusals; a
fault on a bad address is caught and reported as the stub reports it.
`reset` restarts PebbleOS, `poweroff` powers it off. I2C runs at the bus
driver's speed whatever `--fast` says.

`console.py` runs one shell command and prints what it printed until the
prompt comes back, or follows the logs:

```shell
python3 tools/una/tether/console.py 'battery status'        # the port found by its IDs
python3 tools/una/tether/console.py --tcp 127.0.0.1:4501 gauge
python3 tools/una/tether/console.py --tcp 127.0.0.1:4501 reboot
python3 tools/una/tether/console.py --tcp 127.0.0.1:4501 --follow --for 60
```

The shell's commands for it:

| Command | |
| --- | --- |
| `reboot` | restart: through the bootloader to the stub in a slot build, to PebbleOS otherwise |
| `reboot off` | power off through the bootloader |
| `reboot menu` | power off: hold the power button 4 s at power-on for the menu, which nothing else can ask for ({doc}`hardware`, section 9.6) |
| `usb`, `usb on`, `usb off` | the setting, the charger input and the USB's state; turn the setting on or off |

## tether.py

| Command | |
| --- | --- |
| `info` | stub version, IDCODE, flash size, option bytes (read only), unique ID, clocks, reset cause (`RCC_CSR` at boot), USB state, faults |
| `peek ADDR [N] [-w 1/2/4]` | N values |
| `poke ADDR VALUE... [-w 1/2/4]` | write values |
| `dump ADDR LEN [-o FILE]` | hex dump, or to a file |
| `load ADDR FILE` | a file to RAM, checked |
| `fill ADDR VALUE COUNT [-w]` | |
| `crc ADDR LEN` | zlib's CRC-32 of a range |
| `run BLOB [ARGS...] [-t S]` | load an experiment, run it, print its output and r0 |
| `call ADDR [ARGS...]` | call code with r0-r3 (`--anywhere` outside the load region) |
| `i2c BUS ADDR [-w HEX] [-r N] [--fast]` | write, then read after a repeated start; 100 kHz, `--fast` 400 kHz |
| `i2c-scan BUS` | |
| `gpio PIN [get/high/low/od-high/od-low/in [up/down]/analog]` | e.g. `gpio PF3 low` (frontlight on) |
| `flash IMAGE [--boot]` | erase, program and check a PebbleOS image in the slot |
| `boot [ADDR]` | start the slot's image |
| `note TEXT` | a line on the screen |
| `reset`, `poweroff` | the second through the bootloader, as PebbleOS powers off |
| `clear-fault` | forget the recorded faults |

Buses: 1 main (PMIC `0x61`, fuel gauge `0x36`, USB-PD `0x29`, haptics `0x5a`,
display EEPROM `0x50`), 2 body, 3 BLE module, 4 GPS/IMU module
({doc}`hardware`, section 3.2). The module is also usable from Python:
`tether.Tether(tether.open_transport())` has a method per command.

## Safety

The stub refuses, whatever the host asks:

- any write to the code region below `0x10000000` (the flash, its secure
  alias, the system memory) except the slot, programmed through the flash
  commands; erasing or programming below `0x08080000`;
- writes to the flash controller (`0x40022000`, `0x50022000`), so the option
  bytes are never touched, to the stub's own 128 KiB of RAM and to the USB
  controller; reads that would pop the USB FIFOs;
- I2C writes beyond a register address to the PMIC, the fuel gauge and the
  USB-PD controller unless `--unsafe` is given: the stub itself never writes
  them, and does not change the PMIC or the charger;
- calls outside the load region unless `--anywhere`; GPIO changes on PA11 to
  PA14 (USB and SWD).

At start it switches off what the bootloader may have left on, as PebbleOS
does: eMMC (`EMMC_EN` low), buzzer, barometer LDO, BLE (in reset), GNSS,
haptics, and the debug accessory switch (`USB_DAM` low). It feeds the
independent watchdog, in case something started it. Experiments are
read-only by default, and an eMMC write must stay within the GUI region,
sectors 409600 to 614399 (`tether_emmc_write_allowed()`).

Recovery is always the bootloader menu: hold the power button 4 s as the
watch turns on. If the stub hangs, holding the power button resets the
watch through the PMIC's long press (16 s once the stock firmware has set
it), whatever runs.

## Experiments

An experiment is a small program linked into SRAM at `0x20020000`, run by the
stub on its own stack (top `0x2025e000`), privileged, with the stub's SysTick
running. `experiments/` has three: `rcc_clocks` (the clock tree, decoded),
`max17262` (the fuel gauge's and the charger's registers) and `emmc_cid`
(powers the eMMC, reads its CID, CSD and EXT_CSD at 400 kHz on one line, and
a sector given as argument, then powers it off). All three only read.

```shell
docker/build.sh -- make -C tools/una/tether experiments
python3 tools/una/tether/tether.py run tools/una/tether/build/experiments/emmc_cid.bin 409600
```

To write one, add `experiments/NAME.c` and its name to `EXPERIMENTS` in the
`Makefile`:

```c
#include "tether_blob.h"

uint32_t blob_main(const uint32_t *args, uint32_t nargs) {
  uint8_t reg = 0x09, v[2];
  if (tether->i2c(1, 0x36, &reg, 1, v, 2, 0) != 0) {
    return 1;
  }
  tprintf("VCell raw %04x\n", v[0] | (v[1] << 8));
  return 0;
}
```

`blob_crt0.c` puts a header first (magic, load address, entry, size, name),
clears `.bss` and calls `blob_main` with the arguments given to `run`;
`tether.py` checks the header, writes the image, checks its CRC and calls
the entry. Its return value comes back as r0, what it prints (up to 16 KiB)
as its output. `stub/tether_api.h` lists what the stub gives it in `tether`:
`printf`, delays, milliseconds and cycles, I2C, the watchdog, the 240 x 240
ARGB2222 framebuffer with text drawing and a display update, and IRQ
handlers in the stub's vector table. After the call the stub disables every
IRQ, drops the handlers and restores its SysTick.

A fault in an experiment (HardFault, BusFault, MemManage, UsageFault, an
ECC NMI) returns to the stub with the registers, the fault status registers
and the faulting address, which `tether.py` prints decoded; so does an
experiment that runs past its timeout (10 s by default, `-t 0` for none),
with where it was. Peeks and pokes of bad addresses are caught the same way.
What cannot be recovered resets the watch; the stub then shows `reset by`
and reports the fault in `info`. Experiments must keep the USB working: no
changes to CONTROL, the USB, HSE, or HCLK below 30 MHz, and interrupts on
when they call the stub, whose I2C timeouts count milliseconds.

## Chain-booting PebbleOS

PebbleOS can be linked for the slot after the stub, at `0x08080000`, then
flashed over USB and started without the bootloader menu:

```shell
BOARD=unawatch BUILD_DIR=build-slot docker/build.sh -DCONFIG_RELEASE=y \
    -DCONFIG_BOARD_UNAWATCH_TETHER_SLOT=y
python3 tools/una/tether/tether.py flash build-slot/pebbleos_resources.bin --boot
```

`CONFIG_BOARD_UNAWATCH_TETHER_SLOT` moves the start of the code partition from
`0x60000` to `0x80000` (`CONFIG_FLASH_OFFSET`), leaving 2048 KiB for the code
up to the system resources at `0x280000`, where they stay.
`pebbleos_resources.bin` is the code, erased flash and the resources, about
2.9 MiB; flashing it should take some 30 s (estimated: 4 KiB a command,
programmed by quad-words, then a CRC of the whole). `pebbleos.bin` alone
flashes the code only, which is enough when the resources there are of the
same build. The stub starts it as the
bootloader would: USB disconnected, interrupts masked, the image's vector
table in VTOR and its stack pointer in MSP; PebbleOS's start-up resets the
rest. Up on the watch starts it too. A slot image is never an `.ota`: the
bootloader always installs at `0x08060000`.

Every reset or reboot goes through the bootloader to the stub again, which
then waits to be told to boot; powering off works as usual. PebbleOS reads
its system resources in place at `0x08280000`: without those of its build it
powers off.

While PebbleOS runs, the stub and its port are gone, so a test run has to end
for its results to be read. `-DCONFIG_BOARD_UNAWATCH_TETHER_REBOOT_S=600`
makes the image restart, into the stub, once it has run 600 s, from the high
priority work queue, so that a stuck background queue does not hold it up.
The port coming back early means the run reset by itself; then, through the
stub, the boot trace shows how the run ended (a `kernel 0x5` breadcrumb at
the set time for the restart, task watchdog or fault records otherwise) and
the flash log, in the `debug-db` partition on the eMMC, what it logged
(decision 261127-130125-02):

```shell
until ssh mac 'ls /dev/cu.usbmodem*'; do sleep 5; done  # the port is back
python3 tools/una/tether/tether.py dump 0x2025dc00 0x400 -o record.bin
python3 tools/una/tether/tether.py run emmc_read_ram.bin 436224 1024 0x20100000
python3 tools/una/tether/tether.py dump 0x20100000 0x80000 -o log.bin
```

## Protocol

Frames are the same both ways, little-endian:

| Bytes | |
| --- | --- |
| 2 | `A5 5A` |
| 1 | command; a reply sets bit 7 |
| 1 | sequence number, echoed by the reply |
| 2 | payload length, at most 4160 |
| n | payload; a reply's starts with a u32 status |
| 4 | CRC-32 (zlib's) of the command byte to the end of the payload |

The stub looks for the next `A5 5A` after a frame with a bad length or CRC,
so noise and false starts cost nothing; a damaged frame that holds no other
gets a `0xff` reply with status 1, and one left incomplete is dropped after
500 ms. `stub/tether_proto.h` is the reference, and `test_tether.py` checks
`tether.py` against it.

| Command | Request | Reply data |
| --- | --- | --- |
| `01` INFO | | `struct tether_info` |
| `02` READ | addr, len (u32), width (u8: 0 words where aligned, 1, 2, 4) | the bytes, at most 4096 |
| `03` WRITE | addr, width, 3 bytes padding, data | |
| `04` FILL | addr, width, padding, value, count | |
| `05` CRC32 | addr, len | u32 |
| `06` CALL | addr (Thumb), flags (1 blob convention, 2 anywhere, 4 keep output), timeout ms, count, up to 12 u32 arguments | `struct tether_call_result`, the start of the output, then a `struct tether_fault` if it faulted or timed out |
| `07` I2C | bus, address, flags (1 unsafe, 2 400 kHz), padding, write length (u16), read length (u16), data | the bytes read |
| `08` GPIO | port (0 = A), pin, op (0 read, 1 output, 2 input with pull 0/1/2, 3 open drain, 4 analog), level or pull | MODER OTYPER OSPEEDR PUPDR IDR ODR AFRL AFRH, then the pin's level |
| `09` RESET | 0 reset, 1 power off | (sent before) |
| `0a` FLASH_ERASE | addr, len, in 8 KiB pages | |
| `0b` FLASH_WRITE | addr, data in 16-byte units, checked after | |
| `0c` BOOT | addr, 0 for the slot | (sent before) |
| `0d` NOTE | text, 31 bytes | |
| `0e` CLEAR_FAULT | | |

Status: 0 ok, 1 bad CRC, 2 unknown command, 3 bad argument, 4 fault (the
data is a `struct tether_fault`), 5 timeout, 6 protected, 7 NACK, 8 bus
error, 9 flash error, 10 too long, 11 unsupported.

## Memory

| Range | |
| --- | --- |
| `0x08060000`–`0x0807ffff` | the stub (128 KiB, 21 KiB used) |
| `0x08080000`–`0x083fffff` | the slot, for a PebbleOS image |
| `0x20000000`–`0x2001ffff` | the stub's RAM: buffers, framebuffer, 16 KiB stack; written by no command |
| `0x20020000`–`0x2024dfff` | experiments |
| `0x2024e000`–`0x2025dfff` | their stack, also the stub's for guarded accesses |
| `0x2026ff00`–`0x2026ffff` | the fault record kept across a reset, at the top of the stock kernel's retained RAM, which it leaves unused |

## How it starts

The bootloader jumps to the stub as it would to PebbleOS, with interrupts
masked and its clocks and peripherals as it used them ({doc}`hardware`,
section 1.4). The stub records `RCC_CSR` and clears the reset flags, disables
and clears every interrupt and the EXTI lines, resets the bus masters the
bootloader may have left running (GPDMA1, DMA2D, SDMMC1, LPDMA1, then the USB
core), runs from a vector table in RAM, and moves the system clock to MSIS
before setting up PLL1 at 160 MHz as PebbleOS does, in voltage range 1 with
the booster (48 MHz on MSIS if PLL1 does not lock). It sets the loads' enables
to off, stops the VCOM the bootloader may have left running, clears the
panel, starts VCOM on LPTIM1 and LPTIM3 from the LSE, and brings up the USB:
the HSE crystal for the PHY's 16 MHz reference, the transceiver supply, the
PHY, a soft reset of the core, device mode, then 300 ms disconnected before
it connects at high speed. Everything after that is polled.

## Status

| | |
| --- | --- |
| **PebbleOS's developer access** | built for debug, release slot and debug slot images and the host's tests (the tether tools against the simulator answering as PebbleOS, `console.py` against a fake shell, the USB stack's unit tests); not yet run on a watch |
| **Tested on a host** | the protocol code as the stub runs it, against `tether.py`, over pipes, a pty and TCP through `bridge.py` (`make test`, with AddressSanitizer); the image's vectors (`pack_ota.py`); a PebbleOS build linked for the slot |
| **Not yet run on a watch** | all of the stub |

Things to watch on its first start:

- **USB**: the PHY bring-up runs for the first time outside ST's HAL. A port
  that does not appear with **USB HS...** on screen may enumerate at full
  speed with Down held at start; **USB err N** gives the failing step.
- **Display**: the same driver timings as PebbleOS's; a blank screen with a
  blinking frontlight means the stub runs. The order of the lines tells the
  rotation.
- **Clock**: `48MHz` on screen means PLL1 did not lock.
