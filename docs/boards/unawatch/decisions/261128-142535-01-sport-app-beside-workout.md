# A Sport app beside the Workout app, on the workout service

- Status: Accepted
- Date: 2026-11-28 14:25 UTC
- Decided by: agent

## Context
Recorded after the fact: decided 2026-11-26 while writing the Run app
(261126-130734-01). Upstream's Workout app (`pebble/apps/system/workout`)
draws what the workout service or a phone's Sports API gives it, through a
controller of fixed metrics, in an `active.c` of about 2000 lines. GPS
screens, laps, auto-pause and recording do not fit that model without
rewriting most of it. The workout service underneath is useful as it is:
it subscribes the heart rate monitor, gives heart rate, zones and steps, and
saves the activity as a Health session.

## Decision
The Run and Bike apps are a new system app family (`pebble/apps/system/sport`),
not new Workout modes. They start, pause and stop a workout in the workout
service for heart rate, steps and the Health session, and keep their own
timer, distance and laps in the sport engine. The workout service gains
`workout_service_set_distance()`, so the Health session gets the GNSS
distance instead of the step estimate once a client sets one. Auto-pause
does not pause the workout service, since steps have to keep coming in to
detect the resume.

## Consequences
Workout stays as upstream has it, for step-based workouts. Health's
duration for a sport activity includes auto-paused time. Stopping still
shows the workout service's "way to go" notification over the sport
summary. Both apps can't run a workout at once: a sport started while the
Workout app has one runs without heart rate and steps.
