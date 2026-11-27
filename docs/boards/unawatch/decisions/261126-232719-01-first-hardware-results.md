# First results on a watch: the first port boots, the eMMC build loops

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: maintainer (results); agent (record)

## Context
The maintainer installed the first images on a watch, without a console or a
probe: only the display, the frontlight, the vibration motor and the update
drive show anything.

## Decision
Recorded as found:

- Image A, `b697864b4` (the first port: storage in the embedded flash,
  resources in the image, no eMMC, no battery drivers), boots and works
  well.
- Its 2.6 MB `.ota` was accepted, which confirms the 4 MiB STM32U5A5 on this
  watch (see 261126-172141-06-code-partition-and-image-limit).
- Image B, `4ff00ebca` (storage and resources on the eMMC, the clock change,
  the bootbits change), boot-loops. Current main, B with the battery drivers,
  presumably does too.

## Consequences
The cause is in what B added; the experiments firmware
(261126-232719-03-experiments-firmware) is built from A's configuration to
find it. Images over 1664 KiB are possible on this watch.
