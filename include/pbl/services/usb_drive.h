/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

/**
 * @brief The workouts as a USB drive: plugged into a computer, the watch shows a read-only drive
 * of FIT files, one for each finished workout, encoded as the computer reads them.
 *
 * The USB comes up when the watch is plugged in and the setting is on (the default). Without a
 * computer configuring it within a few seconds (a charger) it goes down again until the next
 * plug-in, and so it does once the computer ejects the drive. While the drive is up the CPU stays
 * out of Stop 2. A workout saved or deleted meanwhile takes the drive away for a few seconds,
 * once the computer has stopped reading, and brings it back with the change.
 */

void usb_drive_init(void);

/** @brief The setting: whether the watch shows the drive when plugged in. */
bool usb_drive_get_enabled(void);

void usb_drive_set_enabled(bool enabled);

/** @brief The workouts changed: one was saved or deleted. Any thread. */
void usb_drive_contents_changed(void);
