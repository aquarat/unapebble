/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "engine.h"

#include <stddef.h>
#include <string.h>

#include <pbl/util/trig.h>

// Millimetres per 1e-7 degree of latitude, times 10000.
#define MM_PER_E7_X10000 111195LL
// Rejected fixes in a row after which the last position counted is given up.
#define MAX_REJECTED_S 10
// Doppler speeds below this are a receiver standing still.
#define DOPPLER_MIN_MM_S 250U
// Distance the grade needs on its window.
#define GRADE_MIN_DISTANCE_MM 20000U

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

uint32_t sport_geo_distance_mm(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7) {
  // Equirectangular projection at the mean latitude.
  const int64_t mean_lat_e7 = ((int64_t)lat1_e7 + lat2_e7) / 2;
  const int32_t angle = (int32_t)(mean_lat_e7 * TRIG_MAX_ANGLE / 3600000000LL);
  const int64_t cos_lat = cos_lookup(angle);
  const int64_t dy = ((int64_t)lat2_e7 - lat1_e7) * MM_PER_E7_X10000 / 10000;
  const int64_t dx =
      ((int64_t)lon2_e7 - lon1_e7) * MM_PER_E7_X10000 / 10000 * cos_lat / TRIG_MAX_RATIO;

  return prv_isqrt((uint64_t)(dx * dx + dy * dy));
}

uint32_t sport_pace_s(uint32_t speed_mm_s, uint32_t unit_mm) {
  if (speed_mm_s == 0) {
    return 0;
  }

  return (uint32_t)(((uint64_t)unit_mm + speed_mm_s / 2) / speed_mm_s);
}

uint32_t sport_stats_avg_speed_mm_s(const SportStats *stats) {
  return stats->timer_s ? stats->distance_mm / stats->timer_s : 0;
}

uint8_t sport_stats_avg_hr(const SportStats *stats) {
  return stats->hr_count ? (uint8_t)((stats->hr_sum + stats->hr_count / 2) / stats->hr_count) : 0;
}

uint16_t sport_stats_avg_cadence(const SportStats *stats) {
  return stats->cadence_count
             ? (uint16_t)((stats->cadence_sum + stats->cadence_count / 2) / stats->cadence_count)
             : 0;
}

uint16_t sport_stats_avg_power(const SportStats *stats) {
  return stats->power_count
             ? (uint16_t)((stats->power_sum + stats->power_count / 2) / stats->power_count)
             : 0;
}

void sport_engine_init(SportEngine *engine, const SportEngineConfig *cfg) {
  *engine = (SportEngine){.cfg = *cfg, .state = SportState_Ready};
}

static void prv_open_lap(SportEngine *engine, uint32_t carry_mm) {
  engine->lap = (SportLap){
    .stats = {.distance_mm = carry_mm},
    .start_elapsed_s = engine->elapsed_s,
    .number = (uint16_t)(engine->num_laps + 1),
  };
}

static void prv_close_lap(SportEngine *engine, SportLapTrigger trigger, uint32_t carry_mm) {
  engine->lap.trigger = (uint8_t)trigger;
  if (engine->num_laps < SPORT_ENGINE_MAX_LAPS) {
    engine->laps[engine->num_laps] = engine->lap;
  }
  engine->num_laps++;
  prv_open_lap(engine, carry_mm);
}

void sport_engine_start(SportEngine *engine) {
  if (engine->state != SportState_Ready) {
    return;
  }

  engine->state = SportState_Running;
  prv_open_lap(engine, 0);
}

void sport_engine_pause(SportEngine *engine, bool paused) {
  if (paused && (engine->state == SportState_Running || engine->state == SportState_AutoPaused)) {
    engine->state = SportState_Paused;
  } else if (!paused && engine->state == SportState_Paused) {
    engine->state = SportState_Running;
    // Nothing moved while paused counts: start again from where we are.
    engine->anchor_valid = false;
    engine->below_s = 0;
  }
}

void sport_engine_stop(SportEngine *engine) {
  if (!sport_engine_is_active(engine)) {
    return;
  }

  if (engine->lap.stats.timer_s > 0 || engine->lap.stats.distance_mm > 0) {
    prv_close_lap(engine, SportLapTrigger_Final, 0);
  }
  engine->state = SportState_Stopped;
}

bool sport_engine_lap(SportEngine *engine) {
  if (!sport_engine_is_active(engine) ||
      (engine->lap.stats.timer_s == 0 && engine->lap.stats.distance_mm == 0)) {
    return false;
  }

  prv_close_lap(engine, SportLapTrigger_Manual, 0);

  return true;
}

bool sport_engine_is_active(const SportEngine *engine) {
  return engine->state == SportState_Running || engine->state == SportState_Paused ||
         engine->state == SportState_AutoPaused;
}

const SportLap *sport_engine_last_lap(const SportEngine *engine) {
  if (engine->num_laps == 0) {
    return NULL;
  }

  return &engine->laps[(engine->num_laps <= SPORT_ENGINE_MAX_LAPS ? engine->num_laps
                                                                  : SPORT_ENGINE_MAX_LAPS) -
                       1];
}

uint32_t sport_engine_current_speed_mm_s(const SportEngine *engine) {
  uint32_t sum = 0;

  if (engine->state != SportState_Running || engine->window_len == 0) {
    return 0;
  }

  for (size_t i = 0; i < engine->window_len; i++) {
    sum += engine->window_mm[i];
  }

  return sum / engine->window_len;
}

bool sport_engine_elevation_cm(const SportEngine *engine, int32_t *alt_cm) {
  if (engine->alt_valid) {
    *alt_cm = engine->alt_cm;
  }

  return engine->alt_valid;
}

bool sport_engine_grade_x10(const SportEngine *engine, int16_t *grade_x10) {
  if (engine->grade_valid) {
    *grade_x10 = engine->grade_x10;
  }

  return engine->grade_valid;
}

static bool prv_fix_usable(const SportEngine *engine, const SportSample *s) {
  return (s->flags & SportSampleFlag_Fix) && s->sats >= engine->cfg.min_sats &&
         (s->hdop_x10 == 0 || s->hdop_x10 <= engine->cfg.max_hdop_x10);
}

// Distance moved by GNSS over the second; false if GNSS cannot tell.
static bool prv_gnss_distance(SportEngine *engine, const SportSample *s, uint32_t *dist_mm) {
  *dist_mm = 0;

  if (!prv_fix_usable(engine, s)) {
    // Whatever moves meanwhile is counted by another source: never bridge the gap.
    engine->anchor_valid = false;
    return false;
  }

  const bool doppler =
      (s->flags & SportSampleFlag_GnssSpeed) && s->gnss_speed_mm_s <= engine->cfg.max_speed_mm_s;
  const uint32_t doppler_mm =
      (doppler && s->gnss_speed_mm_s >= DOPPLER_MIN_MM_S) ? s->gnss_speed_mm_s : 0;

  if (!engine->anchor_valid) {
    engine->anchor_valid = true;
    engine->anchor_lat_e7 = s->lat_e7;
    engine->anchor_lon_e7 = s->lon_e7;
    engine->anchor_age_s = 0;
    engine->rejected_s = 0;
    *dist_mm = doppler_mm;
    return true;
  }

  if (engine->anchor_age_s < UINT8_MAX) {
    engine->anchor_age_s++;
  }

  const uint32_t d =
      sport_geo_distance_mm(engine->anchor_lat_e7, engine->anchor_lon_e7, s->lat_e7, s->lon_e7);

  if ((uint64_t)d > (uint64_t)engine->cfg.max_speed_mm_s * engine->anchor_age_s) {
    // An outlier: hold, the next good fix measures from the anchor again.
    if (++engine->rejected_s >= MAX_REJECTED_S) {
      engine->anchor_lat_e7 = s->lat_e7;
      engine->anchor_lon_e7 = s->lon_e7;
      engine->anchor_age_s = 0;
      engine->rejected_s = 0;
    }
    return true;
  }
  engine->rejected_s = 0;

  // Doppler speed is far less noisy than position differences: count it, and keep positions
  // for rejecting outliers.
  if (doppler) {
    *dist_mm = doppler_mm;
    engine->anchor_lat_e7 = s->lat_e7;
    engine->anchor_lon_e7 = s->lon_e7;
    engine->anchor_age_s = 0;
    return true;
  }

  if (d < engine->cfg.jitter_mm) {
    // Standing still, or too slow to tell yet: the anchor waits.
    return true;
  }

  *dist_mm = d;
  engine->anchor_lat_e7 = s->lat_e7;
  engine->anchor_lon_e7 = s->lon_e7;
  engine->anchor_age_s = 0;

  return true;
}

static uint32_t prv_auto_pause(SportEngine *engine) {
  const SportEngineConfig *cfg = &engine->cfg;

  if (!cfg->auto_pause || engine->source == SportDistanceSource_None) {
    return 0;
  }

  if (engine->state == SportState_Running) {
    engine->below_s = engine->speed_mm_s < cfg->pause_below_mm_s ? engine->below_s + 1 : 0;
    if (engine->below_s >= cfg->pause_after_s) {
      engine->state = SportState_AutoPaused;
      engine->above_s = 0;
      return SportEvent_AutoPause;
    }
  } else if (engine->state == SportState_AutoPaused) {
    engine->above_s = engine->speed_mm_s > cfg->resume_above_mm_s ? engine->above_s + 1 : 0;
    if (engine->above_s >= cfg->resume_after_s) {
      engine->state = SportState_Running;
      engine->below_s = 0;
      return SportEvent_AutoResume;
    }
  }

  return 0;
}

static void prv_elevation(SportEngine *engine, const SportSample *s, bool fix_usable) {
  bool baro;
  int32_t raw;

  if (s->flags & SportSampleFlag_BaroAlt) {
    baro = true;
    raw = s->baro_alt_cm;
  } else if ((s->flags & SportSampleFlag_GnssAlt) && fix_usable) {
    baro = false;
    raw = s->gnss_alt_cm;
  } else {
    return;
  }

  if (!engine->alt_valid || engine->alt_baro != baro) {
    engine->alt_valid = true;
    engine->alt_baro = baro;
    engine->alt_cm = raw;
    engine->alt_ref_cm = raw;
    engine->grade_len = 0;
    engine->grade_valid = false;
    return;
  }

  // Low-pass: GNSS altitude is noisy, the barometer less so.
  engine->alt_cm += (raw - engine->alt_cm) / (baro ? 2 : 4);

  const int32_t threshold =
      baro ? engine->cfg.climb_threshold_baro_cm : engine->cfg.climb_threshold_gnss_cm;
  const int32_t delta = engine->alt_cm - engine->alt_ref_cm;
  const bool counting = engine->state == SportState_Running;

  if (delta >= threshold) {
    if (counting) {
      engine->total.ascent_cm += (uint32_t)delta;
      engine->lap.stats.ascent_cm += (uint32_t)delta;
    }
    engine->alt_ref_cm = engine->alt_cm;
  } else if (-delta >= threshold) {
    if (counting) {
      engine->total.descent_cm += (uint32_t)-delta;
      engine->lap.stats.descent_cm += (uint32_t)-delta;
    }
    engine->alt_ref_cm = engine->alt_cm;
  }
}

static void prv_grade(SportEngine *engine) {
  if (!engine->alt_valid) {
    return;
  }

  engine->grade_alt_cm[engine->grade_pos] = engine->alt_cm;
  engine->grade_dist_mm[engine->grade_pos] = engine->total.distance_mm;
  engine->grade_pos = (uint8_t)((engine->grade_pos + 1) % SPORT_GRADE_WINDOW_S);
  if (engine->grade_len < SPORT_GRADE_WINDOW_S) {
    engine->grade_len++;
  }

  // The oldest entry is the next to be written once the window is full.
  const size_t oldest = engine->grade_len < SPORT_GRADE_WINDOW_S ? 0 : engine->grade_pos;
  const uint32_t dist = engine->total.distance_mm - engine->grade_dist_mm[oldest];

  if (dist < GRADE_MIN_DISTANCE_MM) {
    engine->grade_valid = false;
    return;
  }

  const int64_t rise_mm = (int64_t)(engine->alt_cm - engine->grade_alt_cm[oldest]) * 10;

  engine->grade_x10 = (int16_t)(rise_mm * 1000 / dist);
  engine->grade_valid = true;
}

static void prv_add_stats(SportStats *stats, const SportSample *s, uint32_t dist_mm,
                          uint32_t speed_mm_s) {
  stats->timer_s++;
  stats->distance_mm += dist_mm;
  if (speed_mm_s > stats->max_speed_mm_s) {
    stats->max_speed_mm_s = speed_mm_s;
  }
  if ((s->flags & SportSampleFlag_Hr) && s->hr_bpm > 0) {
    stats->hr_sum += s->hr_bpm;
    stats->hr_count++;
    if (s->hr_bpm > stats->hr_max) {
      stats->hr_max = s->hr_bpm;
    }
  }
  if ((s->flags & SportSampleFlag_Cadence) && s->cadence > 0) {
    stats->cadence_sum += s->cadence;
    stats->cadence_count++;
  }
  if (s->flags & SportSampleFlag_Power) {
    stats->power_sum += s->power_w;
    stats->power_count++;
    if (s->power_w > stats->power_max) {
      stats->power_max = s->power_w;
    }
  }
}

uint32_t sport_engine_tick(SportEngine *engine, const SportSample *s) {
  uint32_t events = 0;
  uint32_t gnss_mm;
  uint32_t dist_mm = 0;

  if (!sport_engine_is_active(engine)) {
    return 0;
  }

  engine->elapsed_s++;

  const bool fix_usable = prv_fix_usable(engine, s);
  const bool gnss_valid = prv_gnss_distance(engine, s, &gnss_mm);

  // Sources by preference: a speed sensor, GNSS, then steps.
  if (s->flags & SportSampleFlag_ExtSpeed) {
    engine->source = SportDistanceSource_External;
    dist_mm = s->ext_speed_mm_s;
    engine->speed_mm_s = s->ext_speed_mm_s;
  } else if (gnss_valid) {
    engine->source = SportDistanceSource_Gnss;
    dist_mm = gnss_mm;
    engine->speed_mm_s = (s->flags & SportSampleFlag_GnssSpeed) ? s->gnss_speed_mm_s : gnss_mm;
  } else if (s->flags & SportSampleFlag_StepDistance) {
    engine->source = SportDistanceSource_Steps;
    dist_mm = s->step_distance_mm;
    engine->speed_mm_s = s->step_distance_mm;
  } else {
    engine->source = SportDistanceSource_None;
    engine->speed_mm_s = 0;
  }

  if (engine->state != SportState_Paused) {
    events |= prv_auto_pause(engine);
  }

  const bool running = engine->state == SportState_Running;

  if (!running) {
    dist_mm = 0;
  }
  engine->window_mm[engine->window_pos] = dist_mm;
  engine->window_pos = (uint8_t)((engine->window_pos + 1) % SPORT_SPEED_WINDOW_S);
  if (engine->window_len < SPORT_SPEED_WINDOW_S) {
    engine->window_len++;
  }

  prv_elevation(engine, s, fix_usable);

  if (!running) {
    return events;
  }

  const uint32_t speed_mm_s = sport_engine_current_speed_mm_s(engine);

  prv_add_stats(&engine->total, s, dist_mm, speed_mm_s);
  prv_add_stats(&engine->lap.stats, s, dist_mm, speed_mm_s);
  prv_grade(engine);

  if (engine->cfg.auto_lap_mm != 0 && engine->lap.stats.distance_mm >= engine->cfg.auto_lap_mm) {
    const uint32_t carry = engine->lap.stats.distance_mm - engine->cfg.auto_lap_mm;

    engine->lap.stats.distance_mm = engine->cfg.auto_lap_mm;
    prv_close_lap(engine, SportLapTrigger_Auto, carry);
    events |= SportEvent_Lap;
  }

  return events;
}
