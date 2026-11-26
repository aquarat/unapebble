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
  {3, {SportMetric_Timer, SportMetric_Distance, SportMetric_Speed}},
  {3, {SportMetric_AvgSpeed, SportMetric_MaxSpeed, SportMetric_LapSpeed}},
  {3, {SportMetric_HeartRate, SportMetric_HrZone, SportMetric_Cadence}},
  {3, {SportMetric_Elevation, SportMetric_Ascent, SportMetric_Grade}},
  {3, {SportMetric_Clock, SportMetric_Power, SportMetric_Laps}},
};

// Wrist steps are no cadence on a bike: cadence and power come from external sensors only.
static const SportProfile s_bike = {
  .name = "Bike",
  .type = SportType_Bike,
  .session_type = ActivitySessionType_Open,
  .sensors = SportSensor_Gnss | SportSensor_Hr | SportSensor_Baro | SportSensor_External,
  .auto_lap_units = 5,
  .engine =
      {
        .auto_pause = true,
        .pause_below_mm_s = 1500,
        .resume_above_mm_s = 2500,
        .pause_after_s = 3,
        .resume_after_s = 2,
        .max_speed_mm_s = 30000,
        .max_hdop_x10 = 50,
        .min_sats = 4,
        .jitter_mm = 4000,
        .climb_threshold_baro_cm = 100,
        .climb_threshold_gnss_cm = 400,
      },
  .screens = s_screens,
  .num_screens = PBL_ARRAY_SIZE(s_screens),
  .pace = false,
  .accent_argb8 = GColorVividCeruleanARGB8,
};

static void prv_main(void) {
  sport_app_main(&s_bike);
}

const PebbleProcessMd *sport_bike_get_app_info(void) {
  static const PebbleProcessMdSystem s_info = {
    .common =
        {
          .main_func = prv_main,
          .uuid =
              {0xbe, 0x2e, 0x03, 0x5a, 0x94, 0x8b, 0x44, 0x99, 0x83, 0x26, 0x5e, 0xc3, 0x02, 0x0d,
               0x44, 0xaa},
        },
    .name = pbl_i18n_noop("Bike"),
    .icon_resource_id = RESOURCE_ID_GENERIC_SPORTS_TINY,
  };

  return (const PebbleProcessMd *)&s_info;
}
