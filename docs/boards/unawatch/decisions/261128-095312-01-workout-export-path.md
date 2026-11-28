# Workouts off the watch: a host tool over the console first, a USB drive of FIT files next

- Status: Accepted
- Date: 2026-11-28 09:53 UTC
- Decided by: agent

## Context
The maintainer asked for the user to get FIT files off the watch without a
phone or Bluetooth, aimed at a normal user, and for the choice between a host
tool over the existing USB developer access, a USB mass storage drive from
PebbleOS, and FIT files written to UNA's own drives, which the bootloader's
USB drive shows. What there is:

- **The developer access** (261127-134824-01): the console and the tether
  protocol on two CDC ports, in debug and slot builds only, behind a setting.
  The console runs shell commands; the tether protocol reads memory, not
  files. Whoever plugs a cable in while it is on can read and write memory.
- **The USB stack** (`subsys/usb`): device functions declared with
  `PBL_USBD_FUNCTION_DEFINE()`, CDC ACM, DFU and WebUSB/Microsoft OS
  descriptors; no mass storage class. Release builds carry no USB at all.
  With the USB powered the CPU stays out of Stop 2 (17 to 19 mA).
- **UNA's drives** (hardware.md, 3.3 and 3.4): the bootloader exposes only
  UPD, an exFAT volume of 100 MB, over USB, and only from its menu (power
  off, hold the power button 4 s at power-on, connect). It keeps files it
  does not know: it deletes the `.ota` it installs and, at a factory reset,
  every `.ota`, `.lib` and `1:/settings`; a `.pbpack` and the splash `.bmp`
  stay. USR, the stock firmware's user drive, is exposed only by the stock
  firmware. PebbleOS has a read-only FAT and exFAT reader (`lib/fat_ro`),
  no writer, and writes nothing outside the GUI region.

## Decision
- **Now: a host tool over the console.** `workout list|info|fit|close|delete`
  in the shell, and `tools/una/workouts.py`, which lists the workouts and
  downloads each as a FIT file, in 16 KiB ranges checked by CRC-32 and
  retried, then the FIT file's own CRCs, named by its UTC start and sport.
  It works with any build that has the developer access, over a cable or a
  `bridge.py`.
- **Next, for normal users: a read-only USB drive of FIT files from
  PebbleOS.** A mass storage function (bulk-only transport, the SCSI
  commands hosts send, write-protected) over a FAT volume synthesised from
  the activities: the boot sector, FAT and directory computed, file data
  encoded from the journals as the host reads it. Plugged in, the watch
  shows a drive from which files drag to Strava, Garmin Connect or
  intervals.icu. It needs the USB in release builds, powered only while
  plugged in, without the developer access's ports; the cost and how the two
  share the USB are for its own record.
- **Not: FIT files on UNA's update drive.** It would show the files in the
  bootloader's drive, but needs an exFAT writer in PebbleOS writing outside
  the GUI region, where a bug or a power loss could damage the drive the
  bootloader installs updates from, and the drive appears only from the
  bootloader's menu, a power cycle with a button held. Nothing was written
  there.

## Consequences
For now, downloading needs the USB developer access, that is a debug or slot
build, or the setting on, and a computer with Python: a developer's route,
with the setting's exposure while it is on. A normal user's route waits for
the drive. The shell's export is the same encoder the drive would use, so
what the tool downloads is what the drive will show.
