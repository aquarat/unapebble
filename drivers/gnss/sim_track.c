/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "sim_track.h"

#include <stdbool.h>
#include <stddef.h>

#include <pbl/util/size.h>
#include <pbl/util/trig.h>

/* Millimetres per 1e-7 degree of latitude, times 10000. */
#define MM_PER_E7_X10000 111195LL

struct waypoint {
  /** Offset from the origin, in metres. */
  int16_t east_m;
  int16_t north_m;
  /** Altitude, in metres. */
  int16_t alt_m;
  /** Speed on the segment starting here, in centimetres per second. */
  uint16_t speed_cms;
  /** Seconds to stand still on arriving here. */
  uint8_t stop_s;
};

static const struct waypoint s_route[] = {
  {0, 0, 408, 320, 0},     {400, 0, 410, 315, 0},     {700, 250, 418, 300, 0},
  {750, 600, 432, 270, 0}, {600, 850, 445, 300, 0},   {300, 900, 440, 335, 0},
  {0, 800, 428, 340, 0},   {-250, 550, 418, 320, 15}, {-300, 250, 412, 320, 0},
  {-150, 50, 409, 320, 0},
};

static uint32_t prv_isqrt(uint64_t v) {
  uint64_t res = 0;
  uint64_t bit = 1ULL << 62;

  while (bit > v) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (v >= res + bit) {
      v -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }

  return (uint32_t)res;
}

static const struct waypoint *prv_wp(size_t i) {
  return &s_route[i % PBL_ARRAY_SIZE(s_route)];
}

static uint32_t prv_seg_len_mm(size_t seg) {
  const struct waypoint *a = prv_wp(seg);
  const struct waypoint *b = prv_wp(seg + 1);
  const int64_t dx = (int64_t)(b->east_m - a->east_m) * 1000;
  const int64_t dy = (int64_t)(b->north_m - a->north_m) * 1000;

  return prv_isqrt((uint64_t)(dx * dx + dy * dy));
}

/* Uniform in [-range, range]. */
static int32_t prv_noise(struct gnss_sim_track *track, int32_t range) {
  track->rng = track->rng * 1664525U + 1013904223U;

  return (int32_t)((track->rng >> 8) % (uint32_t)(2 * range + 1)) - range;
}

uint32_t gnss_sim_track_length_mm(void) {
  uint32_t len = 0;

  for (size_t i = 0; i < PBL_ARRAY_SIZE(s_route); i++) {
    len += prv_seg_len_mm(i);
  }

  return len;
}

void gnss_sim_track_reset(struct gnss_sim_track *track) {
  *track = (struct gnss_sim_track){.rng = 0x5eed1234U};
}

void gnss_sim_track_step(struct gnss_sim_track *track, struct pbl_gnss_fix *fix) {
  uint32_t speed_mm_s = 0;

  if (track->stop_s > 0) {
    track->stop_s--;
  } else {
    /* About 3% of pace variation, second to second. */
    const int32_t base = prv_wp(track->seg)->speed_cms * 10;
    uint32_t left = (uint32_t)(base + prv_noise(track, base / 32));

    speed_mm_s = left;
    while (left > 0) {
      const uint32_t seg_len = prv_seg_len_mm(track->seg);
      const uint32_t room = seg_len - track->progress_mm;

      if (left < room) {
        track->progress_mm += left;
        break;
      }

      left -= room;
      track->progress_mm = 0;
      track->seg = (uint8_t)((track->seg + 1) % PBL_ARRAY_SIZE(s_route));
      track->stop_s = prv_wp(track->seg)->stop_s;
      if (track->stop_s > 0) {
        speed_mm_s -= left;
        break;
      }
    }
  }

  const struct waypoint *a = prv_wp(track->seg);
  const struct waypoint *b = prv_wp(track->seg + 1);
  const int64_t seg_len = prv_seg_len_mm(track->seg);
  const int64_t p = track->progress_mm;

  /* True position, then about 1.5 m of receiver noise. */
  int64_t east_mm = a->east_m * 1000LL + (b->east_m - a->east_m) * 1000LL * p / seg_len;
  int64_t north_mm = a->north_m * 1000LL + (b->north_m - a->north_m) * 1000LL * p / seg_len;
  const int64_t alt_mm = a->alt_m * 1000LL + (b->alt_m - a->alt_m) * 1000LL * p / seg_len;

  east_mm += prv_noise(track, 1500);
  north_mm += prv_noise(track, 1500);

  const int32_t lat_e7 =
      GNSS_SIM_TRACK_ORIGIN_LAT_E7 + (int32_t)(north_mm * 10000 / MM_PER_E7_X10000);
  const int32_t lat_angle = (int32_t)((int64_t)lat_e7 * TRIG_MAX_ANGLE / 3600000000LL);
  const int64_t cos_lat = cos_lookup(lat_angle);

  fix->lat_e7 = lat_e7;
  fix->lon_e7 = GNSS_SIM_TRACK_ORIGIN_LON_E7 +
                (int32_t)(east_mm * 10000 * TRIG_MAX_RATIO / (MM_PER_E7_X10000 * cos_lat));
  fix->alt_cm = (int32_t)(alt_mm / 10) + prv_noise(track, 150);
  fix->speed_mm_s = speed_mm_s;

  /* atan2_lookup() counts counterclockwise from east, a course clockwise from north. */
  const int32_t angle = atan2_lookup(b->north_m - a->north_m, b->east_m - a->east_m);
  const int32_t course = (TRIG_MAX_ANGLE / 4 - angle + TRIG_MAX_ANGLE) % TRIG_MAX_ANGLE;

  fix->course_cdeg = (uint16_t)((int64_t)course * 36000 / TRIG_MAX_ANGLE);
  fix->hdop_x10 = (uint16_t)(11 + prv_noise(track, 2));
  fix->h_acc_dm = (uint16_t)(30 + prv_noise(track, 8));
}
