# The workouts' USB drive: a virtual FAT of FIT files, on whenever plugged in

- Status: Proposed
- Date: 2026-11-28 14:26 UTC
- Decided by: agent

## Context
261128-095312-01 chose a read-only USB drive of FIT files served by
PebbleOS as the normal user's way to get workouts off the watch. What there
is: the USB stack (`subsys/usb`) with functions fixed at build time, CDC ACM
and the DWC2 driver on the U5's HS PHY (261127-134824-04), USB only in debug
and slot builds, powered while connected and keeping the CPU out of Stop 2
(261127-153346-01); the FIT encoder, which encodes from the start; PFS on
the eMMC. The core's FIFO is 952 words (GHWCFG3 read on the watch), not the
1024 of the reference manual.

## Decision
- **Medium**: a FAT16 volume of 128 MiB in 4 KiB clusters, no partition
  table, computed as it is read (`lib/vfat`): boot sector, two FATs and the
  root directory from the list of files, each file's clusters contiguous,
  long names with 8.3 aliases. Nothing is stored.
- **Files**: one per finished workout this firmware reads, oldest first,
  named `YYYY-MM-DD-HHMM-sport.fit` in the watch's local time of the start
  (the GNSS's UTC when it gave one), `-2` and on for a second one in the same
  minute; dated likewise. Unfinished workouts, the one recording among them,
  are left out. The bytes are those of `sport_store_export_fit()`, what
  `workouts.py` downloads.
- **Encoding as read**: `sport/fit_stream.c` sizes each file once (one pass,
  marks every 32 KiB: the encoder's state and the journal's position), then
  reads carry on from where the last ended or from the nearest mark, the
  trailing CRC from a pass from the start, kept. A cache of 16 blocks of
  4 KiB serves re-reads. Sizes and marks are kept across plug-ins while a
  workout's summary is unchanged. While sizing, the drive says it is
  becoming ready.
- **When**: whenever the watch is plugged in and **Settings > System > USB
  Drive** is on (the default; release builds included). No host configuring
  the device within 10 s (a charger), or the host ejecting the drive, and the
  drive gives the USB up until the next plug-in, so the CPU returns to
  Stop 2. A workout saved or deleted meanwhile takes the medium away for 5 s
  once the host has not read for 3 s, then brings it back changed.
- **Composite**: the drive is a function of the same device as the developer
  access (`1209:0002`), after its two serial ports (interface 4, endpoints
  0x85 and 0x03), or alone in builds without it. `services/usb_power` keeps
  the USB up while either wants it. FIFOs: 128 words on IN endpoints 1 to 5.
- **Bulk-only**: no endpoint is ever halted; missing data is sent as zeros,
  unwanted data taken, the residue tells.

## Consequences
Release builds carry USB (code 1672 KiB, RAM +106 KiB of static buffers).
Whoever plugs the watch in can copy its tracks, as with any watch that shows a
drive; the setting turns it off. Proposed: on the watch, the composite device
did not get configured by macOS in the first tries (the developer access
without the drive did), and the cause is not found yet.
