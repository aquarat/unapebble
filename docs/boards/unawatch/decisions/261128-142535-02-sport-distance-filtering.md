# Sport distance from GNSS speed first, then positions, then steps

- Status: Accepted
- Date: 2026-11-28 14:25 UTC
- Decided by: agent

## Context
Recorded after the fact: decided 2026-11-26 in the sport engine
(`pebble/apps/system/sport/engine.c`). Adding up position differences each
second overstates the distance: in host tests with 1.5 m of position noise,
a 1.8 km run came out about 4% long, and a watch standing still gained about
16 m in two minutes. Receivers report Doppler ground speed, which is far less
noisy than position.

## Decision
- A fix counts only with HDOP 5.0 or better and four or more satellites
  (per sport, in its profile).
- With a usable fix, the second's distance is the GNSS ground speed, zero
  under 0.25 m/s; positions only reject outliers (a jump faster than the
  sport's top speed since the last fix counted is ignored, and given up on
  after 10 in a row).
- Without a ground speed, a move counts once it is further than the
  sport's jitter distance (3 m running, 4 m cycling) from the last position
  counted.
- Without a usable fix, steps give the distance, spread over 16 s since the
  step counter reports in bursts. The next fix starts afresh, so a gap is
  never counted twice.
- A speed sensor of its own (a BLE wheel sensor, later) beats all of these.
- Elevation: the barometer, calibrated once to the first GNSS altitude with
  HDOP 1.5 or better, else the GNSS altitude. Both are smoothed, and a climb
  counts once it passes 1 m (barometer) or 4 m (GNSS), and only while
  running.
- Auto-pause below a speed for some seconds, resume above a higher one for
  some seconds, both per sport (running: under 0.5 m/s for 4 s, over 1 m/s
  for 2 s).

## Consequences
Host tests (`tests/pebble/apps/system_apps/sport`) hold each of these. With
a ground speed, distance is exact on the simulator and within the tests'
tolerance with noise. Without one, up to the jitter distance is pending at any
time. GNSS ascent loses up to 4 m per climb.
