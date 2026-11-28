/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "sensors.h"

#include "profile.h"

#include <string.h>

#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/services/activity/activity_calculators.h>
#include <pbl/services/activity/workout_service.h>
#include <pbl/util/size.h>
#ifdef CONFIG_SERVICE_LOCATION
#include <pbl/services/location.h>
#endif

#include <syscall/syscall.h>

// External values older than this are dropped.
#define EXTERNAL_MAX_AGE_MS 3000U
// Steps are counted over this many seconds for the cadence.
#define CADENCE_WINDOW_S 16
// Calibrate the barometer on a GNSS altitude of a fix at least this good.
#define BARO_CAL_MAX_HDOP_X10 15

typedef struct SportSensors {
  uint32_t sensors;
  bool workout;
  bool baro;
  bool baro_calibrated;
  int32_t baro_offset_cm;
  SportGnssStatus gnss;
  uint8_t sats;
  uint32_t cursor;
  //! UTC of the last epoch with a time, and whether it followed the one before by a second.
  uint64_t last_utc_ms;
  bool utc_ok;
  int32_t last_steps;
  int32_t step_window[CADENCE_WINDOW_S];
  uint8_t step_pos;
  uint8_t step_len;
  uint32_t external[SportExternalCount];
  uint64_t external_ms[SportExternalCount];
} SportSensors;

static SportSensors s_sensors;

int32_t sport_pressure_to_altitude_cm(uint32_t pa) {
  // h = 44330.8 m * (1 - (p / p0) ^ 0.190263), with series for ln and exp: no libm.
  const float p0 = 101325.0f;
  const float y = ((float)pa - p0) / ((float)pa + p0);
  const float y2 = y * y;
  const float ln_r = 2.0f * y * (1.0f + y2 / 3.0f + y2 * y2 / 5.0f + y2 * y2 * y2 / 7.0f);
  const float x = 0.190263f * ln_r;
  const float pow_r = 1.0f + x * (1.0f + x / 2.0f * (1.0f + x / 3.0f * (1.0f + x / 4.0f)));

  return (int32_t)(4433080.0f * (1.0f - pow_r));
}

void sport_sensors_gnss_start(void) {
#ifdef CONFIG_SERVICE_LOCATION
  if ((s_sensors.sensors & SportSensor_Gnss) && s_sensors.gnss == SportGnssStatus_None &&
      location_service_start() == 0) {
    s_sensors.gnss = SportGnssStatus_Searching;
    s_sensors.cursor = location_service_get_cursor();
    s_sensors.last_utc_ms = 0;
    s_sensors.utc_ok = false;
  }
#endif
}

void sport_sensors_gnss_stop(void) {
#ifdef CONFIG_SERVICE_LOCATION
  if (s_sensors.gnss != SportGnssStatus_None) {
    location_service_stop();
  }
#endif
  s_sensors.gnss = SportGnssStatus_None;
  s_sensors.utc_ok = false;
}

bool sport_sensors_gnss_utc(uint32_t *utc_s) {
  if (!s_sensors.utc_ok) {
    return false;
  }
  *utc_s = (uint32_t)(s_sensors.last_utc_ms / 1000U);

  return true;
}

void sport_sensors_init(uint32_t sensors) {
  s_sensors = (SportSensors){.sensors = sensors};

  sport_sensors_gnss_start();

  if (sensors & SportSensor_Baro) {
    uint32_t pa;
    s_sensors.baro = sys_pressure_read(&pa) == 0;
  }

  PBL_LOG_DBG("sensors 0x%" PRIx32 ", gnss %d, baro %d", sensors, s_sensors.gnss, s_sensors.baro);
}

void sport_sensors_deinit(void) {
  sport_sensors_gnss_stop();
}

void sport_sensors_set_workout(bool active) {
  s_sensors.workout = active;
  s_sensors.last_steps = -1;
  s_sensors.step_len = 0;
}

void sport_sensors_external_report(SportExternal which, uint32_t value) {
  if (which >= SportExternalCount || !(s_sensors.sensors & SportSensor_External)) {
    return;
  }

  s_sensors.external[which] = value;
  s_sensors.external_ms[which] = pbl_uptime_ms();
}

static bool prv_external(SportExternal which, uint32_t *value) {
  if (s_sensors.external_ms[which] == 0 ||
      pbl_uptime_ms() - s_sensors.external_ms[which] > EXTERNAL_MAX_AGE_MS) {
    return false;
  }

  *value = s_sensors.external[which];

  return true;
}

static void prv_poll_gnss(SportSample *sample) {
#ifdef CONFIG_SERVICE_LOCATION
  struct pbl_gnss_fix fixes[LOCATION_SERVICE_HISTORY];
  size_t n;

  if (s_sensors.gnss == SportGnssStatus_None) {
    return;
  }

  n = location_service_read(&s_sensors.cursor, fixes, PBL_ARRAY_SIZE(fixes));
  if (n == 0) {
    if (location_service_get_status() != LocationStatus_Fix) {
      s_sensors.gnss = SportGnssStatus_Searching;
    }
    return;
  }

  // A time is trusted once two epochs a second apart agree on it.
  for (size_t i = 0; i < n; i++) {
    const uint64_t utc_ms = fixes[i].utc_ms;
    s_sensors.utc_ok = utc_ms != 0 && s_sensors.last_utc_ms != 0 &&
                       utc_ms - s_sensors.last_utc_ms >= 900U &&
                       utc_ms - s_sensors.last_utc_ms <= 1100U;
    s_sensors.last_utc_ms = utc_ms;
  }

  // The engine counts from the last position it took, so the latest epoch is enough.
  const struct pbl_gnss_fix *fix = &fixes[n - 1];

  s_sensors.sats = fix->sats_used;
  if (fix->quality == PBL_GNSS_FIX_NONE) {
    s_sensors.gnss = SportGnssStatus_Searching;
    return;
  }

  s_sensors.gnss = SportGnssStatus_Fix;
  sample->flags |= SportSampleFlag_Fix | SportSampleFlag_GnssSpeed;
  sample->lat_e7 = fix->lat_e7;
  sample->lon_e7 = fix->lon_e7;
  sample->hdop_x10 = fix->hdop_x10;
  sample->sats = fix->sats_used;
  sample->gnss_speed_mm_s = fix->speed_mm_s;
  if (fix->quality >= PBL_GNSS_FIX_3D) {
    sample->flags |= SportSampleFlag_GnssAlt;
    sample->gnss_alt_cm = fix->alt_cm;
  }
#endif
}

static void prv_poll_baro(SportSample *sample) {
  uint32_t pa;

  if (!s_sensors.baro || sys_pressure_read(&pa) != 0) {
    return;
  }

  int32_t alt_cm = sport_pressure_to_altitude_cm(pa);

  // The weather moves the barometer's zero: take it from the first good GNSS altitude.
  if (!s_sensors.baro_calibrated && (sample->flags & SportSampleFlag_GnssAlt) &&
      sample->hdop_x10 != 0 && sample->hdop_x10 <= BARO_CAL_MAX_HDOP_X10) {
    s_sensors.baro_offset_cm = sample->gnss_alt_cm - alt_cm;
    s_sensors.baro_calibrated = true;
  }

  sample->flags |= SportSampleFlag_BaroAlt;
  sample->baro_alt_cm = alt_cm + s_sensors.baro_offset_cm;
}

static void prv_poll_workout(SportSample *sample) {
  WorkoutServiceMetrics metrics;

  if (!s_sensors.workout || !workout_service_get_current_workout_metrics(&metrics)) {
    return;
  }

  if ((s_sensors.sensors & SportSensor_Hr) && metrics.current_bpm > 0) {
    sample->flags |= SportSampleFlag_Hr;
    sample->hr_bpm = (uint8_t)metrics.current_bpm;
  }

  if (!(s_sensors.sensors & SportSensor_Steps)) {
    return;
  }

  int32_t delta = 0;

  if (s_sensors.last_steps >= 0 && metrics.steps >= s_sensors.last_steps) {
    delta = metrics.steps - s_sensors.last_steps;
  }
  s_sensors.last_steps = metrics.steps;

  s_sensors.step_window[s_sensors.step_pos] = delta;
  s_sensors.step_pos = (uint8_t)((s_sensors.step_pos + 1) % CADENCE_WINDOW_S);
  if (s_sensors.step_len < CADENCE_WINDOW_S) {
    s_sensors.step_len++;
  }

  // Steps come in bursts: spread them over the window, which keeps the total.
  int32_t steps = 0;
  for (size_t i = 0; i < s_sensors.step_len; i++) {
    steps += s_sensors.step_window[i];
  }
  const uint32_t window_ms = s_sensors.step_len * 1000U;

  sample->flags |= SportSampleFlag_StepDistance;
  sample->step_distance_mm =
      activity_private_compute_distance_mm((uint32_t)steps, window_ms) / s_sensors.step_len;
  if (s_sensors.step_len == CADENCE_WINDOW_S) {
    sample->flags |= SportSampleFlag_Cadence;
    sample->cadence = (uint16_t)(steps * 60 / CADENCE_WINDOW_S);
  }
}

static void prv_poll_external(SportSample *sample) {
  uint32_t v;

  if (prv_external(SportExternal_Hr, &v)) {
    sample->flags |= SportSampleFlag_Hr;
    sample->hr_bpm = (uint8_t)v;
  }
  if (prv_external(SportExternal_Speed, &v)) {
    sample->flags |= SportSampleFlag_ExtSpeed;
    sample->ext_speed_mm_s = v;
  }
  if (prv_external(SportExternal_Cadence, &v)) {
    sample->flags |= SportSampleFlag_Cadence;
    sample->cadence = (uint16_t)v;
  }
  if (prv_external(SportExternal_Power, &v)) {
    sample->flags |= SportSampleFlag_Power;
    sample->power_w = (uint16_t)v;
  }
}

void sport_sensors_poll(SportSample *sample) {
  *sample = (SportSample){};

  prv_poll_gnss(sample);
  prv_poll_baro(sample);
  prv_poll_workout(sample);
  prv_poll_external(sample);
}

SportSensorsStatus sport_sensors_get_status(void) {
  return (SportSensorsStatus){
    .gnss = s_sensors.gnss,
    .sats = s_sensors.sats,
    .baro = s_sensors.baro,
  };
}
