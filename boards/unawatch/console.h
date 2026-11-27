/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// The console's port: the first USB serial port with the developer access, which keeps it while
// the USB is off, the USART3 debug line otherwise
#ifdef CONFIG_DEV_USB
#include <pbl/usb/cdc_acm.h>
#define UNAWATCH_CONSOLE_UART USB_CDC_ACM_UART(0)
#else
#include <devicetree/devices.h>
#define UNAWATCH_CONSOLE_UART (&dt_usart3)
#endif
