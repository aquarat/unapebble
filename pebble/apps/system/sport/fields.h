/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "engine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Data fields: a field is a metric and its formatter, a screen a list of fields.

#define SPORT_UNIT_KM_MM 1000000U
#define SPORT_UNIT_MI_MM 1609344U

#define SPORT_SCREEN_MAX_FIELDS 3

typedef enum SportMetric {
  SportMetric_Timer,
  SportMetric_Elapsed,
  SportMetric_Distance,
  SportMetric_Pace,
  SportMetric_AvgPace,
  SportMetric_LapPace,
  SportMetric_LapTime,
  SportMetric_LapDistance,
  SportMetric_Speed,
  SportMetric_AvgSpeed,
  SportMetric_MaxSpeed,
  SportMetric_LapSpeed,
  SportMetric_HeartRate,
  SportMetric_HrZone,
  SportMetric_AvgHr,
  SportMetric_Cadence,
  SportMetric_AvgCadence,
  SportMetric_Elevation,
  SportMetric_Ascent,
  SportMetric_Grade,
  SportMetric_Power,
  SportMetric_AvgPower,
  SportMetric_Clock,
  SportMetric_Laps,
  SportMetricCount,
} SportMetric;

typedef struct SportScreen {
  uint8_t num_fields;
  SportMetric fields[SPORT_SCREEN_MAX_FIELDS];
} SportScreen;

//! Live values a field may show, besides the engine's.
typedef struct SportFieldContext {
  const SportEngine *engine;
  //! Distance unit: SPORT_UNIT_KM_MM or SPORT_UNIT_MI_MM.
  uint32_t unit_mm;
  uint8_t hr_bpm;
  uint8_t hr_zone;
  uint16_t cadence;
  uint16_t power_w;
  bool has_hr;
  bool has_cadence;
  bool has_power;
  //! Time of day.
  uint8_t hour;
  uint8_t minute;
} SportFieldContext;

//! Upper-case label, e.g. "PACE".
const char *sport_metric_label(SportMetric metric);

//! Unit for the distance unit in use, e.g. "/km", or "".
const char *sport_metric_unit(SportMetric metric, uint32_t unit_mm);

//! Format the value, "--" when there is none.
void sport_metric_format(SportMetric metric, const SportFieldContext *ctx, char *buf, size_t len);

//! Format seconds as H:MM:SS, or M:SS under an hour.
void sport_format_duration(uint32_t s, char *buf, size_t len);

//! Format a pace in seconds per unit as M:SS, "--" for none or slower than 60 min per unit.
void sport_format_pace(uint32_t pace_s, char *buf, size_t len);

//! Format a distance in the unit with two decimals, truncated.
void sport_format_distance(uint32_t distance_mm, uint32_t unit_mm, char *buf, size_t len);

//! Format a speed in the unit per hour with one decimal.
void sport_format_speed(uint32_t speed_mm_s, uint32_t unit_mm, char *buf, size_t len);

//! Format an elevation in metres, or feet with the mile unit.
void sport_format_elevation(int32_t alt_cm, uint32_t unit_mm, char *buf, size_t len);
