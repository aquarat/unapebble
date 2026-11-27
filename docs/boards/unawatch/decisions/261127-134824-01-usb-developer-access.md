# USB developer access behind a setting

- Status: Accepted
- Date: 2026-11-27 13:48 UTC
- Decided by: owner (a setting, off by default; the tether functions in PebbleOS); agent (the rest)

## Context
Without a console, nothing could tell PebbleOS to reboot, and every look at
the hardware needed the tethered stub. The owner asked for the console over
USB and for the stub's functions inside PebbleOS, behind a settings toggle
that is off by default and kept.

## Decision
`CONFIG_DEV_USB` (`pebble/services/dev_usb`) gives two USB serial ports:
the console (logs and shell) on the first, the tether protocol on the
second. It is a toggle, **Settings > System > Debugging > USB Developer**,
off by default, behind the existing "PebbleOS developers only!" confirmation,
and the shell's `usb on|off`. The toggle is kept in a settings file of its
own (`devusb`), not in the shell preferences, which a paired phone can
write through the preferences blob database. With it off nothing enumerates:
the controller, its PHY and the 16 MHz crystal stay unpowered.

## Consequences
While it is on, **anyone who plugs a cable in can read and write the
watch's memory and registers, drive its I2C buses and GPIO, and restart or
power it off**, with no authentication, as with the stub; the refusals of
the protocol (261127-134824-06) protect the flash, the option bytes and the
USB controller, not the user's data. The bridge on the Mac adds the same
exposure to whoever reaches its TCP ports. The toggle is the only gate.
