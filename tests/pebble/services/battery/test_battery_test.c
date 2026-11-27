/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "core.h"

#include <clar.h>

#define MAGIC 0x42540064U

static struct bt_state s_st;
static struct bt_run s_run;
static struct bt_record s_rec;
static uint32_t s_time;
static uint16_t s_counter;
static uint8_t s_resets;

// A sample a minute after the last, the counter moved by counts (positive while charging)
static unsigned int prv_step(uint8_t flags, int counts, int32_t avg_ua, int32_t temp_mc) {
  const struct bt_sample s = {
    .time = s_time,
    .flags = flags,
    .r = {
      .soc_cpct = 5000U,
      .voltage_mv = 3800,
      .avg_voltage_mv = 3800,
      .current_ua = avg_ua,
      .temp_mc = temp_mc,
      .avg_current_ua = avg_ua,
      .full_uah = 190000U,
      .full_nominal_uah = 195000U,
      .design_uah = 200000U,
      .cycles_pct = 150U,
      .counter = (uint16_t)(s_counter + counts),
      .counter_uah = 500U,
      .resets = s_resets,
    },
  };

  s_counter = s.r.counter;
  s_time += 60U;

  return bt_step(&s_st, &s_run, &s, &s_rec);
}

static void prv_to_discharge(void) {
  bt_start(&s_st, &s_run, MAGIC, false);
  cl_assert_equal_i(prv_step(BT_S_PLUGGED, 0, -50000, 25000), BT_STEP_LOG);
  cl_assert_equal_i(s_st.phase, BT_CHARGE);
  cl_assert_equal_i(prv_step(BT_S_PLUGGED | BT_S_FULL, 2, -10000, 25000),
                    BT_STEP_LOG | BT_STEP_SAVE);
  cl_assert_equal_i(s_st.phase, BT_FULL);
  cl_assert_equal_i(s_st.full[0].full_uah, 190000);
  cl_assert_equal_i(prv_step(0U, 0, 30000, 25000), BT_STEP_LOG | BT_STEP_SAVE);
  cl_assert_equal_i(s_st.phase, BT_DISCHARGE);
  cl_assert_equal_i(s_st.dis.start, 1000U + 120U);
}

void test_battery_test__initialize(void) {
  s_time = 1000U;
  s_counter = 0x0010U;
  s_resets = 0U;
}

void test_battery_test__counter_wraps(void) {
  // 0.5 mAh a count, positive out of the battery
  cl_assert_equal_i(bt_counter_out_uah(1000U, 990U, 500U), 5000);
  cl_assert_equal_i(bt_counter_out_uah(0x0005U, 0xFFF0U, 500U), 21 * 500);
  cl_assert_equal_i(bt_counter_out_uah(0xFFF0U, 0x0010U, 500U), -32 * 500);
  cl_assert_equal_i(bt_counter_out_uah(0x8000U, 0x7FFFU, 500U), 500);
}

void test_battery_test__integral_and_average(void) {
  // 10 mA for a minute: 600 mAs
  cl_assert_equal_i(bt_integral_uas(10000, 10000, 60U), 600000);
  cl_assert_equal_i(bt_integral_uas(10000, 20000, 60U), 900000);
  cl_assert_equal_i(bt_integral_uas(-60000, -60000, 60U), -3600000);
  // 100 mAh in 10 h: 10 mA
  cl_assert_equal_i(bt_avg_ua(100000, 0U, 36000U), 10000);
  cl_assert_equal_i(bt_avg_ua(100000, 5U, 5U), 0);
}

void test_battery_test__idle_and_done_do_nothing(void) {
  s_st = (struct bt_state){0};
  cl_assert_equal_i(prv_step(BT_S_PLUGGED, 0, 0, 25000), 0);
  s_st.phase = BT_DONE;
  cl_assert_equal_i(prv_step(BT_S_PLUGGED, 0, 0, 25000), 0);
}

void test_battery_test__counts_from_full(void) {
  bt_start(&s_st, &s_run, MAGIC, true);
  cl_assert(s_st.flags & BT_F_HEAVY);
  prv_step(BT_S_PLUGGED, 0, -50000, 25000);
  prv_step(BT_S_PLUGGED | BT_S_FULL, 5, -10000, 25000);
  cl_assert_equal_i(s_run.out_uah, 0);
  cl_assert_equal_i(s_run.sw_uas, 0);
  // The charger starts again before the unplug: back to charging, full once more
  prv_step(BT_S_PLUGGED, 1, -20000, 25000);
  cl_assert_equal_i(s_st.phase, BT_CHARGE);
  prv_step(BT_S_PLUGGED | BT_S_FULL, 1, -10000, 25000);
  cl_assert_equal_i(s_st.phase, BT_FULL);
  cl_assert_equal_i(s_run.out_uah, 0);
}

// The whole test, with a power-off and a reboot between the discharge and the recharge
void test_battery_test__full_cycle_across_power_off(void) {
  struct bt_record last;

  // Discharge at 30 mA, a count a minute, through the counter's wrap; heavy for a while
  prv_to_discharge();
  for (int i = 0; i < 100; i++) {
    uint8_t flags = (i >= 40 && i < 60) ? BT_S_HEAVY : 0U;

    if (i == 99) {
      flags |= BT_S_EMPTY;
    }
    cl_assert_equal_i(prv_step(flags, -1, 30000, 20000 + (i * 100)), BT_STEP_LOG);
  }
  cl_assert_equal_i(s_st.phase, BT_DISCHARGE);
  cl_assert_equal_i(s_rec.out_uah, 50000);
  cl_assert_equal_i(s_rec.sw_uah, 50000);
  cl_assert_equal_i(s_rec.loads, BT_LOAD_NORMAL | BT_LOAD_HEAVY);
  cl_assert_equal_i(s_rec.tmin_c, 20);
  cl_assert_equal_i(s_rec.tmax_c, 29);

  // Off for an hour, losing 2 counts; then plugged in and powered on: a new boot
  last = s_rec;
  s_run = (struct bt_run){0};
  bt_run_restore(&s_run, &last);
  s_time += 3600U;
  cl_assert_equal_i(prv_step(BT_S_PLUGGED, -2, -60000, 24000), BT_STEP_LOG | BT_STEP_SAVE);
  cl_assert_equal_i(s_st.phase, BT_RECHARGE);
  cl_assert_equal_i(s_st.dis.uah, 50000);
  cl_assert_equal_i(s_st.dis.sw_uah, 50000);
  cl_assert_equal_i(s_st.dis.end, last.time);
  cl_assert_equal_i(s_st.dis.tmin_c, 20);
  cl_assert_equal_i(s_st.dis.tmax_c, 29);
  cl_assert(s_st.flags & BT_F_COMPLETE);
  cl_assert(!(s_st.flags & BT_F_LOST));
  cl_assert_equal_i(s_st.loads, BT_LOAD_NORMAL | BT_LOAD_HEAVY);
  // The gap is bridged by the counter
  cl_assert_equal_i(s_st.gap_uah, 1000);

  // Charge at 60 mA, two counts a minute, for 30 minutes
  for (int i = 0; i < 30; i++) {
    prv_step(BT_S_PLUGGED, 2, -60000, 30000);
  }
  cl_assert_equal_i(prv_step(BT_S_PLUGGED | BT_S_FULL, 0, -60000, 31000),
                    BT_STEP_LOG | BT_STEP_SAVE);
  cl_assert_equal_i(s_st.phase, BT_DONE);
  // From the end of the discharge: 30 mAh in, less the 1 mAh lost while off, plus the last minute
  cl_assert_equal_i(s_st.rec.uah, 29000);
  cl_assert_equal_i(s_st.rec.sw_uah, 29000 + 1000);
  cl_assert_equal_i(s_st.rec.tmin_c, 24);
  cl_assert_equal_i(s_st.rec.tmax_c, 31);
  cl_assert_equal_i(s_st.full[1].cycles_pct, 150);
  cl_assert_equal_i(prv_step(BT_S_PLUGGED | BT_S_FULL, 0, 0, 25000), 0);
}

void test_battery_test__plugged_before_empty(void) {
  prv_to_discharge();
  prv_step(0U, -1, 30000, 25000);
  prv_step(BT_S_PLUGGED, 0, -60000, 25000);
  cl_assert_equal_i(s_st.phase, BT_RECHARGE);
  cl_assert(!(s_st.flags & BT_F_COMPLETE));
  cl_assert_equal_i(s_st.loads, BT_LOAD_NORMAL);
}

void test_battery_test__gauge_reset(void) {
  prv_to_discharge();
  prv_step(0U, -1, 30000, 25000);
  cl_assert_equal_i(s_rec.out_uah, 500);
  // The gauge reset: its counter starts again at 0, the integral stands in
  s_resets++;
  s_counter = 0U;
  prv_step(0U, 0, 30000, 25000);
  cl_assert(s_rec.flags & BT_S_LOST);
  cl_assert(s_st.flags & BT_F_LOST);
  cl_assert_equal_i(s_rec.out_uah, 1000);
  // Then counts again
  prv_step(0U, -1, 30000, 25000);
  cl_assert(!(s_rec.flags & BT_S_LOST));
  cl_assert_equal_i(s_rec.out_uah, 1500);
}

void test_battery_test__clock_jump_is_a_gap(void) {
  prv_to_discharge();
  // The clock goes back: no integral over it, the counter bridges it
  s_time -= 7200U;
  prv_step(0U, -3, 30000, 25000);
  cl_assert_equal_i(s_rec.out_uah, 1500);
  cl_assert_equal_i(s_rec.sw_uah, 1500);
  cl_assert_equal_i(s_st.gap_uah, 1500);
}
