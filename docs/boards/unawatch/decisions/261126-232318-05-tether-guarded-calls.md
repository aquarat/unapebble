# Faults and timeouts return to the stub

- Status: Accepted
- Date: 2026-11-26 23:23 UTC
- Decided by: agent

## Context
Experiments will fault, and so will peeks at unmapped addresses. A fault that
kills the stub loses the tether until the watch is reset.

## Decision
Peeks, pokes, fills, CRCs and experiment calls run under a guard, on their
own stack (PSP). A fault (HardFault, MemManage, BusFault, UsageFault, an ECC
NMI) or a SysTick timeout taken from guarded thread mode is recorded
(registers, CFSR, HFSR, MMFAR, BFAR) and unwound to the guard's caller
through an exception frame built on the main stack; the stub then disables
every IRQ an experiment enabled. A fault it cannot unwind (in the stub
itself, or in an experiment's IRQ handler) is kept at `0x2026ff00`, the top
of the stock kernel's retained RAM, and the watch resets; the stub shows and
reports it after. An unexpected IRQ is disabled and noted. The independent
watchdog is fed throughout, in case something started it.

## Consequences
A faulting experiment costs a reply with its fault, not a reset. An
experiment that hangs with interrupts masked cannot be stopped but by the
watchdog, if running, or the PMIC's long press.
