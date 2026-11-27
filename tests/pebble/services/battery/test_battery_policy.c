/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "battery_policy.h"

#include <clar.h>

static struct battery_policy_thermal s_t;

static bool prv_at(int32_t temp_mc) {
  return battery_policy_thermal_update(&s_t, true, temp_mc);
}

void test_battery_policy__initialize(void) {
  s_t = (struct battery_policy_thermal)BATTERY_POLICY_THERMAL_INIT;
}

void test_battery_policy__off_until_read(void) {
  cl_assert(s_t.inhibited);
  cl_assert(battery_policy_thermal_update(&s_t, false, 25000));
  cl_assert(!prv_at(25000));
}

void test_battery_policy__starts_inside_hysteresis_only(void) {
  cl_assert(prv_at(2000));
  cl_assert(prv_at(43000));
  cl_assert(!prv_at(3000));
}

void test_battery_policy__hot(void) {
  cl_assert(!prv_at(25000));
  cl_assert(!prv_at(44999));
  cl_assert(prv_at(45000));
  cl_assert(prv_at(43000));
  cl_assert(!prv_at(42000));
}

void test_battery_policy__cold(void) {
  cl_assert(!prv_at(25000));
  cl_assert(!prv_at(1));
  cl_assert(prv_at(0));
  cl_assert(prv_at(-10000));
  cl_assert(prv_at(2999));
  cl_assert(!prv_at(3000));
}

void test_battery_policy__unknown_temperature(void) {
  cl_assert(!prv_at(25000));
  // A failed read or two keeps the decision
  cl_assert(!battery_policy_thermal_update(&s_t, false, 0));
  cl_assert(!battery_policy_thermal_update(&s_t, false, 0));
  cl_assert(!prv_at(25000));
  cl_assert(!battery_policy_thermal_update(&s_t, false, 0));
  cl_assert(!battery_policy_thermal_update(&s_t, false, 0));
  // The third in a row stops charging, until a reading allows it again
  cl_assert(battery_policy_thermal_update(&s_t, false, 0));
  cl_assert(battery_policy_thermal_update(&s_t, false, 0));
  cl_assert(!prv_at(25000));
}

void test_battery_policy__percent(void) {
  struct pbl_gauge_reading r = {.soc_cpct = 5099, .avg_voltage_mv = 3800};

  cl_assert_equal_i(battery_policy_percent(&r, false), 50);
  // Full only at 100%
  r.soc_cpct = 9999;
  cl_assert_equal_i(battery_policy_percent(&r, true), 99);
  r.soc_cpct = 10000;
  cl_assert_equal_i(battery_policy_percent(&r, true), 100);
}

void test_battery_policy__empty(void) {
  struct pbl_gauge_reading r = {.soc_cpct = 99, .avg_voltage_mv = 3600};

  // Below 1%, or at the voltage floor with the gauge's SOC stalled, unplugged: empty
  cl_assert_equal_i(battery_policy_percent(&r, false), 0);
  r.soc_cpct = 1500;
  r.avg_voltage_mv = 3350;
  cl_assert_equal_i(battery_policy_percent(&r, false), 0);
  r.avg_voltage_mv = 3351;
  cl_assert_equal_i(battery_policy_percent(&r, false), 15);
  // Plugged in, it is not shut down
  r.avg_voltage_mv = 3300;
  cl_assert_equal_i(battery_policy_percent(&r, true), 15);
}
