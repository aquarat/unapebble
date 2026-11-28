# Back up the BlueNRG-2's DTM, and patch it if that is sound

- Status: Accepted
- Date: 2026-11-28 00:10 UTC
- Decided by: maintainer

## Context
The DTM 3.2 on the BlueNRG-2 passes no ACL data in link-layer-only mode,
whatever the order of its set-up (261127-204244-01, 261127-233959-01).
ST's DTM source of 3.1.0 forwards ACL data only in a build with
`LL_ONLY`; the updater the chip keeps can read its flash back and only
writes at or above the DTM's start (hardware.md, 2.5.1).

## Decision
The maintainer approved:

1. A backup, read only: the updater entered, the flash read from
   0x10042000 to its end, the stack's data page at 0x1007F000 included, at
   least twice, compared, and checked against the updater's CRC; kept
   outside the repository with its SHA-256, never committed nor published,
   as it is ST's code. The chip checked to start normally afterwards.
2. An analysis of the image against ST's DTM 3.1 source, for whether the
   ACL code is present or a patch must add it.
3. A binary patch, written only if the backup is verified, the patch is
   small and understood, and the updater's refusal to write below the DTM
   is confirmed as far as can be without writing: only the pages that
   change, never below 0x10042000, the 0x1007F000 page left alone; read
   back and CRC-checked before the chip is reset; the restore of the
   backup scripted and checked first.
4. In the repository, a tool that reads a user's own backup, checks that
   it is the known DTM 3.2 image, patches it and writes it through the
   tether, and restores it; no ST code distributed.

Otherwise: stop before writing and report, with what option (b) would
take.

## Consequences
Option (a) continues on a patched chip, if the patch is sound.
