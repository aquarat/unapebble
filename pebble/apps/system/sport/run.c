/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
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
  {3, {SportMetric_HeartRate, SportMetric_HrZone, SportMetric_Cadence}},
  {3, {SportMetric_Elevation, SportMetric_Ascent, SportMetric_Clock}},
};

static const SportProfile s_run = {
  .name = "Run",
  .type = SportType_Run,
  .session_type = ActivitySessionType_Run,
  .sensors = SportSensor_Gnss | SportSensor_Hr | SportSensor_Baro | SportSensor_Steps |
             SportSensor_External,
  .auto_lap_units = 1,
  .engine =
      {
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
      },
  .screens = s_screens,
  .num_screens = PBL_ARRAY_SIZE(s_screens),
  .pace = true,
  .accent_argb8 = GColorChromeYellowARGB8,
};

static void prv_main(void) {
  sport_app_main(&s_run);
}

const PebbleProcessMd *sport_run_get_app_info(void) {
  static const PebbleProcessMdSystem s_info = {
    .common =
        {
          .main_func = prv_main,
          .uuid =
              {0x3b, 0x22, 0x27, 0x44, 0xd8, 0xb5, 0x47, 0xd4, 0xa9, 0x1c, 0xaf, 0x69, 0xd6, 0x5a,
               0x92, 0xef},
        },
    .name = pbl_i18n_noop("Run"),
    .icon_resource_id = RESOURCE_ID_RUN_TINY,
  };

  return (const PebbleProcessMd *)&s_info;
}
