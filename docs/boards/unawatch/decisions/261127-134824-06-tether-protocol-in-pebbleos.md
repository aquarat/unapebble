# The stub's protocol in PebbleOS, without calls or flashing

- Status: Accepted
- Date: 2026-11-27 13:48 UTC
- Decided by: agent

## Context
The owner asked for the stub's functions in PebbleOS, the same protocol so
that `tether.py` works unchanged, and to decide on running code and on
flashing.

## Decision
PebbleOS builds the stub's `proto.c` from `tools/una/tether/stub` under a
port of its own (`pebble/services/dev_usb/tether.c`), on a thread of its
own. It answers INFO, READ, WRITE, FILL, CRC32, I2C (through the bus
drivers, at their speed; the same PMIC, gauge and USB-PD write guard), GPIO
(the registers, as the stub; PA11 to PA14 refused), RESET (0 restart, 1
power off, both orderly), NOTE (logged) and CLEAR_FAULT. Its refusals are
the stub's, but for the stub's RAM, which is PebbleOS's here; instead it
refuses writes to the system control block and the MPU
(0xe000ed00-0xe000edff and the alias). Memory goes through one-instruction
probes; a bus or MPU fault on one is stepped over by a new kernel hook,
`pbl_fault_fixup()`, and reported as the stub reports it.

CALL is refused (unsupported): the experiments are linked into SRAM that
PebbleOS uses, run with the stub's API, its vector table and its SysTick,
and a fault or hang in one would take the OS with it. FLASH_ERASE,
FLASH_WRITE and BOOT are refused too: the running image cannot erase
itself, and the stub already flashes safely. `tether.py flash` against
PebbleOS resets it first and flashes through the stub once it answers,
which works for slot builds, whose every reset returns to the stub.

## Consequences
A write to a buffered address that faults late is caught as an imprecise
fault only if it lands inside the probe; a write to a register that hangs
the bus still hangs the watch. A slot build restarts into the stub, any
other into itself: `tether.py flash` only goes through for slot builds.
