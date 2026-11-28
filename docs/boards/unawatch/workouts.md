# UNA Watch: recording and exporting workouts

The watch records runs, walks and rides with its own GPS, with no phone, keeps
them, and gives them to a computer as FIT files, which Strava, Garmin Connect,
intervals.icu and most training tools import. This page is the how-to; the
design is on the board page ({doc}`index`, Workouts) and in the decision log
from 261128-092921-01.

What it records each second: the position, distance, speed and pace, the
elevation (from the barometer, set from the GPS's altitude), the time, and the
cadence while the watch counts steps. It has no heart rate.

## The buttons

| Button | |
| --- | --- |
| Top right | **Select**: start, pause, choose |
| Bottom right | **Back**: a lap during a workout; leaves a screen |
| Top left | **Up**: the previous screen of data |
| Bottom left | **Down**: the next screen of data |

## Recording

1. In the launcher, open **Run**, **Walk** or **Bike**. The watch looks for
   satellites: **Locating GPS...** and how many it uses. Go outdoors, under
   open sky, and keep still: the first fix can take a few minutes, with no
   phone to help it.

   ```{thumbnail} images/workout-ready-searching.webp
   :title: Locating GPS.
   ```

2. **GPS ready**, with a buzz: press **Select** to start after a 3 second
   countdown, or hold it to start at once.

   ```{thumbnail} images/workout-ready.webp
   :title: Ready to start.
   ```

   Indoors, or to start before the fix, press Select twice: the time,
   steps and elevation record, the track once the fix comes. A ready screen
   left 10 minutes turns the GPS off to save the battery; Select searches
   again.

3. During the workout, **Up** and **Down** show the other screens: timer,
   distance and pace; average and lap pace and lap time; cadence, elevation
   and ascent; the clock. The dot at the bottom is green with a fix, red
   without. **Back** closes a lap; laps also close every kilometre or mile
   (every 5 on the bike), with a buzz and the lap's time and pace.

   ```{thumbnail} images/workout-active.webp
   :title: Timer, distance and pace.
   ```

   ```{thumbnail} images/workout-active-pace.webp
   :title: Average pace, lap pace and lap time.
   ```

   ```{thumbnail} images/workout-active-elevation.webp
   :title: Elevation, ascent and the clock.
   ```

   ```{thumbnail} images/workout-lap.webp
   :title: A lap.
   ```

   The workout pauses itself when you stop (**AUTO PAUSE**) and carries on
   when you move.

4. **Select** pauses and shows **Resume**, **Save** (with the time and
   distance so far) and **Discard**.

   ```{thumbnail} images/workout-pause-menu.webp
   :title: Paused.
   ```

5. **Save** ends it. A workout of a minute or more also goes to Health, which
   says so; Back dismisses it. Then the summary: time, distance, average pace
   or speed, top speed, ascent, laps, each lap. The GPS is off from then on.
   **Back** leaves.

   ```{thumbnail} images/workout-health.webp
   :title: Health has it too.
   ```

   ```{thumbnail} images/workout-summary.webp
   :title: The summary.
   ```

If the watch restarts in the middle of a workout, from a fault or a flat
battery, what it recorded is kept, all but at most the last 10 seconds. Open
the same sport within 30 minutes: it offers to **Resume** (Select), paused,
with its time, distance and laps, or (Down) to save it as it was and start a
new one.

```{thumbnail} images/workout-resume.webp
:title: A workout cut short by a restart.
```

The watch keeps the 30 latest workouts, about 115 KiB an hour each, and
keeps them through firmware updates.

With the GPS on, the watch draws about 12 to 13 mA (measured indoors, the GPS
searching): some 15 hours of recording on a full charge.

## Getting the workouts off the watch

For now this goes over the USB developer access, so it needs a debug build,
or **Settings > System > Debugging > USB Developer** on: while it is on,
anyone with a cable can read the watch's memory, so turn it off afterwards.
A USB drive showing the FIT files, without the developer access, is planned.

On a computer with Python 3 and the firmware's `tools/una` directory (in its
sources, or the tether host tools of a release), plug the watch in and run:

```shell
python3 tools/una/workouts.py list
python3 tools/una/workouts.py get -o ~/workouts
```

`get` downloads the workouts not downloaded yet, checks them end to end and
names them by their start (UTC), sport and id. `get ID` downloads one,
`get --all --force` all again, `delete ID` deletes one from the watch. If the
watch is plugged into another machine running `bridge.py`, add
`--tcp HOST:4501`.

For example, from the watch used to test this, indoors (the first two
before the watch had set its clock):

```text
$ python3 tools/una/workouts.py list
4b3f6957  2010-01-02 15:42Z?  Run     0.62 km  0:03:14    2 laps  done
4b3f6b38  2010-01-02 15:50Z?  Run     1.46 km  0:08:02    1 laps  done
6b0ab3fb  2026-11-28 10:59Z?  Run     0.00 km  0:00:03    1 laps  done
6b0ab9cf  2026-11-28 11:23Z  Run     0.24 km  0:01:14    2 laps  done
$ python3 tools/una/workouts.py get -o ~/workouts
~/workouts/2026-11-28-112359-run-6b0ab9cf.fit  2385 bytes  6b0ab9cf  2026-11-28 11:23Z  Run     0.24 km  0:01:14    2 laps  done
...
```

A public FIT decoder reads them back, here fitdecode and fitparse through
`tests/pebble/apps/system_apps/sport/check_fit.py`:

```text
2026-11-28-112359-run-6b0ab9cf.fit: ok, 74 records, 2 laps, running 237 m in 74 s, from 2026-11-28T11:23:59+00:00
```

A `?` after the time means the GPS never gave the time during that workout,
so the watch's clock was used; the watch also sets its clock from the GPS
whenever it has the time and no workout is running.

Then upload the files: on Strava, **+ > Upload activity > File**; on Garmin
Connect, **Import Data**; on intervals.icu, **Import**.

```{thumbnail} images/workout-track.webp
:title: The track of an exported workout, drawn from its FIT file (the replayed test route).
```

## Not yet

- No heart rate: the watch's sensor has no driver.
- Cadence needs the watch's clock to be set, which it does from the GPS
  before a workout once the GPS has the time.
- No list of past workouts on the watch itself; the computer lists them.
- No phone sync: Bluetooth works now, but the workouts do not go over it
  yet.
