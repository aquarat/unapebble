# No software entry to the bootloader's menu: reboot or power off

- Status: Accepted
- Date: 2026-11-27 13:48 UTC
- Decided by: agent

## Context
The maintainer asked for a "reboot into the bootloader menu", if the stock
bootloader can be asked. Bootloader 0.1.4 was traced (hardware.md, 9.6).

## Decision
It cannot: the bootloader reads only TAMP BKP3R, the power-off flag, and no
RAM word, option byte or GPIO but the buttons. The menu comes from the power
button held alone for more than 4 s, or from an image whose stack pointer is
invalid. So `reboot` restarts (into the stub in a slot build), `reboot off`
powers off through the bootloader, and `reboot menu` powers off and says
to hold the power button 4 s at power-on. Breaking the image's stack pointer
to force the menu was rejected: the image would not start again until
reinstalled.

## Consequences
Getting to the menu needs a hand on the watch; flashing without one goes
through the stub.
