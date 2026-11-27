# The AG3335M behind PebbleOS's GNSS class, started as the stock firmware starts it

- Status: Accepted
- Date: 2026-11-27 19:45 UTC
- Decided by: agent

## Context
The maintainer asked for a GNSS driver a sports recorder can use: power,
start-up, NMEA parsing into fix, time, position, speed, HDOP and satellite
counts. The port already has an interface for it (261126-130734-01): the GNSS
class (`pbl/drivers/gnss.h`, one `struct pbl_gnss_fix` per epoch to a
listener), the location service on it, and the Run and Bike apps on that,
so far fed only by a simulator.

UNA's kernel drives the receiver with Airoha's PAIR commands
(`AirohaBase.cpp`, `AirohaAG3335M.cpp`, `GpsAiroha.cpp`): GNSS_EN high, then
nothing sent until the receiver says `$PAIR001,002,0`; then system sleep
locked (PAIR382,1), the GNSS subsystem off (PAIR003), dual band off
(PAIR104,0, L1 only), on again (PAIR002), a hot start (PAIR004). On L1 it
keeps the system sleep locked. It sets neither the constellations nor the
rate, and answers the receiver's aiding requests (PAIR010) with EPO data, the
time and a reference location. Each command is acknowledged with
`$PAIR001,<command>,<result>`. The NMEA sentences come in a burst per epoch,
in no order the standard fixes.

## Decision
- `drivers/gnss/ag3335.c` implements the class for `airoha,ag3335m`, a child
  of its UART with its enable line. Starting powers the receiver and runs
  UNA's sequence, plus a version query (PAIR020), as a state machine on the
  high priority system work queue: no caller waits for the receiver, and
  the shell's commands, which run on the system work queue, can wait for its
  answers. A command goes out up to four times, 0.5 s apart (2 s for the
  power and start commands): the receiver loses one now and then, its UART
  asleep (hardware.md, section 2.6.1). A receiver that does
  not answer within 3 s is powered again once, then given up on. Stopping
  cuts its power; it is off at least 0.5 s before the next start.
- `drivers/gnss/nmea.c` checks and splits the sentences and gathers GGA,
  RMC, GSA and GSV into an epoch, reported when the line has been quiet for
  100 ms after a burst, or when a GGA or RMC of another time arrives. All in
  integers. Quality from GGA (2D or 3D from GSA), none for dead reckoning or
  an RMC saying the data is not valid; dates before 2020, the receiver's
  1980-01-06 before it knows the time, are no time.
- The class gains the satellites in view in each fix (`sats_in_view`, the
  most any signal of a constellation lists, summed), and an optional
  statistics operation: the driver's state, the receiver's firmware,
  sentences by type, bad lines, overruns, epochs, satellites with a signal,
  the strongest C/N0, the time to first fix.
- The shell's `gnss` takes `on`, `off`, `status`, and `pair PAIRnnn,...`,
  which sends a command to the running receiver and prints its result and
  answer, for measurements: only a list of commands that set the running
  receiver or query it, none that writes its flash.
- L1 only and the system sleep locked, as UNA runs it; L5
  (`CONFIG_GNSS_AG3335_DUAL_BAND`) and the sleep unlocked
  (`CONFIG_GNSS_AG3335_SLEEP`) are options. Intervals under 1 s set the fix
  rate (PAIR050); longer ones report every Nth 1 Hz epoch. No aiding yet: the
  receiver's requests for time and location are not answered.

## Consequences
The location service and the sport apps run on the watch's receiver as they
do on the simulator, once switched on (261127-194525-03). A cold start takes
whatever it takes without aiding. The class change is source compatible;
`struct pbl_gnss_fix` grows from 32 to 40 bytes.
