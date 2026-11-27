# Clock the display's frames out on a background thread

- Status: Accepted
- Date: 2026-11-27 13:01 UTC
- Decided by: agent

## Context
With its eMMC write timeout fixed (261127-123202-01), the eMMC firmware
reached the Launcher, then about 35 s later the task watchdog found SysWorkq
not fed for 10 s, the app was killed for a full event queue, SysWorkq stalled
again and the watchdog reset the watch with a core dump. In the core dump
SysWorkq was ready, preempted in `pbl_mutex_unlock()`, not waiting for
anything; KernelMain (priority 3) was in `jdi_gpio_line()`, sending a frame of
the battery's charging animation, which loops while the charge is under 100%.
`jdi-parallel-gpio` clocked each frame out on the CPU in the caller,
KernelMain, and completed before returning: at about 15 frames a second (a
frame takes 63 ms measured on the watch, not the 40 ms estimated) KernelMain
never blocked, and the app (2), SysWorkq (1) and the workers got no CPU at
all. The eMMC was not the cause: its accesses were slow but bounded (see
261127-130125-03).

Measured on the watch through the tether, with a temporary build that keeps
the charging animation running and logs CPU shares every 15 s:

- frames clocked on KernelMain: SysWorkq not fed for 6.8 s, 4 s after the
  Launcher started; task watchdog reset at 11 s of uptime;
- frames on a thread at priority 1: 300 s without a stall, until the timed
  restart; the display thread 92% of the CPU, KernelMain 6%, idle 1%, every
  log flush done, 14.5 frames a second.

## Decision
The driver clocks frames out on a thread of its own, `Display`, at
`CONFIG_DISPLAY_JDI_GPIO_THREAD_PRIO` (1, the background work queue's, which
shares the CPU with it a tick at a time), as a controller's DMA would run
beside the CPU: `pbl_display_update()` hands it the framebuffer and returns,
the thread sends the frame and calls `pbl_display_complete()`. Everything of
higher priority preempts it; the panel's timings are all minimums, so a frame
only takes longer. The display class allows a callback from a thread of the
display, and the compositor, which took any callback outside an interrupt to
be its own synchronous one, finishes a flush reported from another thread on
KernelMain through an event, as it does one from an interrupt.

Not taken: raising the watchdog timeout, or frame pacing in the compositor,
which hide the starvation without ending it; a timer and DMA driving the
pins, which frees the CPU but needs the signals of several ports sequenced
and cannot be checked without someone watching the panel.

## Consequences
An animation still takes all the CPU that is left, now from below: the app,
KernelMain and SysWorkqHigh come first, SysWorkq shares with the display.
A busy app slows the display rather than the reverse. The 63 ms frame, a
third more than the panel's 480 half periods of 69 us, is the line conversion
running inside the LSB half period; it could be overlapped with the waits.
`tests/pebble/services/compositor/test_compositor_display.c` checks where the
compositor finishes a flush for each source of the callback.
