/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "fields.h"

#include <inttypes.h>
#include <stdio.h>

#define NO_VALUE "--"
// Paces slower than this are not shown.
#define MAX_PACE_S (60U * 60U)

typedef struct MetricInfo {
  const char *label;
  //! Unit with kilometres, then with miles.
  const char *unit_km;
  const char *unit_mi;
} MetricInfo;

static const MetricInfo s_metrics[SportMetricCount] = {
  [SportMetric_Timer] = {"TIMER", "", ""},
  [SportMetric_Elapsed] = {"ELAPSED", "", ""},
  [SportMetric_Distance] = {"DISTANCE", "km", "mi"},
  [SportMetric_Pace] = {"PACE", "/km", "/mi"},
  [SportMetric_AvgPace] = {"AVG PACE", "/km", "/mi"},
  [SportMetric_LapPace] = {"LAP PACE", "/km", "/mi"},
  [SportMetric_LapTime] = {"LAP TIME", "", ""},
  [SportMetric_LapDistance] = {"LAP DIST", "km", "mi"},
  [SportMetric_Speed] = {"SPEED", "km/h", "mph"},
  [SportMetric_AvgSpeed] = {"AVG SPEED", "km/h", "mph"},
  [SportMetric_MaxSpeed] = {"MAX SPEED", "km/h", "mph"},
  [SportMetric_LapSpeed] = {"LAP SPEED", "km/h", "mph"},
  [SportMetric_HeartRate] = {"HEART RATE", "bpm", "bpm"},
  [SportMetric_HrZone] = {"HR ZONE", "", ""},
  [SportMetric_AvgHr] = {"AVG HR", "bpm", "bpm"},
  [SportMetric_Cadence] = {"CADENCE", "spm", "spm"},
  [SportMetric_AvgCadence] = {"AVG CAD", "spm", "spm"},
  [SportMetric_Elevation] = {"ELEVATION", "m", "ft"},
  [SportMetric_Ascent] = {"ASCENT", "m", "ft"},
  [SportMetric_Grade] = {"GRADE", "%", "%"},
  [SportMetric_Power] = {"POWER", "W", "W"},
  [SportMetric_AvgPower] = {"AVG POWER", "W", "W"},
  [SportMetric_Clock] = {"TIME", "", ""},
  [SportMetric_Laps] = {"LAPS", "", ""},
};

const char *sport_metric_label(SportMetric metric) {
  return metric < SportMetricCount ? s_metrics[metric].label : "";
}

const char *sport_metric_unit(SportMetric metric, uint32_t unit_mm) {
  if (metric >= SportMetricCount) {
    return "";
  }

  return unit_mm == SPORT_UNIT_MI_MM ? s_metrics[metric].unit_mi : s_metrics[metric].unit_km;
}

void sport_format_duration(uint32_t s, char *buf, size_t len) {
  const uint32_t h = s / 3600;
  const uint32_t m = (s / 60) % 60;

  if (h > 0) {
    snprintf(buf, len, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, h, m, s % 60);
  } else {
    snprintf(buf, len, "%" PRIu32 ":%02" PRIu32, m, s % 60);
  }
}

void sport_format_pace(uint32_t pace_s, char *buf, size_t len) {
  if (pace_s == 0 || pace_s >= MAX_PACE_S) {
    snprintf(buf, len, NO_VALUE);
    return;
  }

  snprintf(buf, len, "%" PRIu32 ":%02" PRIu32, pace_s / 60, pace_s % 60);
}

void sport_format_distance(uint32_t distance_mm, uint32_t unit_mm, char *buf, size_t len) {
  const uint32_t hundredths = (uint32_t)((uint64_t)distance_mm * 100 / unit_mm);

  snprintf(buf, len, "%" PRIu32 ".%02" PRIu32, hundredths / 100, hundredths % 100);
}

void sport_format_speed(uint32_t speed_mm_s, uint32_t unit_mm, char *buf, size_t len) {
  const uint32_t tenths = (uint32_t)(((uint64_t)speed_mm_s * 36000 + unit_mm / 2) / unit_mm);

  snprintf(buf, len, "%" PRIu32 ".%" PRIu32, tenths / 10, tenths % 10);
}

void sport_format_elevation(int32_t alt_cm, uint32_t unit_mm, char *buf, size_t len) {
  // Round half away from zero.
  int32_t value;

  if (unit_mm == SPORT_UNIT_MI_MM) {
    const int64_t num = (int64_t)alt_cm * 100;
    value = (int32_t)((num + (num < 0 ? -1524 : 1524)) / 3048);
  } else {
    value = (alt_cm + (alt_cm < 0 ? -50 : 50)) / 100;
  }

  snprintf(buf, len, "%" PRId32, value);
}

static void prv_format_lap_pace(const SportFieldContext *ctx, char *buf, size_t len) {
  const SportStats *lap = &ctx->engine->lap.stats;

  sport_format_pace(sport_pace_s(sport_stats_avg_speed_mm_s(lap), ctx->unit_mm), buf, len);
}

static void prv_format_uint_or_none(bool valid, uint32_t value, char *buf, size_t len) {
  if (!valid) {
    snprintf(buf, len, NO_VALUE);
    return;
  }

  snprintf(buf, len, "%" PRIu32, value);
}

void sport_metric_format(SportMetric metric, const SportFieldContext *ctx, char *buf, size_t len) {
  const SportEngine *e = ctx->engine;
  const SportStats *total = &e->total;
  const SportStats *lap = &e->lap.stats;
  const uint32_t unit = ctx->unit_mm;

  switch (metric) {
    case SportMetric_Timer:
      sport_format_duration(total->timer_s, buf, len);
      return;
    case SportMetric_Elapsed:
      sport_format_duration(e->elapsed_s, buf, len);
      return;
    case SportMetric_Distance:
      sport_format_distance(total->distance_mm, unit, buf, len);
      return;
    case SportMetric_Pace:
      sport_format_pace(sport_pace_s(sport_engine_current_speed_mm_s(e), unit), buf, len);
      return;
    case SportMetric_AvgPace:
      sport_format_pace(sport_pace_s(sport_stats_avg_speed_mm_s(total), unit), buf, len);
      return;
    case SportMetric_LapPace:
      prv_format_lap_pace(ctx, buf, len);
      return;
    case SportMetric_LapTime:
      sport_format_duration(lap->timer_s, buf, len);
      return;
    case SportMetric_LapDistance:
      sport_format_distance(lap->distance_mm, unit, buf, len);
      return;
    case SportMetric_Speed:
      sport_format_speed(sport_engine_current_speed_mm_s(e), unit, buf, len);
      return;
    case SportMetric_AvgSpeed:
      sport_format_speed(sport_stats_avg_speed_mm_s(total), unit, buf, len);
      return;
    case SportMetric_MaxSpeed:
      sport_format_speed(total->max_speed_mm_s, unit, buf, len);
      return;
    case SportMetric_LapSpeed:
      sport_format_speed(sport_stats_avg_speed_mm_s(lap), unit, buf, len);
      return;
    case SportMetric_HeartRate:
      prv_format_uint_or_none(ctx->has_hr, ctx->hr_bpm, buf, len);
      return;
    case SportMetric_HrZone:
      prv_format_uint_or_none(ctx->has_hr, ctx->hr_zone, buf, len);
      return;
    case SportMetric_AvgHr:
      prv_format_uint_or_none(total->hr_count > 0, sport_stats_avg_hr(total), buf, len);
      return;
    case SportMetric_Cadence:
      prv_format_uint_or_none(ctx->has_cadence, ctx->cadence, buf, len);
      return;
    case SportMetric_AvgCadence:
      prv_format_uint_or_none(total->cadence_count > 0, sport_stats_avg_cadence(total), buf, len);
      return;
    case SportMetric_Elevation: {
      int32_t alt_cm;
      if (!sport_engine_elevation_cm(e, &alt_cm)) {
        snprintf(buf, len, NO_VALUE);
        return;
      }
      sport_format_elevation(alt_cm, unit, buf, len);
      return;
    }
    case SportMetric_Ascent:
      if (!e->alt_valid) {
        snprintf(buf, len, NO_VALUE);
        return;
      }
      sport_format_elevation((int32_t)total->ascent_cm, unit, buf, len);
      return;
    case SportMetric_Grade: {
      int16_t g;
      if (!sport_engine_grade_x10(e, &g)) {
        snprintf(buf, len, NO_VALUE);
        return;
      }
      const int a = g < 0 ? -g : g;
      snprintf(buf, len, "%s%d.%d", g < 0 ? "-" : "", a / 10, a % 10);
      return;
    }
    case SportMetric_Power:
      prv_format_uint_or_none(ctx->has_power, ctx->power_w, buf, len);
      return;
    case SportMetric_AvgPower:
      prv_format_uint_or_none(total->power_count > 0, sport_stats_avg_power(total), buf, len);
      return;
    case SportMetric_Clock:
      snprintf(buf, len, "%u:%02u", ctx->hour, ctx->minute);
      return;
    case SportMetric_Laps:
      snprintf(buf, len, "%u", (unsigned)(e->num_laps + 1));
      return;
    case SportMetricCount:
      break;
  }

  snprintf(buf, len, NO_VALUE);
}
