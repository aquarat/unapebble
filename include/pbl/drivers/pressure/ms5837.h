/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/i2c.h>
#include <pbl/drivers/pressure.h>
#include <pbl/drivers/regulator.h>

/**
 * @defgroup drivers_pressure_ms5837 MS5837
 * @ingroup drivers_pressure
 * @brief TE Connectivity MS5837-02BA @ref drivers_pressure driver.
 *
 * Each read runs a pressure then a temperature conversion. The driver powers the part through its
 * supply, when it has one, at init: off for 20 ms, then on, and keeps it on; a read that fails
 * powers it again once and tries again.
 *
 * @code{.c}
 * PBL_MS5837_DEFINE(s_ms5837, &s_i2c2, 0x76, &s_baro_ldo.reg);
 *
 * const struct pbl_pressure *const PRESSURE = &s_ms5837;
 * @endcode
 * @{
 */

/** @brief MS5837 driver state. */
struct pbl_ms5837_state {
  /** Calibration PROM: C0 to C6. */
  uint16_t prom[7];
  /** Temperature of the last read, in hundredths of a degree C. */
  int32_t temp_cdeg;
};

/** @brief MS5837 pressure sensor. */
struct pbl_ms5837 {
  /** Pressure sensor, child of the I2C bus. */
  struct pbl_pressure pressure;
  /** Driver state. */
  struct pbl_ms5837_state *state;
  /** I2C device. */
  struct pbl_i2c_dev i2c;
  /** Supply, or NULL. */
  const struct pbl_regulator *supply;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_pressure_ops pbl_ms5837_ops;
/** @endcond */

/**
 * @brief Define an MS5837.
 *
 * @param sym Symbol of the instance, a struct pbl_pressure.
 * @param _bus I2C bus.
 * @param _addr 7-bit I2C address.
 * @param _supply Supply, or NULL.
 */
#define PBL_MS5837_DEFINE(sym, _bus, _addr, _supply)                                    \
  PBL_PRESSURE_STATE_DEFINE(sym);                                                       \
  static struct pbl_ms5837_state sym##_ms5837_state;                                    \
  PBL_DEVICE_INSTANCE(sym, struct pbl_ms5837, struct pbl_pressure, pressure) {          \
    .pressure = PBL_PRESSURE_INIT(sym, "ms5837", &(_bus)->dev, &pbl_ms5837_ops),        \
    .state = &sym##_ms5837_state, .i2c = PBL_I2C_DEV(_bus, _addr), .supply = (_supply), \
  }

/** @} */
