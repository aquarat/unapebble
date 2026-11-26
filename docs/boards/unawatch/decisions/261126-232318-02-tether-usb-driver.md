# A polled DWC2 driver of its own for the stub's USB

- Status: Accepted
- Date: 2026-11-26 23:23 UTC
- Decided by: agent

## Context
The stub needs a USB CDC-ACM device on the OTG HS controller. TinyUSB (MIT)
would fit the repository's Apache-2.0 licence but is a dependency to fetch or
vendor, built around interrupts and its own task loop. ST's USB device library
comes under ST's own licence terms. A CDC-ACM device needs only a control
endpoint, one bulk pair and an idle interrupt endpoint.

## Decision
`stub/usb.c` drives the Synopsys DWC2 core directly, in slave (FIFO) mode,
polled, about 700 lines. Its PHY and core bring-up follow ST's HAL for the
STM32U5A5 (`USB_CoreInit`, `USB_DevInit`) and UNA's `Usb.cpp` step by step;
ST's code was read, not copied.

## Consequences
No dependency, no interrupts, nothing a crashing experiment's IRQ state can
break. The driver has not run on the hardware: its first start may need
fixes, which the on-screen USB state and error step should narrow down.
