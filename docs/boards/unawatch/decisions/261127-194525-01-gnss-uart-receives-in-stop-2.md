# The GNSS port receives in Stop 2: LPUART1 on MSIK, GPDMA1, woken by its first character

- Status: Accepted
- Date: 2026-11-27 19:45 UTC
- Decided by: agent

## Context
The maintainer asked for GPS on the watch, for a sports recorder later, with
the receive path on DMA and idle detection so that the CPU can sleep. The
AG3335M is on LPUART1 (PA2/PA3, AF8) at 921600 baud with no flow control; the
stock firmware clocks the port from MSIK at 4 MHz and receives on GPDMA1
channel 11, a circular linked list with idle detection, waking from Stop 2 on
the first byte (`LpUart.cpp`, hardware.md section 3.2). The receiver sends a
burst of NMEA once a second, about 450 bytes indoors, 5 ms of the line.

PebbleOS idles in Stop 2 (261127-153346-01), and its USARTs block Stop while
receiving: their kernel clocks stop in it. Blocking Stop while the receiver
runs would cost what Stop saves, about 5 mA (Sleep on PLL1 at 160 MHz against
1 mA in Stop 2), on top of the receiver. LPUART1 is in the SmartRun domain:
with UESM set, its autonomous mode on (SRDAMR) and a kernel clock that runs,
it receives in Stop 2 into its 8-character FIFO and its receive interrupt
wakes the CPU. GPDMA1 does not run in Stop 2; LPDMA1 would, but only into
SRAM4, and PebbleOS has no driver for either. Started on demand, MSIK takes
microseconds, longer than half a bit at 921600 baud (0.54 us); MSIKERON keeps
it running in Stop. An LPUART also has no receiver timeout, and its idle
interrupt does not wake the CPU from Stop.

## Decision
- A GPDMA1 driver in the DMA class (`drivers/dma/stm32u5.c`): single and
  cyclic transfers, a cyclic one a block that reloads itself from a
  linked-list item, its half and full transfer interrupts ending the two
  periods, as the STM32H7 driver's ring.
- The STM32 UART driver takes the STM32U5 LPUART (`st,stm32u5-lpuart`): its
  baud rate generator, the FIFO on, and, with a receive DMA channel,
  reception in Stop. While reception is enabled the receive interrupt is armed
  as a wake-up only; the first character after an idle line wakes the CPU,
  the interrupt is disarmed and Stop blocked; the idle interrupt drains the
  1 KiB ring and lets Stop back in, re-arming the wake-up. The FIFO holds the
  characters that come while the CPU leaves Stop and GPDMA1 starts again
  (8 characters, 87 us).
- The clock driver runs MSIK in Stop whenever it is on (MSIKERON with MSIKON),
  and sets LPUART1's autonomous mode with its clock. MSIK is LPUART1's only
  user.
- An LPUART is clocked, and has its pins, only while in use, receiving or
  sending: otherwise its clock is gated, which turns MSIK off, and its pins
  are in their sleep state, analog, so that the powered-down receiver is not
  fed through them.

## Consequences
The CPU sleeps in Stop 2 between bursts while the receiver runs, out of it
for each burst, and an idle gap in a burst costs one more wake-up. Measured
on the watch: 97% of the time in Stop 2 with the receiver on, about 450 KB received
over three sessions without an overrun or a lost sentence past the
receiver's own start-up garbage, and the watch drawing about 10 mA with it
searching, 0.8 mA with it off (index, Power). Off, the port draws nothing more
than before. Whether MSIK started on demand would do (as the stock firmware
has it, without MSIKERON) was not tried: what MSIK costs running in Stop is
below what the gauge resolves. The USARTs are unchanged. A peripheral that needs MSIK in Stop
without wanting it to run there would need the clock driver to tell the two
apart. Other GPDMA1 users (the BlueNRG-2's SPI) can use the same driver; they
must block Stop while they transfer, as GPDMA1 stops in it.
