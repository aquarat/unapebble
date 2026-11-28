/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The drive after two serial ports, as the UNA Watch has them, on a high-speed controller of 9
// endpoints each way: its descriptors, its configuration, its class requests

#define FAKE_NUM_EPS   9U
#define FAKE_MAX_SPEED PBL_UDC_SPEED_HIGH

#include "fake_udc.h"

#include <stddef.h>

#include <pbl/usb/cdc_acm.h>
#include <pbl/usb/msc.h>

#define PORT_LEN 66U
#define OFF_MSC  (9U + 2U * PORT_LEN)

bool pbl_device_is_ready(const struct pbl_device *dev) {
  return true;
}

void test_usbd_msc__initialize(void) {
  s_speed = PBL_UDC_SPEED_HIGH;
  cl_assert_equal_i(pbl_usbd_enable(), 0);
}

void test_usbd_msc__cleanup(void) {
  cl_assert_equal_i(pbl_usbd_disable(), 0);
}

void test_usbd_msc__config_desc(void) {
  cl_assert_equal_i(prv_get_descriptor(PBL_USB_DESC_CONFIGURATION, 0U), OFF_MSC + 9U + 7U + 7U);
  cl_assert_equal_i(prv_le16(2U), OFF_MSC + 23U);
  cl_assert_equal_i(s_in[4], 5);

  // Interface 4: mass storage, SCSI, bulk-only, two endpoints after the ports' 1 to 4 IN and 1
  // and 2 OUT, of 512 bytes at high speed
  cl_assert_equal_i(s_in[OFF_MSC + 1U], PBL_USB_DESC_INTERFACE);
  cl_assert_equal_i(s_in[OFF_MSC + 2U], 4);
  cl_assert_equal_i(s_in[OFF_MSC + 4U], 2);
  cl_assert_equal_i(s_in[OFF_MSC + 5U], 0x08);
  cl_assert_equal_i(s_in[OFF_MSC + 6U], 0x06);
  cl_assert_equal_i(s_in[OFF_MSC + 7U], 0x50);
  cl_assert_equal_i(s_in[OFF_MSC + 9U + 2U], 0x03);
  cl_assert_equal_i(s_in[OFF_MSC + 9U + 3U], PBL_UDC_EP_BULK);
  cl_assert_equal_i(prv_le16(OFF_MSC + 9U + 4U), 512);
  cl_assert_equal_i(s_in[OFF_MSC + 16U + 2U], 0x85);
  cl_assert_equal_i(prv_le16(OFF_MSC + 16U + 4U), 512);

  {
    const uint8_t name = s_in[OFF_MSC + 8U];
    const int len = prv_get_descriptor(PBL_USB_DESC_STRING, name);

    prv_assert_utf16(2U, (size_t)len - 2U, "Workouts");
  }
}

void test_usbd_msc__configured_and_its_requests(void) {
  const uint8_t type = PBL_USB_TYPE_CLASS | PBL_USB_RECIP_INTERFACE;

  memset(s_enabled_eps, 0, sizeof(s_enabled_eps));
  cl_assert_equal_i(prv_request(PBL_USB_TYPE_STANDARD, PBL_USB_REQ_SET_CONFIGURATION, 1U, 0U, 0U),
                    0);
  cl_assert(pbl_usb_msc_is_configured());
  cl_assert(s_enabled_eps[1][5]);
  cl_assert(s_enabled_eps[0][3]);

  // GET MAX LUN: one unit
  cl_assert_equal_i(prv_request_in(type, 0xfe, 0U, 4U, 1U), 1);
  cl_assert_equal_i(s_in[0], 0);
  // Bulk-only reset
  cl_assert_equal_i(prv_request(type, 0xff, 0U, 4U, 0U), 0);
  cl_assert(s_enabled_eps[1][5]);
  // Not the ports'
  cl_assert_equal_i(prv_request_in(type, 0xfe, 0U, 0U, 1U), -1);
}
