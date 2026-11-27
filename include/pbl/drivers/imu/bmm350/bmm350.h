/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/i2c.h>
#include <pbl/drivers/mag.h>
#include <pbl/kernel/work.h>

/**
 * @defgroup drivers_imu_bmm350 BMM350
 * @ingroup drivers_imu
 * @brief Bosch BMM350 @ref drivers_mag driver.
 *
 * The part measures in its normal mode while sampling and is suspended otherwise; the driver reads
 * it on a delayed work item at its output data rate and compensates each reading with the
 * calibration read from its OTP at init.
 *
 * @code{.c}
 * PBL_BMM350_DEFINE(s_bmm350, &s_i2c4, 0x14,
 *                   .axis_map = {1, 0, 2},
 *                   .axis_dir = {1, 1, -1});
 *
 * const struct pbl_mag *const MAG = &s_bmm350;
 * @endcode
 * @{
 */

/** @brief BMM350 calibration, from its OTP. */
struct pbl_bmm350_comp {
  /** Offsets, microtesla. */
  float offset[3];
  /** Sensitivity errors. */
  float sens[3];
  /** Temperature coefficients of the offsets. */
  float tco[3];
  /** Temperature coefficients of the sensitivities. */
  float tcs[3];
  /** Temperature offset. */
  float t_offs;
  /** Temperature sensitivity error. */
  float t_sens;
  /** Reference temperature. */
  float t0;
  /** Cross-axis sensitivities. */
  float cross_x_y;
  float cross_y_x;
  float cross_z_x;
  float cross_z_y;
};

/** @brief BMM350 driver state, owned by the driver. */
struct pbl_bmm350_state {
  /** Watch mounted rotated 180 degrees: X and Y are inverted. */
  bool rotated;
  /** ODR code while sampling, 0 when suspended. */
  uint8_t odr;
  /** A sample has been read since sampling started. */
  bool have_sample;
  /** Last sample. */
  MagData last;
  /** Temperature of the last sample, in hundredths of a degree C. */
  int16_t temp_cdeg;
  /** Bus errors. */
  uint32_t errors;
  /** Calibration. */
  struct pbl_bmm350_comp comp;
  /** Polling work. */
  struct pbl_delayed_work poll;
};

/** @brief BMM350 magnetometer. */
struct pbl_bmm350 {
  /** Magnetometer, child of the I2C bus. */
  struct pbl_mag mag;
  /** Driver state. */
  struct pbl_bmm350_state *state;
  /** I2C device. */
  struct pbl_i2c_dev i2c;
  /** Sensor axis feeding each watch axis (0: X, 1: Y, 2: Z). */
  uint8_t axis_map[3];
  /** Direction of each watch axis: 1 or -1. */
  int8_t axis_dir[3];
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_mag_ops pbl_bmm350_ops;
/** @endcond */

/**
 * @brief Define a BMM350.
 *
 * Only one instance is supported.
 *
 * @param sym Symbol of the instance, a struct pbl_mag.
 * @param _bus I2C bus.
 * @param _addr 7-bit I2C address.
 * @param ... Designated initializers of the remaining fields: @c axis_map and @c axis_dir.
 */
#define PBL_BMM350_DEFINE(sym, _bus, _addr, ...)                                \
  PBL_MAG_STATE_DEFINE(sym);                                                    \
  static struct pbl_bmm350_state sym##_bmm350_state;                            \
  PBL_DEVICE_INSTANCE(sym, struct pbl_bmm350, struct pbl_mag, mag) {            \
    .mag = PBL_MAG_INIT(sym, "bmm350", &(_bus)->dev, &pbl_bmm350_ops),          \
    .state = &sym##_bmm350_state, .i2c = PBL_I2C_DEV(_bus, _addr), __VA_ARGS__, \
  }

/** @} */
