/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "profile.h"
#include "sport.h"

#include <pbl/i18n/i18n.h>
#include <pbl/util/size.h>

#include <applib/graphics/gtypes.h>
#include <process_management/pebble_process_md.h>
#include <resource/resource_ids.auto.h>

static const SportScreen s_screens[] = {
  {3, {SportMetric_Timer, SportMetric_Distance, SportMetric_Pace}},
  {3, {SportMetric_AvgPace, SportMetric_LapPace, SportMetric_LapTime}},
  {3, {SportMetric_Cadence, SportMetric_Elevation, SportMetric_Ascent}},
  {3, {SportMetric_HeartRate, SportMetric_Elapsed, SportMetric_Clock}},
};

static const SportProfile s_walk = {
  .name = "Walk",
  .type = SportType_Walk,
  .session_type = ActivitySessionType_Walk,
  .sensors = SportSensor_Gnss | SportSensor_Hr | SportSensor_Baro | SportSensor_Steps |
             SportSensor_External,
  .auto_lap_units = 1,
  .engine =
      {
        .auto_pause = true,
        .pause_below_mm_s = 300,
        .resume_above_mm_s = 600,
        .pause_after_s = 6,
        .resume_after_s = 3,
        .max_speed_mm_s = 5000,
        .max_hdop_x10 = 50,
        .min_sats = 4,
        .jitter_mm = 3000,
        .climb_threshold_baro_cm = 100,
        .climb_threshold_gnss_cm = 400,
      },
  .screens = s_screens,
  .num_screens = PBL_ARRAY_SIZE(s_screens),
  .pace = true,
  .accent_argb8 = GColorIslamicGreenARGB8,
};

static void prv_main(void) {
  sport_app_main(&s_walk);
}

const PebbleProcessMd *sport_walk_get_app_info(void) {
  static const PebbleProcessMdSystem s_info = {
    .common =
        {
          .main_func = prv_main,
          .uuid =
              {0x11, 0xf0, 0x49, 0xc8, 0xdd, 0xfc, 0x49, 0xca, 0xa1, 0x82, 0x72, 0x72, 0xb8, 0xc5,
               0xdc, 0x67},
        },
    .name = pbl_i18n_noop("Walk"),
    .icon_resource_id = RESOURCE_ID_WORKOUT_APP_WALK_TINY,
  };

  return (const PebbleProcessMd *)&s_info;
}
