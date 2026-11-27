/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Sport activity engine: the session state machine, distance and speed filtering, auto-pause,
//! laps, elevation and the statistics, for every sport. Pure logic fed one sample per second, so
//! that it runs unchanged in host tests.

//! Stored lap summaries; laps past this are counted but only kept in the recording.
#define SPORT_ENGINE_MAX_LAPS 40
//! Seconds of distance averaged for the current speed and pace.
#define SPORT_SPEED_WINDOW_S 8
//! Seconds of elevation and distance the grade is taken over.
#define SPORT_GRADE_WINDOW_S 16

typedef enum SportState {
  SportState_Ready,
  SportState_Running,
  SportState_Paused,
  SportState_AutoPaused,
  SportState_Stopped,
} SportState;

//! What a sample carries. Sources other than GNSS and the accelerometer, such as BLE speed,
//! cadence and power sensors, fill the same fields.
typedef enum SportSampleFlag {
  //! GNSS position, with its quality: lat_e7, lon_e7, hdop_x10, sats.
  SportSampleFlag_Fix = 1 << 0,
  //! GNSS ground (Doppler) speed; preferred over position differences for distance.
  SportSampleFlag_GnssSpeed = 1 << 1,
  //! GNSS altitude.
  SportSampleFlag_GnssAlt = 1 << 2,
  //! Barometric altitude.
  SportSampleFlag_BaroAlt = 1 << 3,
  //! Distance from steps over the second.
  SportSampleFlag_StepDistance = 1 << 4,
  //! Heart rate.
  SportSampleFlag_Hr = 1 << 5,
  //! Cadence, steps or revolutions per minute.
  SportSampleFlag_Cadence = 1 << 6,
  //! Speed from a sensor of its own, such as a wheel; preferred over GNSS.
  SportSampleFlag_ExtSpeed = 1 << 7,
  //! Power.
  SportSampleFlag_Power = 1 << 8,
} SportSampleFlag;

//! One second of sensor data.
typedef struct SportSample {
  uint16_t flags;
  uint8_t sats;
  uint8_t hr_bpm;
  uint16_t hdop_x10;
  uint16_t cadence;
  int32_t lat_e7;
  int32_t lon_e7;
  uint32_t gnss_speed_mm_s;
  int32_t gnss_alt_cm;
  int32_t baro_alt_cm;
  uint32_t step_distance_mm;
  uint32_t ext_speed_mm_s;
  uint16_t power_w;
} SportSample;

typedef struct SportEngineConfig {
  //! Auto-lap distance, 0 for none.
  uint32_t auto_lap_mm;
  bool auto_pause;
  //! Auto-pause below this speed for pause_after_s seconds...
  uint32_t pause_below_mm_s;
  //! ... and resume above this one, higher, for resume_after_s seconds.
  uint32_t resume_above_mm_s;
  uint8_t pause_after_s;
  uint8_t resume_after_s;
  //! Fixes implying a faster move than this are outliers.
  uint32_t max_speed_mm_s;
  //! Fixes with a worse HDOP or fewer satellites are dropped.
  uint16_t max_hdop_x10;
  uint8_t min_sats;
  //! Without GNSS speed, moves shorter than this from the last position counted are jitter.
  uint16_t jitter_mm;
  //! Elevation change counted as ascent or descent, per altitude source.
  uint16_t climb_threshold_baro_cm;
  uint16_t climb_threshold_gnss_cm;
} SportEngineConfig;

typedef enum SportDistanceSource {
  SportDistanceSource_None,
  SportDistanceSource_Gnss,
  SportDistanceSource_Steps,
  SportDistanceSource_External,
} SportDistanceSource;

typedef enum SportLapTrigger {
  SportLapTrigger_Auto,
  SportLapTrigger_Manual,
  SportLapTrigger_Final,
} SportLapTrigger;

typedef struct SportStats {
  uint32_t timer_s;
  uint32_t distance_mm;
  uint32_t ascent_cm;
  uint32_t descent_cm;
  uint32_t max_speed_mm_s;
  uint32_t hr_sum;
  uint32_t hr_count;
  uint32_t cadence_sum;
  uint32_t cadence_count;
  uint32_t power_sum;
  uint32_t power_count;
  uint16_t power_max;
  uint8_t hr_max;
} SportStats;

typedef struct SportLap {
  SportStats stats;
  //! Engine elapsed time at the start of the lap.
  uint32_t start_elapsed_s;
  //! 1-based.
  uint16_t number;
  uint8_t trigger;
} SportLap;

typedef enum SportEvent {
  SportEvent_Lap = 1 << 0,
  SportEvent_AutoPause = 1 << 1,
  SportEvent_AutoResume = 1 << 2,
} SportEvent;

typedef struct SportEngine {
  SportEngineConfig cfg;
  SportState state;
  //! Seconds since the start, pauses included.
  uint32_t elapsed_s;
  SportStats total;
  SportLap lap;
  SportLap laps[SPORT_ENGINE_MAX_LAPS];
  uint16_t num_laps;
  SportDistanceSource source;

  //! Instantaneous speed, for auto-pause.
  uint32_t speed_mm_s;
  uint8_t below_s;
  uint8_t above_s;

  //! Last GNSS position counted.
  bool anchor_valid;
  uint8_t anchor_age_s;
  uint8_t rejected_s;
  int32_t anchor_lat_e7;
  int32_t anchor_lon_e7;

  uint32_t window_mm[SPORT_SPEED_WINDOW_S];
  uint8_t window_pos;
  uint8_t window_len;

  //! Smoothed elevation and the reference climbs are counted from.
  bool alt_valid;
  bool alt_baro;
  int32_t alt_cm;
  int32_t alt_ref_cm;

  int32_t grade_alt_cm[SPORT_GRADE_WINDOW_S];
  uint32_t grade_dist_mm[SPORT_GRADE_WINDOW_S];
  uint8_t grade_pos;
  uint8_t grade_len;
  bool grade_valid;
  //! Grade, in tenths of a percent.
  int16_t grade_x10;
} SportEngine;

void sport_engine_init(SportEngine *engine, const SportEngineConfig *cfg);

void sport_engine_start(SportEngine *engine);

//! Pause or resume by hand. Distance moved while paused is not counted.
void sport_engine_pause(SportEngine *engine, bool paused);

//! Stop, closing the last lap.
void sport_engine_stop(SportEngine *engine);

//! Feed one second. Returns the SportEvent bits that happened.
uint32_t sport_engine_tick(SportEngine *engine, const SportSample *sample);

//! Close the current lap by hand. Returns false if it is empty.
bool sport_engine_lap(SportEngine *engine);

bool sport_engine_is_active(const SportEngine *engine);

//! Last lap closed, or NULL.
const SportLap *sport_engine_last_lap(const SportEngine *engine);

//! Speed over the last few seconds; 0 while paused.
uint32_t sport_engine_current_speed_mm_s(const SportEngine *engine);

//! Elevation, false without an altitude source yet.
bool sport_engine_elevation_cm(const SportEngine *engine, int32_t *alt_cm);

//! Grade, false until enough distance was covered on the window.
bool sport_engine_grade_x10(const SportEngine *engine, int16_t *grade_x10);

uint32_t sport_stats_avg_speed_mm_s(const SportStats *stats);
uint8_t sport_stats_avg_hr(const SportStats *stats);
uint16_t sport_stats_avg_cadence(const SportStats *stats);
uint16_t sport_stats_avg_power(const SportStats *stats);

//! Seconds per @p unit_mm at @p speed_mm_s, 0 for no speed.
uint32_t sport_pace_s(uint32_t speed_mm_s, uint32_t unit_mm);

//! Distance between two positions, in millimetres; accurate to well under 1% at lap scale.
uint32_t sport_geo_distance_mm(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7);
