# The stub's USB at high speed, with a full speed fallback

- Status: Accepted
- Date: 2026-11-26 23:23 UTC
- Decided by: agent

## Context
The U5A5's OTG HS controller has an internal high-speed PHY, which the stock
kernel and bootloader use with the 16 MHz HSE as its reference. Full speed on
the same PHY needs the same bring-up. The stub needs a USB vendor and product
ID.

## Decision
The stub connects at high speed (512-byte bulk packets); holding Down as it
starts forces full speed. Its IDs are `1209:0001`, pid.codes' test ID for
private use, with the chip's unique ID as serial number. `USB_DAM` is driven
low, as the stock kernel leaves it unless a debug accessory is attached.

## Consequences
Should high speed fail on the watch, full speed is one button away without a
new image. The test ID must not ship in a product; a PID of its own can be
requested from pid.codes later.
