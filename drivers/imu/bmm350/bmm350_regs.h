/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// BMM350 registers, and the compensation of its readings from the calibration in its OTP, apart
// from the bus so the unit tests can check them. The compensation follows Bosch Sensortec's
// BMM350 SensorAPI v1.4.0 (bmm350.c: update_mag_off_sens(), read_out_raw_data(),
// update_default_coefiecents(), bmm350_get_compensated_mag_xyz_temp_data()), BSD-3-Clause, as
// NOTICE says.

#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/imu/bmm350/bmm350.h>

#define BMM350_CHIP_ID          0x00U
#define BMM350_PMU_CMD_AGGR_SET 0x04U
#define BMM350_PMU_CMD_AXIS_EN  0x05U
#define BMM350_PMU_CMD          0x06U
#define BMM350_PMU_CMD_STATUS_0 0x07U
#define BMM350_INT_CTRL         0x2EU
#define BMM350_MAG_X_XLSB       0x31U
#define BMM350_OTP_CMD          0x50U
#define BMM350_OTP_DATA_MSB     0x52U
#define BMM350_OTP_STATUS       0x55U
#define BMM350_CMD              0x7EU

#define BMM350_CHIP_ID_VAL 0x33U

// A read starts with two dummy bytes
#define BMM350_DUMMY_BYTES 2U

#define BMM350_CMD_SOFTRESET 0xB6U

#define BMM350_PMU_CMD_SUS     0x00U
#define BMM350_PMU_CMD_NM      0x01U
#define BMM350_PMU_CMD_UPD_OAE 0x02U
#define BMM350_PMU_CMD_FGR     0x05U
#define BMM350_PMU_CMD_BR      0x07U

// PMU_CMD_STATUS_0: the last command in bits 7:5, normal mode in bit 3
#define BMM350_PMU_STATUS_CMD(v) (((v) >> 5) & 0x07U)
#define BMM350_PMU_STATUS_NORMAL (1U << 3)

#define BMM350_AXIS_EN_XYZ 0x07U

#define BMM350_OTP_CMD_DIR_READ 0x20U
#define BMM350_OTP_CMD_PWR_OFF  0x80U
#define BMM350_OTP_STATUS_DONE  0x01U
#define BMM350_OTP_STATUS_ERROR 0xE0U
#define BMM350_OTP_WORDS        32U

// Data: X, Y, Z and temperature, 24 bits each, little endian
#define BMM350_DATA_BYTES 12U

// Timings, in microseconds (bmm350_defs.h)
#define BMM350_STARTUP_US           3000U
#define BMM350_SOFTRESET_US         24000U
#define BMM350_OTP_POLL_US          300U
#define BMM350_OTP_POLLS            20U
#define BMM350_GOTO_SUSPEND_US      6000U
#define BMM350_SUSPEND_TO_NORMAL_US 38000U
#define BMM350_UPD_OAE_US           1000U
#define BMM350_BR_US                14000U
#define BMM350_FGR_US               18000U

// Output data rates: 400 Hz (code 2) to 1.5625 Hz (code 10), 2.5 ms << (code - 2)
#define BMM350_ODR_400HZ    0x02U
#define BMM350_ODR_1_5625HZ 0x0AU
#define BMM350_ODR_US(odr)  (2500U << ((odr) - BMM350_ODR_400HZ))

//! @p v of @p bits bits, two's complement.
static inline int32_t bmm350_sign_extend(uint32_t v, unsigned int bits) {
  const uint32_t sign = 1UL << (bits - 1U);

  v &= (sign << 1) - 1U;

  return (int32_t)(v ^ sign) - (int32_t)sign;
}

//! The ODR code for the longest period no longer than @p interval_us, saturating at 400 Hz and
//! 1.5625 Hz.
static inline uint8_t bmm350_odr_for_interval(uint32_t interval_us) {
  uint8_t odr = BMM350_ODR_1_5625HZ;

  while (odr > BMM350_ODR_400HZ && BMM350_ODR_US(odr) > interval_us) {
    odr--;
  }

  return odr;
}

//! @p avg (0 for 1 sample to 3 for 8), reduced to what the part allows at @p odr.
static inline uint8_t bmm350_avg_for(uint8_t odr, uint8_t avg) {
  // 400 Hz takes 1 sample, 200 Hz 2, 100 Hz 4
  const uint8_t max = (odr <= BMM350_ODR_400HZ) ? 0U : (odr == 3U) ? 1U : (odr == 4U) ? 2U : 3U;

  return (avg > max) ? max : avg;
}

//! The calibration of a part from its OTP words.
static inline void bmm350_parse_otp(const uint16_t otp[BMM350_OTP_WORDS],
                                    struct pbl_bmm350_comp *c) {
  const uint16_t off_x = otp[0x0E] & 0x0FFFU;
  const uint16_t off_y = (uint16_t)(((otp[0x0E] & 0xF000U) >> 4) + (otp[0x0F] & 0x00FFU));
  const uint16_t off_z = (uint16_t)((otp[0x0F] & 0x0F00U) + (otp[0x10] & 0x00FFU));

  c->offset[0] = (float)bmm350_sign_extend(off_x, 12U);
  c->offset[1] = (float)bmm350_sign_extend(off_y, 12U);
  c->offset[2] = (float)bmm350_sign_extend(off_z, 12U);
  c->t_offs = (float)bmm350_sign_extend(otp[0x0D] & 0xFFU, 8U) / 5.0f;

  c->sens[0] = (float)bmm350_sign_extend(otp[0x10] >> 8, 8U) / 256.0f;
  c->sens[1] = ((float)bmm350_sign_extend(otp[0x11] & 0xFFU, 8U) / 256.0f) + 0.01f;
  c->sens[2] = (float)bmm350_sign_extend(otp[0x11] >> 8, 8U) / 256.0f;
  c->t_sens = (float)bmm350_sign_extend(otp[0x0D] >> 8, 8U) / 512.0f;

  for (unsigned int i = 0U; i < 3U; i++) {
    c->tco[i] = (float)bmm350_sign_extend(otp[0x12 + i] & 0xFFU, 8U) / 32.0f;
    c->tcs[i] = (float)bmm350_sign_extend(otp[0x12 + i] >> 8, 8U) / 16384.0f;
  }
  c->tcs[2] -= 0.0001f;

  c->t0 = ((float)bmm350_sign_extend(otp[0x18], 16U) / 512.0f) + 23.0f;

  c->cross_x_y = (float)bmm350_sign_extend(otp[0x15] & 0xFFU, 8U) / 800.0f;
  c->cross_y_x = (float)bmm350_sign_extend(otp[0x15] >> 8, 8U) / 800.0f;
  c->cross_z_x = (float)bmm350_sign_extend(otp[0x16] & 0xFFU, 8U) / 800.0f;
  c->cross_z_y = (float)bmm350_sign_extend(otp[0x16] >> 8, 8U) / 800.0f;
}

//! Raw X, Y, Z and temperature from the 12 data bytes.
static inline void bmm350_unpack(const uint8_t data[BMM350_DATA_BYTES], int32_t raw[4]) {
  for (unsigned int i = 0U; i < 4U; i++) {
    const uint8_t *p = &data[3U * i];

    raw[i] = bmm350_sign_extend(p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16), 24U);
  }
}

//! The field in microtesla on the part's axes, and its temperature in degrees C, from raw values.
static inline void bmm350_compensate(const struct pbl_bmm350_comp *c, const int32_t raw[4],
                                     float out[4]) {
  // The nominal gains of the part (update_default_coefiecents())
  const float power = 1000000.0f / 1048576.0f;
  const float adc_gain = 1.0f / 1.5f;
  const float lut_gain = 0.714607238769531f;
  const float lsb_xy = power / (14.55f * 19.46f * adc_gain * lut_gain);
  const float lsb_z = power / (9.0f * 31.0f * adc_gain * lut_gain);
  const float lsb_t = 1.0f / (0.00204f * adc_gain * lut_gain * 1048576.0f);
  const float div = 1.0f - (c->cross_y_x * c->cross_x_y);
  float v[3];
  float t;

  v[0] = (float)raw[0] * lsb_xy;
  v[1] = (float)raw[1] * lsb_xy;
  v[2] = (float)raw[2] * lsb_z;
  t = (float)raw[3] * lsb_t;
  if (t > 0.0f) {
    t -= 25.49f;
  } else if (t < 0.0f) {
    t += 25.49f;
  }
  t = ((1.0f + c->t_sens) * t) + c->t_offs;

  for (unsigned int i = 0U; i < 3U; i++) {
    v[i] *= 1.0f + c->sens[i];
    v[i] += c->offset[i];
    v[i] += c->tco[i] * (t - c->t0);
    v[i] /= 1.0f + (c->tcs[i] * (t - c->t0));
  }

  out[0] = (v[0] - (c->cross_x_y * v[1])) / div;
  out[1] = (v[1] - (c->cross_y_x * v[0])) / div;
  out[2] = v[2] + (((v[0] * ((c->cross_y_x * c->cross_z_y) - c->cross_z_x)) -
                    (v[1] * (c->cross_z_y - (c->cross_x_y * c->cross_z_x)))) /
                   div);
  out[3] = t;
}
