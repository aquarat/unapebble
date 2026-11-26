/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// What the battery state makes of a fuel gauge reading: whether the battery may be charged at
// its temperature, and the percentage shown. Apart from the service so the unit tests can check
// it.

#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/gauge.h>

// The usual charge window of a lithium cell, 0 to 45 degrees C, with 3 degrees of hysteresis
#define BATTERY_POLICY_CHARGE_MIN_MC 0
#define BATTERY_POLICY_CHARGE_MAX_MC 45000
#define BATTERY_POLICY_HYSTERESIS_MC 3000
// Failed readings in a row after which the temperature counts as unknown
#define BATTERY_POLICY_MAX_FAILURES 3U
// Unplugged, the battery is empty below 1% or at this averaged voltage, as the stock firmware
// has it: the gauge's SOC may stall just above 0, and a load spike must not count
#define BATTERY_POLICY_EMPTY_MV 3350

struct battery_policy_thermal {
  //! Charging is kept off
  bool inhibited;
  //! Failed readings in a row
  uint8_t failures;
};

//! The initial state: charging off until a reading allows it
#define BATTERY_POLICY_THERMAL_INIT {.inhibited = true, .failures = 0U}

//! Update the charging decision with a reading, or a failure to get one. Charging stops at or
//! beyond the limits, or with the temperature unknown, and resumes once it is back inside them
//! by the hysteresis.
static inline bool battery_policy_thermal_update(struct battery_policy_thermal *t, bool valid,
                                                 int32_t temp_mc) {
  if (!valid) {
    if (t->failures < BATTERY_POLICY_MAX_FAILURES) {
      t->failures++;
    }
    if (t->failures >= BATTERY_POLICY_MAX_FAILURES) {
      t->inhibited = true;
    }
    return t->inhibited;
  }

  t->failures = 0U;
  if (t->inhibited) {
    if ((temp_mc >= BATTERY_POLICY_CHARGE_MIN_MC + BATTERY_POLICY_HYSTERESIS_MC) &&
        (temp_mc <= BATTERY_POLICY_CHARGE_MAX_MC - BATTERY_POLICY_HYSTERESIS_MC)) {
      t->inhibited = false;
    }
  } else if ((temp_mc <= BATTERY_POLICY_CHARGE_MIN_MC) ||
             (temp_mc >= BATTERY_POLICY_CHARGE_MAX_MC)) {
    t->inhibited = true;
  }

  return t->inhibited;
}

//! The percentage shown, rounded down so the battery reads full only once the gauge has it at
//! 100%; 0 for an empty battery, which shuts the watch down unless it is plugged in.
static inline uint8_t battery_policy_percent(const struct pbl_gauge_reading *r, bool plugged) {
  if (!plugged && ((r->soc_cpct < 100U) || (r->avg_voltage_mv <= BATTERY_POLICY_EMPTY_MV))) {
    return 0U;
  }

  return (uint8_t)(r->soc_cpct / 100U);
}
