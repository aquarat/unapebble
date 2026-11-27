/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "engine.h"

#include <stdbool.h>
#include <stdint.h>

//! Sensor intake: turns the watch's sensors, and any external ones, into one SportSample a
//! second for the engine.

typedef enum SportGnssStatus {
  SportGnssStatus_None,
  SportGnssStatus_Searching,
  SportGnssStatus_Fix,
} SportGnssStatus;

//! Values external sensors report, such as BLE heart rate straps (0x180D), speed and cadence
//! sensors (CSC, 0x1816) and power meters (CPS, 0x1818).
typedef enum SportExternal {
  //! Beats per minute.
  SportExternal_Hr,
  //! Millimetres per second.
  SportExternal_Speed,
  //! Revolutions or steps per minute.
  SportExternal_Cadence,
  //! Watts.
  SportExternal_Power,
  SportExternalCount,
} SportExternal;

typedef struct SportSensorsStatus {
  SportGnssStatus gnss;
  uint8_t sats;
  bool baro;
} SportSensorsStatus;

//! Start the sensors, SportSensor bits.
void sport_sensors_init(uint32_t sensors);

void sport_sensors_deinit(void);

//! Connect to the workout service once the activity starts, for heart rate and steps.
void sport_sensors_set_workout(bool active);

//! Collect the last second. Call once a second.
void sport_sensors_poll(SportSample *sample);

SportSensorsStatus sport_sensors_get_status(void);

//! Report a value from an external sensor, on the app task. Values older than a few seconds are
//! dropped; external heart rate and cadence take over from the watch's.
void sport_sensors_external_report(SportExternal which, uint32_t value);

//! Altitude for a pressure, in the standard atmosphere, in centimetres.
int32_t sport_pressure_to_altitude_cm(uint32_t pa);
