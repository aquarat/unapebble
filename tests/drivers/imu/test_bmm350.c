/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "bmm350_regs.h"

#include <math.h>
#include <string.h>

#include <clar.h>

static uint16_t s_otp[BMM350_OTP_WORDS];

static void prv_assert_near(float actual, float expected, float tolerance) {
  cl_assert_(fabsf(actual - expected) <= tolerance, "value out of tolerance");
}

void test_bmm350__initialize(void) {
  memset(s_otp, 0, sizeof(s_otp));
}

void test_bmm350__sign_extend(void) {
  cl_assert_equal_i(bmm350_sign_extend(0x7FFU, 12U), 2047);
  cl_assert_equal_i(bmm350_sign_extend(0x800U, 12U), -2048);
  cl_assert_equal_i(bmm350_sign_extend(0xFFFFFFU, 24U), -1);
  cl_assert_equal_i(bmm350_sign_extend(0x800000U, 24U), -8388608);
  cl_assert_equal_i(bmm350_sign_extend(0x80U, 8U), -128);
  // Bits above the field are ignored
  cl_assert_equal_i(bmm350_sign_extend(0x1FFU, 8U), -1);
}

void test_bmm350__odr(void) {
  cl_assert_equal_i(BMM350_ODR_US(BMM350_ODR_400HZ), 2500);
  cl_assert_equal_i(BMM350_ODR_US(7), 80000);
  cl_assert_equal_i(BMM350_ODR_US(BMM350_ODR_1_5625HZ), 640000);
  // The longest period no longer than asked
  cl_assert_equal_i(bmm350_odr_for_interval(100000U), 7);
  cl_assert_equal_i(bmm350_odr_for_interval(80000U), 7);
  cl_assert_equal_i(bmm350_odr_for_interval(40000U), 6);
  cl_assert_equal_i(bmm350_odr_for_interval(1000U), BMM350_ODR_400HZ);
  cl_assert_equal_i(bmm350_odr_for_interval(10000000U), BMM350_ODR_1_5625HZ);
}

// The averaging allowed falls at the fastest rates (bmm350_set_odr_performance())
void test_bmm350__avg(void) {
  cl_assert_equal_i(bmm350_avg_for(BMM350_ODR_400HZ, 3U), 0);
  cl_assert_equal_i(bmm350_avg_for(3U, 3U), 1);
  cl_assert_equal_i(bmm350_avg_for(4U, 3U), 2);
  cl_assert_equal_i(bmm350_avg_for(5U, 3U), 3);
  cl_assert_equal_i(bmm350_avg_for(7U, 2U), 2);
}

// Each field where update_mag_off_sens() takes it, with its sign and scale
void test_bmm350__parse_otp(void) {
  struct pbl_bmm350_comp c;

  s_otp[0x0D] = 0x80FBU; // t_sens -128, t_offs -5
  s_otp[0x0E] = 0x3801U; // offset_x 0x801 (-2047), offset_y high nibble 3
  s_otp[0x0F] = 0x0F02U; // offset_y low byte 2, offset_z high nibble 0xF
  s_otp[0x10] = 0x40FFU; // sens_x 64, offset_z low byte 0xFF
  s_otp[0x11] = 0xC010U; // sens_z -64, sens_y 16
  s_otp[0x12] = 0x7F20U; // tcs_x 127, tco_x 32
  s_otp[0x13] = 0x01E0U; // tcs_y 1, tco_y -32
  s_otp[0x14] = 0x0000U;
  s_otp[0x15] = 0x50B0U; // cross_y_x 80, cross_x_y -80
  s_otp[0x16] = 0x0828U; // cross_z_y 8, cross_z_x 40
  s_otp[0x18] = 0xFE00U; // t0 23 - 1
  bmm350_parse_otp(s_otp, &c);

  prv_assert_near(c.offset[0], -2047.0f, 0.0f);
  prv_assert_near(c.offset[1], 0x302, 0.0f);
  prv_assert_near(c.offset[2], -1.0f, 0.0f);
  prv_assert_near(c.t_offs, -1.0f, 1e-6f);
  prv_assert_near(c.t_sens, -0.25f, 1e-6f);
  prv_assert_near(c.sens[0], 0.25f, 1e-6f);
  prv_assert_near(c.sens[1], (16.0f / 256.0f) + 0.01f, 1e-6f);
  prv_assert_near(c.sens[2], -0.25f, 1e-6f);
  prv_assert_near(c.tco[0], 1.0f, 1e-6f);
  prv_assert_near(c.tco[1], -1.0f, 1e-6f);
  prv_assert_near(c.tco[2], 0.0f, 1e-6f);
  prv_assert_near(c.tcs[0], 127.0f / 16384.0f, 1e-9f);
  prv_assert_near(c.tcs[1], 1.0f / 16384.0f, 1e-9f);
  prv_assert_near(c.tcs[2], -0.0001f, 1e-9f);
  prv_assert_near(c.t0, 22.0f, 1e-6f);
  prv_assert_near(c.cross_x_y, -0.1f, 1e-6f);
  prv_assert_near(c.cross_y_x, 0.1f, 1e-6f);
  prv_assert_near(c.cross_z_x, 0.05f, 1e-6f);
  prv_assert_near(c.cross_z_y, 0.01f, 1e-6f);
}

void test_bmm350__unpack(void) {
  const uint8_t data[BMM350_DATA_BYTES] = {0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
                                           0x00, 0x00, 0x80, 0x56, 0x34, 0x12};
  int32_t raw[4];

  bmm350_unpack(data, raw);
  cl_assert_equal_i(raw[0], 1);
  cl_assert_equal_i(raw[1], -1);
  cl_assert_equal_i(raw[2], -8388608);
  cl_assert_equal_i(raw[3], 0x123456);
}

// An uncalibrated part: the nominal gains of the SensorAPI, about 0.00707 uT a count on X and Y,
// 0.00717 on Z, 0.000981 degrees C on the temperature, which is offset by 25.49
void test_bmm350__compensate_nominal(void) {
  struct pbl_bmm350_comp c;
  const int32_t raw[4] = {7073, -7073, 6969, 50000};
  float out[4];

  // t0 of 23 with no coefficients
  bmm350_parse_otp(s_otp, &c);
  bmm350_compensate(&c, raw, out);
  prv_assert_near(out[0], 50.0f, 0.05f);
  // Y carries the SensorAPI's 1% correction
  prv_assert_near(out[1], -50.51f, 0.05f);
  prv_assert_near(out[2], 50.0f, 0.05f);
  prv_assert_near(out[3], 49.06f - 25.49f, 0.05f);
}

// The cross-axis terms, as the SensorAPI applies them
void test_bmm350__compensate_cross_axis(void) {
  struct pbl_bmm350_comp c;
  const int32_t raw[4] = {14146, 0, 0, 0};
  float out[4];

  bmm350_parse_otp(s_otp, &c);
  c.sens[1] = 0.0f;
  c.cross_y_x = 0.1f;
  c.cross_z_x = 0.05f;
  bmm350_compensate(&c, raw, out);
  prv_assert_near(out[0], 100.0f, 0.05f);
  prv_assert_near(out[1], -10.0f, 0.05f);
  prv_assert_near(out[2], -5.0f, 0.05f);
}
