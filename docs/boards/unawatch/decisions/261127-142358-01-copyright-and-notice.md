# Copyright lines, a NOTICE and a fork notice, for publishing the source

- Status: Accepted
- Date: 2026-11-27 14:23 UTC
- Decided by: owner (publish the source, these fixes); agent (how)

## Context
The owner asked whether the repository could be published. A review found
no secrets, private hosts, addresses or paths, no committed binaries, and
compatible licences: PebbleOS is Apache-2.0, UNA Watch Ltd's kernel source,
with its bootloader image and update key, MIT. But 172 files the port added
said `SPDX-FileCopyrightText: 2026 Core Devices LLC`, copied from upstream's
template, though Core Devices didn't write them; upstream files changed
substantially carried no notice of the change (Apache-2.0 section 4(b)); and
nothing credited UNA Watch Ltd for the register values, timings, sequences,
file format and key taken from its MIT-licensed source.

## Decision
- An added file that closely matches an upstream file (difflib ratio 0.30 or
  more against the upstream file of the same name or its STM32H7 sibling: 30
  files) keeps Core Devices' line and gains `2026 The unapebble authors`; the
  other 142 carry ours alone.
- Upstream files with 15 or more lines added (32) gain our line after theirs,
  as the notice of change.
- `NOTICE` says what the fork is, credits UNA Watch Ltd with its MIT licence
  text and lists what was taken, says the update key is public, credits the
  unscii-8 font, and disclaims affiliation with Core Devices and UNA Watch Ltd.
- The README starts with a fork notice: unofficial, experimental, the battery
  charging, where the board documentation is, install and recovery, the key.
- Board config files keep no licence header, as upstream's have none.
- Not done, by the owner's choice: the pid.codes test USB IDs (1209:0001 and
  0002) stay while no firmware is distributed; GitHub workflows come later.

## Consequences
The source can be published as it is. Distributing built firmware needs
USB IDs of its own first.
