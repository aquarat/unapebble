/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <process_management/pebble_process_md.h>

//! Run activity, on the watch's own sensors.
const PebbleProcessMd *sport_run_get_app_info(void);

//! Bike activity.
const PebbleProcessMd *sport_bike_get_app_info(void);
