/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

/**
 * @brief The USB device, up while any of its users wants it: the developer access, the drive.
 *
 * The controller, its PHY and their clocks are powered only while the device is connected, and
 * while they are the CPU stays out of Stop 2. Each user says whether it wants the device; the
 * device connects when the first does and disconnects when the last stops, on the system work
 * queue.
 */

enum usb_power_user {
  USB_POWER_DEV_ACCESS = 1U << 0,
  USB_POWER_DRIVE = 1U << 1,
};

/** @brief Whether @p user wants the device up. Any thread. */
void usb_power_request(enum usb_power_user user, bool on);

/** @brief Keep the device down whoever wants it, as a power measurement does. Any thread. */
void usb_power_hold_off(bool hold);

/** @brief Whether the device is connected to the bus. */
bool usb_power_is_on(void);
