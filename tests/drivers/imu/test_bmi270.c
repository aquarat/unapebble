/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "bmi270_regs.h"

#include <clar.h>

static uint32_t prv_crc32(const uint8_t *data, uint32_t len) {
  uint32_t crc = 0xFFFFFFFFU;

  for (uint32_t i = 0U; i < len; i++) {
    crc ^= data[i];
    for (unsigned int b = 0U; b < 8U; b++) {
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
  }

  return ~crc;
}

void test_bmi270__initialize(void) {
}

// Bosch's file, byte for byte: the CRC-32 of bmi270_config_file in SensorAPI v2.86.1
void test_bmi270__config_file_is_bosch(void) {
  cl_assert_equal_i(sizeof(bmi270_config_file), 8192);
  cl_assert_equal_i(prv_crc32(bmi270_config_file, sizeof(bmi270_config_file)), 0x229a1189);
}

void test_bmi270__odr_periods(void) {
  cl_assert_equal_i(BMI270_ODR_US(BMI270_ODR_12_5HZ), 80000);
  cl_assert_equal_i(BMI270_ODR_US(6), 40000);
  cl_assert_equal_i(BMI270_ODR_US(7), 20000);
  cl_assert_equal_i(BMI270_ODR_US(BMI270_ODR_100HZ), 10000);
}

// The longest interval no longer than asked, saturating at 12.5 and 100 Hz
void test_bmi270__odr_for_interval(void) {
  cl_assert_equal_i(bmi270_odr_for_interval(1000000U), BMI270_ODR_12_5HZ);
  // The accelerometer service's 10 Hz floor
  cl_assert_equal_i(bmi270_odr_for_interval(100000U), BMI270_ODR_12_5HZ);
  cl_assert_equal_i(bmi270_odr_for_interval(80000U), BMI270_ODR_12_5HZ);
  cl_assert_equal_i(bmi270_odr_for_interval(79999U), 6);
  cl_assert_equal_i(bmi270_odr_for_interval(40000U), 6);
  cl_assert_equal_i(bmi270_odr_for_interval(20000U), 7);
  cl_assert_equal_i(bmi270_odr_for_interval(10000U), BMI270_ODR_100HZ);
  cl_assert_equal_i(bmi270_odr_for_interval(5000U), BMI270_ODR_100HZ);
  cl_assert_equal_i(bmi270_odr_for_interval(0U), BMI270_ODR_100HZ);
}

void test_bmi270__mode(void) {
  struct bmi270_mode m;

  // Off
  m = bmi270_mode_for(0U, false);
  cl_assert_equal_i(m.odr, 0);
  // Shakes alone: the feature engine's 50 Hz
  m = bmi270_mode_for(0U, true);
  cl_assert_equal_i(m.odr, 7);
  cl_assert_equal_i(m.downs, 0);
  // 25 Hz alone
  m = bmi270_mode_for(6U, false);
  cl_assert_equal_i(m.odr, 6);
  cl_assert_equal_i(m.downs, 0);
  // 25 Hz with shakes: 50 Hz, the FIFO halved
  m = bmi270_mode_for(6U, true);
  cl_assert_equal_i(m.odr, 7);
  cl_assert_equal_i(m.downs, 1);
  // 12.5 Hz with shakes: a quarter
  m = bmi270_mode_for(BMI270_ODR_12_5HZ, true);
  cl_assert_equal_i(m.odr, 7);
  cl_assert_equal_i(m.downs, 2);
  // 100 Hz is fast enough for both
  m = bmi270_mode_for(BMI270_ODR_100HZ, true);
  cl_assert_equal_i(m.odr, BMI270_ODR_100HZ);
  cl_assert_equal_i(m.downs, 0);
}

void test_bmi270__any_motion_threshold(void) {
  // 1/2048 g a count
  cl_assert_equal_i(bmi270_any_motion_threshold(100U, 32U, 999U), 66);
  cl_assert_equal_i(bmi270_any_motion_threshold(0U, 32U, 999U), 2046);
  cl_assert_equal_i(bmi270_any_motion_threshold(200U, 32U, 999U), 66);
  // The accelerometer service's default, 55%
  cl_assert_equal_i(bmi270_any_motion_threshold(55U, 32U, 999U), 958);
  // Capped at the field's 11 bits
  cl_assert_equal_i(bmi270_any_motion_threshold(0U, 32U, 1500U), BMI270_ANY_MOTION_THR_MAX);
}

// Bosch's layout: duration and axes, then threshold and enable (bmi2_defs.h)
void test_bmi270__any_motion_encode(void) {
  uint8_t out[4];

  bmi270_any_motion_encode(2U, 958U, true, out);
  cl_assert_equal_i(out[0], 0x02);
  cl_assert_equal_i(out[1], 0xE0);
  cl_assert_equal_i(out[2], 958 & 0xFF);
  cl_assert_equal_i(out[3], 0x80 | (958 >> 8));

  bmi270_any_motion_encode(0x2FFFU, 0xFFFFU, false, out);
  cl_assert_equal_i(out[0], 0xFF);
  cl_assert_equal_i(out[1], 0xEF);
  cl_assert_equal_i(out[2], 0xFF);
  cl_assert_equal_i(out[3], 0x07);
}

void test_bmi270__fifo_frames(void) {
  cl_assert_equal_i(bmi270_fifo_frames(0U, 128U), 0);
  cl_assert_equal_i(bmi270_fifo_frames(5U, 128U), 0);
  cl_assert_equal_i(bmi270_fifo_frames(6U, 128U), 1);
  cl_assert_equal_i(bmi270_fifo_frames(755U, 128U), 125);
  cl_assert_equal_i(bmi270_fifo_frames(2048U, 128U), 128);
}

void test_bmi270__shake_axis(void) {
  const int16_t rest[3] = {0, 0, -1000};
  int8_t dir = 0;

  cl_assert_equal_i(bmi270_shake_axis(rest, (const int16_t[]){500, 100, -1000}, &dir), 0);
  cl_assert_equal_i(dir, 1);
  cl_assert_equal_i(bmi270_shake_axis(rest, (const int16_t[]){100, -700, -900}, &dir), 1);
  cl_assert_equal_i(dir, -1);
  cl_assert_equal_i(bmi270_shake_axis(rest, (const int16_t[]){0, 0, 200}, &dir), 2);
  cl_assert_equal_i(dir, 1);
}
