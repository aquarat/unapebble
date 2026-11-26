/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pca9420_regs.h"

#include <clar.h>

static uint8_t s_regs[PCA9420_CHG_REGS];

// The UNA Watch: 100 mA, 14 mA termination, 4.36 V
void test_pca9420__unawatch(void) {
  cl_assert_equal_i(pca9420_charge_encode(100000U, 14000U, 4360000U, s_regs), 0);
  cl_assert_equal_i(s_regs[0], 0x14); // ICHG_CC 100 mA
  cl_assert_equal_i(s_regs[1], 0x0E); // ICHG_TOPOFF 14 mA
  cl_assert_equal_i(s_regs[2], 0x08); // ICHG_LOW 8 mA
  cl_assert_equal_i(s_regs[3], 0x04); // ICHG_DEAD 4 mA, 5 s
  cl_assert_equal_i(s_regs[4], 0x66); // VBAT_RESTART 240 mV, VBAT_REG 4.36 V
  cl_assert_equal_i(s_regs[5], 0x95); // 5 h, 30 min, 6.4 min
}

void test_pca9420__rounds_down(void) {
  cl_assert_equal_i(pca9420_charge_encode(104999U, 14999U, 4379999U, s_regs), 0);
  cl_assert_equal_i(s_regs[0], 20);
  cl_assert_equal_i(s_regs[1], 14);
  cl_assert_equal_i(s_regs[4] & 0x3F, 0x26);

  cl_assert_equal_i(pca9420_charge_encode(5000U, 1000U, 3600000U, s_regs), 0);
  cl_assert_equal_i(s_regs[4] & 0x3F, 0);
  cl_assert_equal_i(pca9420_charge_encode(315000U, 63000U, 4600000U, s_regs), 0);
  cl_assert_equal_i(s_regs[0], 0x3F);
  cl_assert_equal_i(s_regs[4] & 0x3F, 0x32);
}

void test_pca9420__precharge_below_charge(void) {
  cl_assert_equal_i(pca9420_charge_encode(5000U, 1000U, 4200000U, s_regs), 0);
  cl_assert_equal_i(s_regs[2], 5);
  cl_assert_equal_i(s_regs[3], 4);
  cl_assert(s_regs[3] <= s_regs[2]);
}

void test_pca9420__refuses(void) {
  cl_assert_equal_i(pca9420_charge_encode(4999U, 14000U, 4200000U, s_regs), -EINVAL);
  cl_assert_equal_i(pca9420_charge_encode(320000U, 14000U, 4200000U, s_regs), -EINVAL);
  cl_assert_equal_i(pca9420_charge_encode(100000U, 999U, 4200000U, s_regs), -EINVAL);
  cl_assert_equal_i(pca9420_charge_encode(100000U, 64000U, 4200000U, s_regs), -EINVAL);
  cl_assert_equal_i(pca9420_charge_encode(100000U, 14000U, 3599999U, s_regs), -EINVAL);
  cl_assert_equal_i(pca9420_charge_encode(100000U, 14000U, 4600001U, s_regs), -EINVAL);
}

void test_pca9420__status(void) {
  const uint8_t pwr = PCA9420_CHG_STATUS_0_IN_PWR_OK;

  // Without input power, CHG_STATUS_2 means nothing
  cl_assert_equal_i(pca9420_status_decode(0x00, 0x46), PBL_CHARGER_STATUS_UNKNOWN);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x00), PBL_CHARGER_STATUS_UNKNOWN);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x21), PBL_CHARGER_STATUS_TRICKLE);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x32), PBL_CHARGER_STATUS_TRICKLE);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x43), PBL_CHARGER_STATUS_CONSTANT_CURRENT);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x44), PBL_CHARGER_STATUS_CONSTANT_CURRENT);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x53), PBL_CHARGER_STATUS_CONSTANT_VOLTAGE);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x55), PBL_CHARGER_STATUS_CONSTANT_VOLTAGE);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x56), PBL_CHARGER_STATUS_FULL);
  cl_assert_equal_i(pca9420_status_decode(pwr, 0x57), PBL_CHARGER_STATUS_UNKNOWN);
}
