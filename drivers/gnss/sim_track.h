/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/gnss.h>

/* Built-in route of the GNSS simulator: a hilly loop of about 3.1 km run at 5 to 6 min/km,
 * with a 15 s stop at a crossing. Pure code, so that host tests can replay it. */

/** Latitude of the start of the route, in 1e-7 degrees. */
#define GNSS_SIM_TRACK_ORIGIN_LAT_E7 473769000
/** Longitude of the start of the route, in 1e-7 degrees. */
#define GNSS_SIM_TRACK_ORIGIN_LON_E7 85417000

struct gnss_sim_track {
  /** Segment being run, from waypoint seg to seg + 1. */
  uint8_t seg;
  /** Seconds left standing at the current waypoint. */
  uint8_t stop_s;
  /** Distance run along the segment, in millimetres. */
  uint32_t progress_mm;
  /** Pseudo-random state for the noise. */
  uint32_t rng;
};

/** Length of one loop, in millimetres. */
uint32_t gnss_sim_track_length_mm(void);

/** Restart the route from its start. */
void gnss_sim_track_reset(struct gnss_sim_track *track);

/**
 * Advance the route by one second and fill the position fields of @p fix (position, altitude,
 * speed, course, accuracy). Time and quality are left to the caller.
 */
void gnss_sim_track_step(struct gnss_sim_track *track, struct pbl_gnss_fix *fix);
