/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "sim_track.h"

#include <errno.h>
#include <stdlib.h>

#include <pbl/drivers/gnss.h>
#include <pbl/services/location.h>
#include <pbl/util/size.h>

#include <clar.h>

// A receiver whose epochs the test reports by hand.
static int s_init_ret;
static int s_start_count;
static int s_stop_count;
static uint32_t s_interval_ms;
static uint64_t s_now_ms;
static PebbleTask s_task;

uint64_t pbl_uptime_ms(void) {
  return s_now_ms;
}

PebbleTask pebble_task_get_current(void) {
  return s_task;
}

static int prv_init(const struct pbl_gnss *gnss) {
  return s_init_ret;
}

static int prv_start(const struct pbl_gnss *gnss, uint32_t interval_ms) {
  s_start_count++;
  s_interval_ms = interval_ms;
  return 0;
}

static int prv_stop(const struct pbl_gnss *gnss) {
  s_stop_count++;
  return 0;
}

static const struct pbl_gnss_ops s_ops = {.init = prv_init, .start = prv_start, .stop = prv_stop};

PBL_GNSS_STATE_DEFINE(s_gnss);
static const struct pbl_gnss s_gnss = PBL_GNSS_INIT(s_gnss, "gnss", nullptr, &s_ops);

PBL_GNSS_STATE_DEFINE(s_absent);
static const struct pbl_gnss s_absent = PBL_GNSS_INIT(s_absent, "absent", nullptr, &s_ops);

const struct pbl_gnss *const GNSS = &s_gnss;

static struct pbl_gnss_fix s_heard[4];
static int s_num_heard;

static void prv_listener_fix(const struct pbl_gnss *gnss, const struct pbl_gnss_fix *fix,
                             void *user_data) {
  if (s_num_heard < (int)PBL_ARRAY_SIZE(s_heard)) {
    s_heard[s_num_heard] = *fix;
  }
  s_num_heard++;
}

static const struct pbl_gnss_listener s_listener = {.fix = prv_listener_fix};

static void prv_report(int32_t lat_e7, uint8_t quality) {
  const struct pbl_gnss_fix fix = {.lat_e7 = lat_e7, .quality = quality, .sats_used = 9};

  s_now_ms += 1000;
  pbl_gnss_lock(&s_gnss);
  pbl_gnss_report_fix(&s_gnss, &fix);
  pbl_gnss_unlock(&s_gnss);
}

void test_gnss__initialize(void) {
  s_init_ret = 0;
  s_start_count = 0;
  s_stop_count = 0;
  s_num_heard = 0;
  s_now_ms = 1000000;
  s_task = PebbleTask_App;
  cl_assert_equal_i(pbl_device_init(&s_gnss.dev), 0);
}

void test_gnss__cleanup(void) {
  location_service_process_cleanup(PebbleTask_App);
  location_service_process_cleanup(PebbleTask_Worker);
  pbl_gnss_stop(&s_gnss);
  pbl_gnss_set_listener(&s_gnss, nullptr);
}

// Class ///////////////////////////////////////////////////////////////////////////////////////////

void test_gnss__start_stop(void) {
  struct pbl_gnss_fix fix;

  cl_assert(!pbl_gnss_is_running(&s_gnss));
  cl_assert_equal_i(pbl_gnss_start(&s_gnss, 0), -EINVAL);
  cl_assert_equal_i(pbl_gnss_start(&s_gnss, 1000), 0);
  cl_assert(pbl_gnss_is_running(&s_gnss));
  cl_assert_equal_i(s_interval_ms, 1000);
  cl_assert_equal_i(pbl_gnss_peek(&s_gnss, &fix), -ENODATA);

  cl_assert_equal_i(pbl_gnss_stop(&s_gnss), 0);
  cl_assert_equal_i(pbl_gnss_stop(&s_gnss), 0);
  cl_assert_equal_i(s_stop_count, 1);
  cl_assert(!pbl_gnss_is_running(&s_gnss));
}

void test_gnss__report_reaches_listener_and_peek(void) {
  struct pbl_gnss_fix fix;

  pbl_gnss_set_listener(&s_gnss, &s_listener);
  pbl_gnss_start(&s_gnss, 1000);
  prv_report(123, PBL_GNSS_FIX_3D);

  cl_assert_equal_i(s_num_heard, 1);
  cl_assert_equal_i(s_heard[0].lat_e7, 123);
  cl_assert_equal_i(pbl_gnss_peek(&s_gnss, &fix), 0);
  cl_assert_equal_i(fix.lat_e7, 123);

  // A restart forgets the last epoch.
  pbl_gnss_stop(&s_gnss);
  pbl_gnss_start(&s_gnss, 1000);
  cl_assert_equal_i(pbl_gnss_peek(&s_gnss, &fix), -ENODATA);
}

void test_gnss__not_ready(void) {
  struct pbl_gnss_fix fix;

  s_init_ret = -ENODEV;
  cl_assert_equal_i(pbl_device_init(&s_absent.dev), -ENODEV);
  cl_assert_equal_i(pbl_gnss_start(&s_absent, 1000), -ENODEV);
  cl_assert_equal_i(pbl_gnss_stop(&s_absent), -ENODEV);
  cl_assert_equal_i(pbl_gnss_peek(&s_absent, &fix), -ENODEV);
  cl_assert(!pbl_gnss_is_running(&s_absent));
}

// Location service ////////////////////////////////////////////////////////////////////////////////

void test_gnss__location_runs_while_any_task_wants_it(void) {
  cl_assert_equal_i(location_service_get_status(), LocationStatus_Off);

  cl_assert_equal_i(location_service_start(), 0);
  cl_assert_equal_i(location_service_start(), 0);
  s_task = PebbleTask_Worker;
  cl_assert_equal_i(location_service_start(), 0);
  cl_assert_equal_i(s_start_count, 1);
  cl_assert_equal_i(s_interval_ms, 1000);
  cl_assert_equal_i(location_service_get_status(), LocationStatus_Searching);

  location_service_stop();
  cl_assert_equal_i(s_stop_count, 0);
  // The app dies without stopping.
  location_service_process_cleanup(PebbleTask_App);
  cl_assert_equal_i(s_stop_count, 1);
  cl_assert_equal_i(location_service_get_status(), LocationStatus_Off);
}

void test_gnss__location_status(void) {
  location_service_start();
  prv_report(1, PBL_GNSS_FIX_NONE);
  cl_assert_equal_i(location_service_get_status(), LocationStatus_Searching);
  prv_report(2, PBL_GNSS_FIX_3D);
  cl_assert_equal_i(location_service_get_status(), LocationStatus_Fix);

  // The receiver goes quiet: the fix goes stale.
  s_now_ms += 5000;
  cl_assert_equal_i(location_service_get_status(), LocationStatus_Searching);
}

void test_gnss__location_read_is_lossless_then_skips_the_oldest(void) {
  struct pbl_gnss_fix fixes[LOCATION_SERVICE_HISTORY];
  uint32_t cursor;

  location_service_start();
  cursor = location_service_get_cursor();
  cl_assert_equal_i(location_service_read(&cursor, fixes, PBL_ARRAY_SIZE(fixes)), 0);

  prv_report(10, PBL_GNSS_FIX_3D);
  prv_report(11, PBL_GNSS_FIX_3D);
  cl_assert_equal_i(location_service_read(&cursor, fixes, 1), 1);
  cl_assert_equal_i(fixes[0].lat_e7, 10);
  cl_assert_equal_i(location_service_read(&cursor, fixes, PBL_ARRAY_SIZE(fixes)), 1);
  cl_assert_equal_i(fixes[0].lat_e7, 11);

  for (int i = 0; i < LOCATION_SERVICE_HISTORY + 3; i++) {
    prv_report(100 + i, PBL_GNSS_FIX_3D);
  }
  cl_assert_equal_i(location_service_read(&cursor, fixes, PBL_ARRAY_SIZE(fixes)),
                    LOCATION_SERVICE_HISTORY);
  cl_assert_equal_i(fixes[0].lat_e7, 103);
  cl_assert_equal_i(fixes[LOCATION_SERVICE_HISTORY - 1].lat_e7, 110);

  struct pbl_gnss_fix last;
  cl_assert(location_service_peek(&last));
  cl_assert_equal_i(last.lat_e7, 110);
}

// Simulator route /////////////////////////////////////////////////////////////////////////////////

void test_gnss__sim_track_is_a_realistic_loop(void) {
  struct gnss_sim_track track;
  struct pbl_gnss_fix fix = {};
  uint64_t speed_sum = 0;
  int stopped = 0;
  int32_t min_alt = INT32_MAX;
  int32_t max_alt = INT32_MIN;
  const uint32_t len = gnss_sim_track_length_mm();

  cl_assert(len > 3000000 && len < 3300000);

  gnss_sim_track_reset(&track);
  int s;
  for (s = 0; s < 3000; s++) {
    const uint8_t seg = track.seg;
    gnss_sim_track_step(&track, &fix);
    speed_sum += fix.speed_mm_s;
    stopped += fix.speed_mm_s == 0;
    min_alt = fix.alt_cm < min_alt ? fix.alt_cm : min_alt;
    max_alt = fix.alt_cm > max_alt ? fix.alt_cm : max_alt;
    if (seg != 0 && track.seg == 0) {
      break;
    }
  }

  // Back at the start within a few metres, after 15 to 19 minutes at 5 to 6 min/km.
  cl_assert(s > 900 && s < 1140);
  cl_assert(abs(fix.lat_e7 - GNSS_SIM_TRACK_ORIGIN_LAT_E7) < 500);
  cl_assert(abs(fix.lon_e7 - GNSS_SIM_TRACK_ORIGIN_LON_E7) < 500);
  cl_assert(stopped >= 14 && stopped <= 16);
  const uint64_t avg = speed_sum / (uint64_t)(s + 1);
  cl_assert(avg > 2800 && avg < 3400);
  cl_assert(max_alt - min_alt > 3200 && max_alt - min_alt < 4200);
}
