/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

/**
 * @defgroup dev_usb USB developer access
 * @ingroup services
 * @brief The console and the tether protocol on two USB serial ports, behind a setting.
 *
 * The setting is off by default and kept in a settings file of its own, out of reach of the
 * preferences a phone syncs. The USB is powered up while the setting is on and, with
 * @c CONFIG_DEV_USB_FOLLOW_VBUS, the charger sees an input; @c CONFIG_DEV_USB_FORCE keeps it on
 * whatever the setting.
 * @{
 */

/** @brief Read the setting and connect the USB if it is on. On KernelMain, after the prefs. */
void dev_usb_init(void);

/** @brief Whether the setting is on. */
bool dev_usb_get_enabled(void);

/**
 * @brief Turn the setting on or off, and keep it. The USB follows on the system work queue.
 *
 * @param enabled New value.
 */
void dev_usb_set_enabled(bool enabled);

/** @brief Whether the setting is overridden on, by @c CONFIG_DEV_USB_FORCE. */
bool dev_usb_is_forced(void);

/** @brief Whether the device is connected to the bus now. */
bool dev_usb_is_connected(void);

/**
 * @brief Keep the USB off for a while whatever the setting, e.g. to measure the power without it.
 *
 * Not kept: the setting applies again once released, and after a restart.
 *
 * @param hold True to disconnect and power the USB down, false to let it follow the setting.
 */
void dev_usb_hold_off(bool hold);

/** @} */
