/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// Restart, with the serial console's reboot reason: through the bootloader, to the tether stub
// in a slot build
[[noreturn]] void dev_usb_reboot(void);

// Power off through the bootloader; holding the power button 4 s at the next power-on gets its
// menu
[[noreturn]] void dev_usb_power_off(void);

#ifdef CONFIG_DEV_USB_TETHER
// The tether thread and its port, once the USB is up
void dev_usb_tether_start(void);
#endif
