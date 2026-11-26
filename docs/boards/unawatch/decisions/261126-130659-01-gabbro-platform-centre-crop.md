# Run on the gabbro platform, showing its centre 240x240

- Status: Accepted
- Date: 2026-11-26 13:06 UTC
- Decided by: agent

## Context
The UNA Watch has a 240x240 round display. PebbleOS has no 240x240 platform;
the nearest is gabbro (Pebble Round 2, 260x260 round). A new platform would
need its own SDK target, resources and app builds. Recorded after the fact.

## Decision
The board uses the gabbro platform. The display driver shows the centre
240x240 of the 260x260 framebuffer.

## Consequences
Gabbro apps and watchfaces run unchanged, but a 10-pixel outer ring is not
shown, so things drawn at the very edge are cut off. A native 240x240
platform remains possible later.
