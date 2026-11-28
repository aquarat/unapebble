# Standalone GPS workouts on the Sport app, recorded on the watch, exported as FIT

- Status: Accepted
- Date: 2026-11-28 09:29 UTC
- Decided by: agent

## Context
The maintainer asked for the watch to record workouts with its own GNSS to
its own storage, and for the user to get them off as FIT files, without
Bluetooth or a phone, which the watch cannot use yet. 261126-130734-02 and
261127-194525-03 kept the Sport app and the location service off on the
watch until a recorder decided otherwise. What the firmware has today:

- **Workout app and workout_service** (upstream PebbleOS). The service holds
  one workout in RAM: type (Walk, Run, Open), duration from the RTC, steps and
  distance from the health service's step counter (or a distance a client
  sets), calories and heart rate. Nothing is stored while it runs: a reboot
  loses the workout. At stop, a workout of a minute or more becomes an
  `ActivitySession`, a summary of 32 bytes (type, start, minutes, steps,
  distance, calories) in the health settings file, of which only today's are
  kept (`activity_sessions.c` prunes older ones once a minute) and which goes
  to the phone through data logging (`DlsSystemTagActivitySession`). No
  track, no position, no export but to the phone. Its "Sports" mode shows
  what a phone app (RunKeeper and the like, through the old Sports API over
  app messages) sends: the phone had the GPS and kept the track.
- **Sport app** (`pebble/apps/system/sport`, this project's, decision
  261126-130734-01): Run and Bike profiles over an engine (timer, laps,
  auto-pause, GNSS distance filtering, steps as a fallback, barometric
  elevation) fed once a second from the location service, the barometer and
  the workout service (steps, cadence, heart rate), with a recorder that
  writes one-second records to the filesystem (`/act<start>.<n>`, 24 bytes a
  second, 20 activities kept) and a summary at the end. It is on only in the
  emulators. Its recorder buffers 30 s in the app's heap and writes the
  summary only at the end, so a reboot loses up to 30 s and leaves an
  activity no reader knows how to close; the records carry no time of their
  own; the GNSS stays on until the app is left, the summary screen included.
- The watch has the AG3335M driver, the BMI270 (steps, through the health
  service), the MS5837 barometer, PFS on the eMMC that survives installs, and
  no heart rate sensor, Bluetooth or phone. Its RTC is set by nothing but the
  user or the stock firmware.

## Decision
- The Sport app is the recorder: on for the watch (`CONFIG_APP_SPORT`,
  `CONFIG_SERVICE_LOCATION`), with a Walk profile beside Run and Bike. The
  Workout app stays as it is, for step-based workouts without GPS; the Sport
  app still runs its activity through workout_service so that steps and
  cadence come from health and the activity appears in Health's sessions.
- The recording moves out of the app's heap into a recorder of its own with
  static state, a versioned, append-only journal in PFS that a reboot,
  crash or power loss cannot leave unreadable, and that a later start can
  close or resume (261128-092921-02).
- FIT activity files are encoded on the watch from the journal, by an
  encoder of this project's own (261128-092921-03), checked against public
  decoders in host tests.
- The GNSS runs only while the app acquires or records (261128-092921-04).
- The activity's time comes from the GNSS when the receiver knows it, since
  nothing sets the RTC (261128-092921-05).
- Getting the files off is decided in its own record, after a look at the
  USB stack and the bootloader's drive.

## Consequences
The part of 261126-130734-02 and 261127-194525-03 that keeps
`CONFIG_APP_SPORT` and `CONFIG_SERVICE_LOCATION` off is superseded for the
watch. The launcher gains Run, Walk and Bike. The image grows by the app,
the location service, the recorder and the encoder. Data logging to the
phone stays as upstream has it, for when a phone can connect: the FIT files
do not depend on it.
