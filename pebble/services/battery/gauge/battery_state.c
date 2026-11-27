/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The battery state from a fuel gauge chip, which estimates the state of charge itself: the
// percentage is the gauge's, external power and the charge status the charger's. Charging is
// only allowed while the gauge reads a temperature the battery may be charged at.

#include "battery_policy.h"

#include <inttypes.h>

#include <pbl/analytics/analytics.h>
#include <pbl/drivers/charger.h>
#include <pbl/drivers/gauge.h>
#include <pbl/kernel/syscall.h>
#include <pbl/kernel/time.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/services/battery/battery_charger.h>
#include <pbl/services/battery/battery_state.h>
#include <pbl/services/battery/battery_test.h>
#include <pbl/util/ratio.h>

#include <kernel/events.h>

PBL_LOG_MODULE_DECLARE(service_battery, CONFIG_SERVICE_BATTERY_LOG_LEVEL);

// The temperature is checked more often while the battery may be charging
#define SAMPLE_PLUGGED_MS     (30 * 1000)
#define SAMPLE_UNPLUGGED_MS   (60 * 1000)
#define RECONNECTION_DELAY_MS 1000
// Shown until the gauge first answers: low, so the watch asks to be charged, never full, and
// above the low power threshold, so it does not shut down on a gauge it cannot read
#define UNKNOWN_PERCENT 5U

static void prv_work(struct pbl_work *work);
static PBL_DELAYED_WORK_DEFINE(s_work, prv_work);

static PreciseBatteryChargeState s_state;
static struct pbl_gauge_reading s_reading;
static bool s_reading_valid;
static struct battery_policy_thermal s_thermal = BATTERY_POLICY_THERMAL_INIT;
static volatile bool s_force;

static void prv_put_event(void) {
  PebbleEvent e = {
    .type = PEBBLE_BATTERY_STATE_CHANGE_EVENT,
    .battery_state = {
      .new_state = s_state,
    },
  };
  event_put(&e);
}

static bool prv_update(bool publish) {
  const bool was_inhibited = s_thermal.inhibited;
  struct pbl_gauge_reading r = {0};
  PreciseBatteryChargeState state;
  bool inhibited;
  bool valid;
  int ret;

  ret = pbl_gauge_read(GAUGE, &r);
  valid = (ret == 0);
  if (valid) {
    s_reading = r;
    s_reading_valid = true;
  } else {
    PBL_LOG_ERR("Fuel gauge read failed (%d)", ret);
  }

  inhibited = battery_policy_thermal_update(&s_thermal, valid, r.temp_mc);
  if (inhibited != was_inhibited) {
    PBL_LOG_WRN("Charging %s (%s, %" PRId32 " mC)", inhibited ? "inhibited" : "allowed",
                valid ? "read" : "unknown", valid ? r.temp_mc : 0);
  }
  // Every time: it also restores the charger's enable after a reset of the PMIC
  battery_charger_set_inhibited(inhibited);

  state.is_plugged = battery_is_usb_connected();
  state.is_charging = state.is_plugged && battery_charge_controller_thinks_we_are_charging();
  state.pct =
      s_reading_valid ? battery_policy_percent(&s_reading, state.is_plugged) : UNKNOWN_PERCENT;
  state.charge_percent = pbl_ratio32_from_percent(state.pct);

  if (state.is_charging != s_state.is_charging) {
    if (state.is_charging) {
      PBL_ANALYTICS_TIMER_START(battery_charge_time_ms);
    } else {
      PBL_ANALYTICS_TIMER_STOP(battery_charge_time_ms);
    }
  }
  if (state.is_plugged != s_state.is_plugged) {
    if (state.is_plugged) {
      PBL_ANALYTICS_TIMER_STOP(battery_discharge_duration_ms);
    } else {
      PBL_ANALYTICS_TIMER_START(battery_discharge_duration_ms);
    }
  }

  if ((state.pct != s_state.pct) || (state.is_charging != s_state.is_charging) ||
      (state.is_plugged != s_state.is_plugged)) {
    PBL_LOG_INFO("Percent: %u, V: %" PRId32 " mV, I: %" PRId32 " uA, T: %" PRId32
                 " mC, charging: %d, plugged: %d",
                 state.pct, s_reading.voltage_mv, s_reading.current_ua, s_reading.temp_mc,
                 state.is_charging, state.is_plugged);
    publish = true;
  }

  s_state = state;
  if (publish) {
    prv_put_event();
  }

  return valid;
}

static void prv_work(struct pbl_work *work) {
  const bool force = s_force;

  s_force = false;
  const bool valid = prv_update(force);
#ifdef CONFIG_BATTERY_TEST
  // On the polls, so that the test does not wake the watch more often
  battery_test_sample(valid ? &s_reading : nullptr, s_state.is_plugged, s_state.pct);
#else
  (void)valid;
#endif

  pbl_delayed_work_reschedule(
      &pbl_sys_workq, &s_work,
      PBL_MSEC(s_state.is_plugged ? SAMPLE_PLUGGED_MS : SAMPLE_UNPLUGGED_MS));
}

static void prv_schedule(uint32_t delay_ms) {
  s_force = true;
  pbl_delayed_work_reschedule(&pbl_sys_workq, &s_work, PBL_MSEC(delay_ms));
}

void battery_state_force_update(void) {
  prv_schedule(0);
}

void battery_state_init(void) {
  // A first state for whoever asks before the first update; the forced one publishes it
  (void)prv_update(false);

  if (!s_state.is_plugged) {
    PBL_ANALYTICS_TIMER_START(battery_discharge_duration_ms);
  }

  battery_state_force_update();
}

void battery_state_handle_connection_event(bool is_connected) {
  prv_schedule(RECONNECTION_DELAY_MS);
}

PBL_SYSCALL(BatteryChargeState, sys_battery_get_charge_state) {
  return battery_get_charge_state();
}

BatteryChargeState battery_get_charge_state(void) {
  return (BatteryChargeState){
    .charge_percent = s_state.pct,
    .is_charging = s_state.is_charging,
    .is_plugged = s_state.is_plugged,
  };
}

// For unit tests
struct pbl_delayed_work *battery_state_get_periodic_timer(void) {
  return &s_work;
}

uint16_t battery_state_get_voltage(void) {
  return (uint16_t)s_reading.voltage_mv;
}

int32_t battery_state_get_temperature(void) {
  return s_reading.temp_mc;
}

// Runs on another thread than the updates
static void prv_collect_battery(void) {
  PBL_ANALYTICS_SET_UNSIGNED(battery_voltage, s_reading.voltage_mv);
  PBL_ANALYTICS_SET_SIGNED(battery_temp_c, s_reading.temp_mc);
  PBL_ANALYTICS_SET_UNSIGNED(battery_soc_pct, s_reading.soc_cpct);
}
PBL_ANALYTICS_COLLECTOR_DEFINE(prv_collect_battery);

#ifdef CONFIG_SHELL
#include <errno.h>
#include <string.h>

#include <pbl/shell/shell.h>

static int prv_cmd_status(const struct pbl_shell *sh, size_t argc, char **argv) {
  pbl_shell_print(sh, "%" PRId32 " mV, %" PRId32 " uA, %" PRId32 " mC", s_reading.voltage_mv,
                  s_reading.current_ua, s_reading.temp_mc);
  pbl_shell_print(sh, "soc: %u%% (gauge %u.%02u%%%s)", s_state.pct, s_reading.soc_cpct / 100U,
                  s_reading.soc_cpct % 100U, s_reading_valid ? "" : ", never read");
  pbl_shell_print(sh, "plugged: %s", s_state.is_plugged ? "YES" : "NO");
  pbl_shell_print(sh, "charging: %s", s_state.is_charging ? "YES" : "NO");
  pbl_shell_print(sh, "charge inhibited: %s", s_thermal.inhibited ? "YES" : "NO");
  return 0;
}

static int prv_cmd_chargeopt(const struct pbl_shell *sh, size_t argc, char **argv) {
  bool is_charging;

  if (!strcmp("disable", argv[1])) {
    is_charging = false;
  } else if (!strcmp("enable", argv[1])) {
    is_charging = true;
  } else {
    pbl_shell_error(sh, "invalid option '%s'", argv[1]);
    return -EINVAL;
  }

  battery_force_charge_enable(is_charging);
  battery_state_force_update();
  return 0;
}

PBL_SHELL_SUBCMD_ADD(sub_battery, status, nullptr, "Print the battery state", prv_cmd_status, 0, 0);
PBL_SHELL_SUBCMD_ADD(sub_battery, chargeopt, nullptr, "Force charging <enable|disable>",
                     prv_cmd_chargeopt, 2, 0);
#endif
