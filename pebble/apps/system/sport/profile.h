/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "engine.h"
#include "fields.h"

#include <stdbool.h>
#include <stdint.h>

#include <pbl/services/activity/activity.h>

//! A sport: what makes Run differ from Bike. Everything else is the shared core.

//! Sensors a sport uses.
typedef enum SportSensor {
  //! Position, speed and altitude from the location service.
  SportSensor_Gnss = 1 << 0,
  //! Heart rate, from the HRM manager through the workout service.
  SportSensor_Hr = 1 << 1,
  //! Barometric elevation.
  SportSensor_Baro = 1 << 2,
  //! Steps: cadence, and distance without a fix.
  SportSensor_Steps = 1 << 3,
  //! External sensors reported with sport_sensors_external_report().
  SportSensor_External = 1 << 4,
} SportSensor;

//! Sport recorded in the activity files; never renumber.
typedef enum SportType {
  SportType_Run = 1,
  SportType_Bike = 2,
} SportType;

typedef struct SportProfile {
  const char *name;
  SportType type;
  //! Session saved in Health.
  ActivitySessionType session_type;
  //! SportSensor bits.
  uint32_t sensors;
  //! Auto-lap every this many km or miles, the user's unit; 0 for none.
  uint8_t auto_lap_units;
  //! Filtering and auto-pause; auto_lap_mm is set from auto_lap_units.
  SportEngineConfig engine;
  const SportScreen *screens;
  uint8_t num_screens;
  //! Show paces, otherwise speeds, in lap alerts and the summary.
  bool pace;
  //! Accent colour, as GColor ARGB8.
  uint8_t accent_argb8;
} SportProfile;

//! Run the activity app for a sport: the app's main function.
void sport_app_main(const SportProfile *profile);
