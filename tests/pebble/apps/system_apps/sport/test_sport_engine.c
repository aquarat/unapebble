/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <apps/system/sport/engine.h>
#include <apps/system/sport/fields.h>
#include <clar.h>

#define ORIGIN_LAT_E7 473769000
#define ORIGIN_LON_E7 85417000

static SportEngine s_engine;
static const SportEngineConfig s_run_cfg = {
  .auto_lap_mm = 1000000,
  .auto_pause = true,
  .pause_below_mm_s = 500,
  .resume_above_mm_s = 1000,
  .pause_after_s = 4,
  .resume_after_s = 2,
  .max_speed_mm_s = 11000,
  .max_hdop_x10 = 50,
  .min_sats = 4,
  .jitter_mm = 3000,
  .climb_threshold_baro_cm = 100,
  .climb_threshold_gnss_cm = 400,
};

// Runner on a straight line north, with the receiver's position noise.
static uint32_t s_pos_mm;
static uint32_t s_rng;

static int32_t prv_noise(int32_t range) {
  s_rng = s_rng * 1664525U + 1013904223U;
  return range == 0 ? 0 : (int32_t)((s_rng >> 8) % (uint32_t)(2 * range + 1)) - range;
}

static SportSample prv_fix(uint32_t speed_mm_s, int32_t noise_mm) {
  s_pos_mm += speed_mm_s;
  const int64_t north_mm = (int64_t)s_pos_mm + prv_noise(noise_mm);
  return (SportSample){
    .flags = SportSampleFlag_Fix | SportSampleFlag_GnssSpeed,
    .lat_e7 = ORIGIN_LAT_E7 + (int32_t)(north_mm * 10000 / 111195),
    .lon_e7 = ORIGIN_LON_E7 + prv_noise(noise_mm) / 8,
    .hdop_x10 = 12,
    .sats = 12,
    .gnss_speed_mm_s = speed_mm_s,
  };
}

static uint32_t prv_run_with(uint32_t seconds, uint32_t speed_mm_s, int32_t noise_mm,
                             bool doppler) {
  uint32_t events = 0;
  for (uint32_t i = 0; i < seconds; i++) {
    SportSample s = prv_fix(speed_mm_s, noise_mm);
    if (!doppler) {
      s.flags &= ~SportSampleFlag_GnssSpeed;
    }
    events |= sport_engine_tick(&s_engine, &s);
  }
  return events;
}

static uint32_t prv_run(uint32_t seconds, uint32_t speed_mm_s, int32_t noise_mm) {
  return prv_run_with(seconds, speed_mm_s, noise_mm, true);
}

static void prv_start(const SportEngineConfig *cfg) {
  sport_engine_init(&s_engine, cfg);
  sport_engine_start(&s_engine);
}

#define assert_near(a, e, t)                                                                     \
  do {                                                                                           \
    const int64_t a_ = (a), e_ = (e), t_ = (t);                                                  \
    if (llabs(a_ - e_) > t_) {                                                                   \
      char msg_[96];                                                                             \
      snprintf(msg_, sizeof(msg_), "%lld not within %lld of %lld", (long long)a_, (long long)t_, \
               (long long)e_);                                                                   \
      cl_fail(msg_);                                                                             \
    }                                                                                            \
  } while (0)

void test_sport_engine__initialize(void) {
  s_pos_mm = 0;
  s_rng = 1;
}

// Geometry ////////////////////////////////////////////////////////////////////////////////////////

void test_sport_engine__geo_distance(void) {
  // 0.001 degree of latitude is 111.2 m anywhere.
  assert_near(
      sport_geo_distance_mm(ORIGIN_LAT_E7, ORIGIN_LON_E7, ORIGIN_LAT_E7 + 10000, ORIGIN_LON_E7),
      111195, 100);
  // 0.001 degree of longitude at 47.4 N is 75.3 m.
  assert_near(
      sport_geo_distance_mm(ORIGIN_LAT_E7, ORIGIN_LON_E7, ORIGIN_LAT_E7, ORIGIN_LON_E7 + 10000),
      75270, 200);
  cl_assert_equal_i(sport_geo_distance_mm(1, 2, 1, 2), 0);
}

void test_sport_engine__pace(void) {
  cl_assert_equal_i(sport_pace_s(3333, SPORT_UNIT_KM_MM), 300);
  cl_assert_equal_i(sport_pace_s(3333, SPORT_UNIT_MI_MM), 483);
  cl_assert_equal_i(sport_pace_s(0, SPORT_UNIT_KM_MM), 0);
}

// Distance filtering //////////////////////////////////////////////////////////////////////////////

void test_sport_engine__distance_follows_a_clean_track(void) {
  prv_start(&s_run_cfg);
  prv_run(300, 3000, 0);

  assert_near(s_engine.total.distance_mm, 900000, 9000);
  cl_assert_equal_i(s_engine.total.timer_s, 300);
  cl_assert_equal_i(s_engine.source, SportDistanceSource_Gnss);
  assert_near(sport_engine_current_speed_mm_s(&s_engine), 3000, 300);
}

void test_sport_engine__distance_with_noise_stays_within_two_percent(void) {
  prv_start(&s_run_cfg);
  prv_run(600, 3000, 1500);

  assert_near(s_engine.total.distance_mm, 1800000, 36000);
}

void test_sport_engine__standing_still_adds_no_jitter(void) {
  SportEngineConfig cfg = s_run_cfg;
  cfg.auto_pause = false;
  prv_start(&cfg);
  prv_run(120, 0, 1500);

  cl_assert_equal_i(s_engine.total.distance_mm, 0);
  cl_assert_equal_i(s_engine.total.timer_s, 120);
}

void test_sport_engine__positions_alone_suppress_jitter(void) {
  SportEngineConfig cfg = s_run_cfg;
  cfg.auto_pause = false;
  prv_start(&cfg);
  prv_run_with(120, 0, 1000, false);

  cl_assert_equal_i(s_engine.total.distance_mm, 0);
}

void test_sport_engine__positions_alone_count_a_slow_walk(void) {
  SportEngineConfig cfg = s_run_cfg;
  cfg.auto_pause = false;
  prv_start(&cfg);
  prv_run_with(100, 800, 0, false);

  // At most the jitter distance is still pending.
  assert_near(s_engine.total.distance_mm, 80000 - 3000, 1000);
}

void test_sport_engine__positions_alone_follow_a_track(void) {
  prv_start(&s_run_cfg);
  prv_run_with(300, 3000, 0, false);

  // The last few metres may still be under the jitter distance.
  assert_near(s_engine.total.distance_mm, 897000 - 1500, 1600);
}

void test_sport_engine__poor_fixes_fall_back_to_steps(void) {
  prv_start(&s_run_cfg);
  for (int i = 0; i < 60; i++) {
    SportSample s = prv_fix(3000, 0);
    s.hdop_x10 = 80;
    s.flags |= SportSampleFlag_StepDistance;
    s.step_distance_mm = 2800;
    sport_engine_tick(&s_engine, &s);
  }
  cl_assert_equal_i(s_engine.source, SportDistanceSource_Steps);
  cl_assert_equal_i(s_engine.total.distance_mm, 60 * 2800);

  // Too few satellites is just as bad.
  SportSample s = prv_fix(3000, 0);
  s.sats = 3;
  sport_engine_tick(&s_engine, &s);
  cl_assert_equal_i(s_engine.source, SportDistanceSource_None);
}

void test_sport_engine__outlier_is_dropped(void) {
  prv_start(&s_run_cfg);
  prv_run(30, 3000, 0);
  const uint32_t before = s_engine.total.distance_mm;

  // One fix 500 m off.
  SportSample s = prv_fix(3000, 0);
  s.lat_e7 += 45000;
  sport_engine_tick(&s_engine, &s);
  cl_assert_equal_i(s_engine.total.distance_mm, before);

  // Only the second of the outlier is lost.
  prv_run(10, 3000, 0);
  cl_assert_equal_i(s_engine.total.distance_mm, before + 30000);
}

void test_sport_engine__lost_fix_does_not_double_count(void) {
  prv_start(&s_run_cfg);
  prv_run(30, 3000, 0);

  // Tunnel: 20 s on steps only, while still moving.
  for (int i = 0; i < 20; i++) {
    SportSample s = {.flags = SportSampleFlag_StepDistance, .step_distance_mm = 3000};
    s_pos_mm += 3000;
    sport_engine_tick(&s_engine, &s);
  }
  prv_run(30, 3000, 0);

  // 80 s at 3 m/s, without counting the tunnel twice.
  assert_near(s_engine.total.distance_mm, 240000, 100);
}

void test_sport_engine__external_speed_wins(void) {
  prv_start(&s_run_cfg);
  for (int i = 0; i < 10; i++) {
    SportSample s = prv_fix(3000, 0);
    s.flags |= SportSampleFlag_ExtSpeed;
    s.ext_speed_mm_s = 5000;
    sport_engine_tick(&s_engine, &s);
  }
  cl_assert_equal_i(s_engine.source, SportDistanceSource_External);
  cl_assert_equal_i(s_engine.total.distance_mm, 50000);
}

// Laps ////////////////////////////////////////////////////////////////////////////////////////////

void test_sport_engine__auto_lap_every_km(void) {
  prv_start(&s_run_cfg);
  const uint32_t events = prv_run(850, 3000, 0);

  cl_assert(events & SportEvent_Lap);
  cl_assert_equal_i(s_engine.num_laps, 2);
  cl_assert_equal_i(s_engine.laps[0].stats.distance_mm, 1000000);
  cl_assert_equal_i(s_engine.laps[1].stats.distance_mm, 1000000);
  cl_assert_equal_i(s_engine.laps[0].number, 1);
  cl_assert_equal_i(s_engine.laps[0].trigger, SportLapTrigger_Auto);
  assert_near(s_engine.laps[0].stats.timer_s, 333, 4);
  // Laps add up to the total.
  cl_assert_equal_i(s_engine.laps[0].stats.distance_mm + s_engine.laps[1].stats.distance_mm +
                        s_engine.lap.stats.distance_mm,
                    s_engine.total.distance_mm);
  cl_assert_equal_i(s_engine.lap.number, 3);
}

void test_sport_engine__auto_lap_in_miles(void) {
  SportEngineConfig cfg = s_run_cfg;
  cfg.auto_lap_mm = SPORT_UNIT_MI_MM;
  prv_start(&cfg);
  prv_run(560, 3000, 0);

  cl_assert_equal_i(s_engine.num_laps, 1);
  cl_assert_equal_i(s_engine.laps[0].stats.distance_mm, SPORT_UNIT_MI_MM);
}

void test_sport_engine__manual_lap_and_stop(void) {
  prv_start(&s_run_cfg);
  cl_assert(!sport_engine_lap(&s_engine));

  prv_run(100, 3000, 0);
  cl_assert(sport_engine_lap(&s_engine));
  cl_assert_equal_i(s_engine.laps[0].trigger, SportLapTrigger_Manual);
  cl_assert_equal_i(s_engine.laps[0].stats.timer_s, 100);
  cl_assert(!sport_engine_lap(&s_engine));

  prv_run(50, 3000, 0);
  sport_engine_stop(&s_engine);
  cl_assert_equal_i(s_engine.state, SportState_Stopped);
  cl_assert_equal_i(s_engine.num_laps, 2);
  cl_assert_equal_i(sport_engine_last_lap(&s_engine)->trigger, SportLapTrigger_Final);
  cl_assert_equal_i(sport_engine_last_lap(&s_engine)->stats.timer_s, 50);

  // Nothing counts once stopped.
  cl_assert_equal_i(prv_run(10, 3000, 0), 0);
  cl_assert_equal_i(s_engine.total.timer_s, 150);
}

void test_sport_engine__laps_past_the_store_are_counted(void) {
  SportEngineConfig cfg = s_run_cfg;
  cfg.auto_lap_mm = 0;
  prv_start(&cfg);
  for (int i = 0; i < SPORT_ENGINE_MAX_LAPS + 5; i++) {
    prv_run(1, 3000, 0);
    cl_assert(sport_engine_lap(&s_engine));
  }
  cl_assert_equal_i(s_engine.num_laps, SPORT_ENGINE_MAX_LAPS + 5);
  cl_assert(sport_engine_last_lap(&s_engine) == &s_engine.laps[SPORT_ENGINE_MAX_LAPS - 1]);
}

// Auto-pause //////////////////////////////////////////////////////////////////////////////////////

void test_sport_engine__auto_pause_when_stopped(void) {
  prv_start(&s_run_cfg);
  prv_run(60, 3000, 0);

  uint32_t events = prv_run(3, 0, 1000);
  cl_assert_equal_i(events, 0);
  cl_assert_equal_i(s_engine.state, SportState_Running);
  events = prv_run(1, 0, 1000);
  cl_assert(events & SportEvent_AutoPause);
  cl_assert_equal_i(s_engine.state, SportState_AutoPaused);
  const uint32_t timer = s_engine.total.timer_s;
  const uint32_t dist = s_engine.total.distance_mm;

  // Standing at a crossing: no time, no distance.
  prv_run(30, 0, 1500);
  cl_assert_equal_i(s_engine.total.timer_s, timer);
  cl_assert_equal_i(s_engine.total.distance_mm, dist);
  cl_assert_equal_i(s_engine.elapsed_s, 94);
  cl_assert_equal_i(sport_engine_current_speed_mm_s(&s_engine), 0);

  events = prv_run(2, 3000, 0);
  cl_assert(events & SportEvent_AutoResume);
  cl_assert_equal_i(s_engine.state, SportState_Running);
  // The second it resumes on counts.
  prv_run(20, 3000, 0);
  cl_assert_equal_i(s_engine.total.timer_s, timer + 21);
}

void test_sport_engine__auto_pause_hysteresis(void) {
  prv_start(&s_run_cfg);
  prv_run(30, 3000, 0);
  prv_run(4, 0, 0);
  cl_assert_equal_i(s_engine.state, SportState_AutoPaused);

  // Between the thresholds: neither resumes nor flaps.
  prv_run(30, 700, 0);
  cl_assert_equal_i(s_engine.state, SportState_AutoPaused);

  // A single fast second is not enough.
  prv_run(1, 3000, 0);
  prv_run(1, 700, 0);
  cl_assert_equal_i(s_engine.state, SportState_AutoPaused);

  prv_run(2, 3000, 0);
  cl_assert_equal_i(s_engine.state, SportState_Running);
  prv_run(30, 700, 0);
  cl_assert_equal_i(s_engine.state, SportState_Running);
}

void test_sport_engine__auto_pause_can_be_off(void) {
  SportEngineConfig cfg = s_run_cfg;
  cfg.auto_pause = false;
  prv_start(&cfg);
  prv_run(60, 0, 0);
  cl_assert_equal_i(s_engine.state, SportState_Running);
  cl_assert_equal_i(s_engine.total.timer_s, 60);
}

void test_sport_engine__manual_pause_drops_what_moves_meanwhile(void) {
  prv_start(&s_run_cfg);
  prv_run(30, 3000, 0);
  sport_engine_pause(&s_engine, true);
  cl_assert_equal_i(s_engine.state, SportState_Paused);

  // Walking to the car: moving, but paused by hand, so no auto-resume.
  prv_run(60, 3000, 0);
  cl_assert_equal_i(s_engine.state, SportState_Paused);
  cl_assert_equal_i(s_engine.total.timer_s, 30);

  sport_engine_pause(&s_engine, false);
  prv_run(10, 3000, 0);
  cl_assert_equal_i(s_engine.total.distance_mm, 40 * 3000);
}

// Elevation ///////////////////////////////////////////////////////////////////////////////////////

static void prv_climb(uint32_t seconds, int32_t *alt_cm, int32_t rate_cm_s, int32_t noise_cm,
                      bool baro) {
  for (uint32_t i = 0; i < seconds; i++) {
    SportSample s = prv_fix(3000, 0);
    *alt_cm += rate_cm_s;
    if (baro) {
      s.flags |= SportSampleFlag_BaroAlt;
      s.baro_alt_cm = *alt_cm + prv_noise(noise_cm);
    } else {
      s.flags |= SportSampleFlag_GnssAlt;
      s.gnss_alt_cm = *alt_cm + prv_noise(noise_cm);
    }
    sport_engine_tick(&s_engine, &s);
  }
}

void test_sport_engine__barometric_ascent(void) {
  int32_t alt = 40000;
  prv_start(&s_run_cfg);

  // Flat, with sensor noise: no ascent.
  prv_climb(120, &alt, 0, 30, true);
  cl_assert_equal_i(s_engine.total.ascent_cm, 0);
  cl_assert_equal_i(s_engine.total.descent_cm, 0);

  // 30 m up, then 30 m down.
  prv_climb(300, &alt, 10, 30, true);
  prv_climb(30, &alt, 0, 30, true);
  assert_near(s_engine.total.ascent_cm, 3000, 150);
  prv_climb(300, &alt, -10, 30, true);
  prv_climb(30, &alt, 0, 30, true);
  assert_near(s_engine.total.descent_cm, 3000, 150);

  int32_t elev;
  cl_assert(sport_engine_elevation_cm(&s_engine, &elev));
  assert_near(elev, 40000, 60);
}

void test_sport_engine__gnss_altitude_fallback_ignores_its_noise(void) {
  int32_t alt = 40000;
  prv_start(&s_run_cfg);

  prv_climb(300, &alt, 0, 200, false);
  cl_assert_equal_i(s_engine.total.ascent_cm, 0);

  prv_climb(200, &alt, 10, 200, false);
  prv_climb(60, &alt, 0, 200, false);
  // Up to one threshold is held back.
  assert_near(s_engine.total.ascent_cm, 2000, 450);
  cl_assert(!s_engine.alt_baro);
}

void test_sport_engine__barometer_preferred_over_gnss(void) {
  prv_start(&s_run_cfg);
  SportSample s = prv_fix(3000, 0);
  s.flags |= SportSampleFlag_GnssAlt | SportSampleFlag_BaroAlt;
  s.gnss_alt_cm = 50000;
  s.baro_alt_cm = 40000;
  sport_engine_tick(&s_engine, &s);

  int32_t elev;
  cl_assert(sport_engine_elevation_cm(&s_engine, &elev));
  cl_assert_equal_i(elev, 40000);
  cl_assert(s_engine.alt_baro);
}

void test_sport_engine__no_ascent_while_paused(void) {
  int32_t alt = 40000;
  prv_start(&s_run_cfg);
  prv_climb(10, &alt, 0, 0, true);
  sport_engine_pause(&s_engine, true);
  prv_climb(100, &alt, 10, 0, true);
  sport_engine_pause(&s_engine, false);
  prv_climb(10, &alt, 0, 0, true);
  cl_assert_equal_i(s_engine.total.ascent_cm, 0);
}

void test_sport_engine__grade(void) {
  int32_t alt = 0;
  int16_t grade;
  prv_start(&s_run_cfg);

  cl_assert(!sport_engine_grade_x10(&s_engine, &grade));
  // 3 m/s, climbing 15 cm/s: 5%.
  prv_climb(60, &alt, 15, 0, true);
  cl_assert(sport_engine_grade_x10(&s_engine, &grade));
  assert_near(grade, 50, 3);
}

// Statistics //////////////////////////////////////////////////////////////////////////////////////

void test_sport_engine__heart_rate_cadence_power(void) {
  prv_start(&s_run_cfg);
  for (int i = 0; i < 100; i++) {
    SportSample s = prv_fix(3000, 0);
    s.flags |= SportSampleFlag_Hr | SportSampleFlag_Cadence | SportSampleFlag_Power;
    s.hr_bpm = (uint8_t)(140 + i % 21);
    s.cadence = 170;
    s.power_w = (uint16_t)(200 + i);
    sport_engine_tick(&s_engine, &s);
  }
  cl_assert_equal_i(sport_stats_avg_hr(&s_engine.total), 150);
  cl_assert_equal_i(s_engine.total.hr_max, 160);
  cl_assert_equal_i(sport_stats_avg_cadence(&s_engine.total), 170);
  cl_assert_equal_i(sport_stats_avg_power(&s_engine.total), 250);
  cl_assert_equal_i(s_engine.total.power_max, 299);
  cl_assert_equal_i(sport_stats_avg_hr(&s_engine.lap.stats), 150);
  assert_near(s_engine.total.max_speed_mm_s, 3000, 100);
}

// Fields //////////////////////////////////////////////////////////////////////////////////////////

void test_sport_engine__format(void) {
  char buf[16];

  sport_format_duration(59, buf, sizeof(buf));
  cl_assert_equal_s(buf, "0:59");
  sport_format_duration(3725, buf, sizeof(buf));
  cl_assert_equal_s(buf, "1:02:05");
  sport_format_pace(331, buf, sizeof(buf));
  cl_assert_equal_s(buf, "5:31");
  sport_format_pace(0, buf, sizeof(buf));
  cl_assert_equal_s(buf, "--");
  sport_format_pace(3600, buf, sizeof(buf));
  cl_assert_equal_s(buf, "--");
  sport_format_distance(1999999, SPORT_UNIT_KM_MM, buf, sizeof(buf));
  cl_assert_equal_s(buf, "1.99");
  sport_format_distance(SPORT_UNIT_MI_MM * 3, SPORT_UNIT_MI_MM, buf, sizeof(buf));
  cl_assert_equal_s(buf, "3.00");
  sport_format_speed(8333, SPORT_UNIT_KM_MM, buf, sizeof(buf));
  cl_assert_equal_s(buf, "30.0");
  sport_format_speed(8941, SPORT_UNIT_MI_MM, buf, sizeof(buf));
  cl_assert_equal_s(buf, "20.0");
  sport_format_elevation(40849, SPORT_UNIT_KM_MM, buf, sizeof(buf));
  cl_assert_equal_s(buf, "408");
  sport_format_elevation(40849, SPORT_UNIT_MI_MM, buf, sizeof(buf));
  cl_assert_equal_s(buf, "1340");
  sport_format_elevation(-1250, SPORT_UNIT_KM_MM, buf, sizeof(buf));
  cl_assert_equal_s(buf, "-13");
}

void test_sport_engine__metric_fields(void) {
  char buf[16];
  prv_start(&s_run_cfg);
  prv_run(600, 3000, 0);

  SportFieldContext ctx =
      {.engine = &s_engine, .unit_mm = SPORT_UNIT_KM_MM, .hour = 7, .minute = 5};
  sport_metric_format(SportMetric_Timer, &ctx, buf, sizeof(buf));
  cl_assert_equal_s(buf, "10:00");
  sport_metric_format(SportMetric_AvgPace, &ctx, buf, sizeof(buf));
  cl_assert_equal_s(buf, "5:33");
  sport_metric_format(SportMetric_Laps, &ctx, buf, sizeof(buf));
  cl_assert_equal_s(buf, "2");
  sport_metric_format(SportMetric_HeartRate, &ctx, buf, sizeof(buf));
  cl_assert_equal_s(buf, "--");
  sport_metric_format(SportMetric_Elevation, &ctx, buf, sizeof(buf));
  cl_assert_equal_s(buf, "--");
  sport_metric_format(SportMetric_Clock, &ctx, buf, sizeof(buf));
  cl_assert_equal_s(buf, "7:05");
  ctx.has_hr = true;
  ctx.hr_bpm = 151;
  sport_metric_format(SportMetric_HeartRate, &ctx, buf, sizeof(buf));
  cl_assert_equal_s(buf, "151");

  cl_assert_equal_s(sport_metric_label(SportMetric_Pace), "PACE");
  cl_assert_equal_s(sport_metric_unit(SportMetric_Pace, SPORT_UNIT_MI_MM), "/mi");
  cl_assert_equal_s(sport_metric_unit(SportMetric_Speed, SPORT_UNIT_KM_MM), "km/h");
}
