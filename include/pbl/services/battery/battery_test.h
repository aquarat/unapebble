/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/drivers/gauge.h>

/**
 * @defgroup services_battery_test Battery capacity test
 * @ingroup services_battery
 * @brief Measures the charge the battery gives from full to the low battery power-off, and
 * takes back, across that power-off.
 *
 * The test charges to full, discharges until the watch powers itself off, and on the next boot
 * logs the recharge to full. It samples on the battery service's polls, about once a minute,
 * and logs to the filesystem.
 * @{
 */

/** @brief Phases of the test. */
enum battery_test_phase {
  BATTERY_TEST_IDLE,
  BATTERY_TEST_CHARGE,
  BATTERY_TEST_FULL,
  BATTERY_TEST_DISCHARGE,
  BATTERY_TEST_RECHARGE,
  BATTERY_TEST_DONE,
};

/**
 * @brief Take a sample, from the battery service's poll.
 *
 * @param r Gauge reading of this poll, or NULL if it failed.
 * @param plugged External power present.
 * @param pct Percentage the battery service reports, 0 for an empty battery.
 */
void battery_test_sample(const struct pbl_gauge_reading *r, bool plugged, uint8_t pct);

/** @brief Start a new test, discarding the last one. */
void battery_test_start(void);

/** @brief Cancel the test, or discard its results. */
void battery_test_cancel(void);

/** @brief Get the phase of the test. */
enum battery_test_phase battery_test_get_phase(void);

/** @brief Choose the heavy load, or the normal one, for the discharge. */
void battery_test_set_heavy(bool heavy);

/** @brief Whether the heavy load is chosen. */
bool battery_test_is_heavy(void);

/** @brief Report whether the heavy load is running, for the samples. */
void battery_test_set_heavy_running(bool running);

/**
 * @brief Describe the test, its phase or its results, in lines of text.
 *
 * @param buf Buffer.
 * @param len Size of @p buf.
 */
void battery_test_describe(char *buf, size_t len);

/** @} */
