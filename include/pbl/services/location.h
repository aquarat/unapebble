/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/drivers/gnss.h>

#include <kernel/pebble_tasks.h>

/**
 * @defgroup services_location Location service
 * @ingroup services
 * @brief On-watch positioning from the GNSS receiver.
 *
 * Tasks start and stop the service; the receiver runs at 1 Hz while at least one task wants it,
 * and the service keeps the last few epochs so that a client polling once a second loses none.
 * Kernel-side only: system apps call it directly, there is no SDK API.
 *
 * @code{.c}
 * uint32_t cursor;
 * struct pbl_gnss_fix fixes[4];
 *
 * location_service_start();
 * cursor = location_service_get_cursor();
 * ...
 * size_t n = location_service_read(&cursor, fixes, PBL_ARRAY_SIZE(fixes));
 * ...
 * location_service_stop();
 * @endcode
 * @{
 */

/** @brief Epochs kept for clients to read. */
#define LOCATION_SERVICE_HISTORY 8

/** @brief Location status. */
typedef enum LocationStatus {
  /** The board has no receiver, or it failed. */
  LocationStatus_Unavailable,
  /** No task wants a position. */
  LocationStatus_Off,
  /** Started, without a position yet or since the signal was lost. */
  LocationStatus_Searching,
  /** The last epoch, at most a few seconds old, has a position. */
  LocationStatus_Fix,
} LocationStatus;

/**
 * @brief Check whether the board can give a position.
 * @return true if a GNSS receiver is ready.
 */
bool location_service_is_available(void);

/**
 * @brief Start positioning for the current task.
 *
 * Idempotent per task.
 *
 * @return 0, -ENODEV without a receiver, or a negative errno.
 */
int location_service_start(void);

/** @brief Stop positioning for the current task. */
void location_service_stop(void);

/**
 * @brief Stop positioning for a task that is going away.
 * @param task Task.
 */
void location_service_process_cleanup(PebbleTask task);

/**
 * @brief Get the status.
 * @return Status.
 */
LocationStatus location_service_get_status(void);

/**
 * @brief Read the last epoch.
 * @param[out] fix Epoch.
 * @return true if an epoch was received since the receiver started.
 */
bool location_service_peek(struct pbl_gnss_fix *fix);

/**
 * @brief Get the cursor of the next epoch to come.
 * @return Cursor, for location_service_read().
 */
uint32_t location_service_get_cursor(void);

/**
 * @brief Read the epochs received since a cursor, oldest first.
 *
 * Epochs older than the last @ref LOCATION_SERVICE_HISTORY are skipped.
 *
 * @param[in,out] cursor Cursor, advanced past the epochs read.
 * @param[out] fixes Epochs.
 * @param max Capacity of @p fixes.
 * @return Number of epochs read.
 */
size_t location_service_read(uint32_t *cursor, struct pbl_gnss_fix *fixes, size_t max);

/** @} */
