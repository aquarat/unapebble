/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/battery.h>
#include <pbl/drivers/gauge.h>
#include <pbl/drivers/i2c.h>

/**
 * @defgroup drivers_gauge_max17262 MAX17262
 * @ingroup drivers_gauge
 * @brief @ref drivers_gauge driver for the Maxim MAX17262, with its internal sense resistor, on
 * the ModelGauge m5 EZ model.
 *
 * The model comes from the battery: its capacity, its termination current and, from its charge
 * voltage, whether it is a high-voltage cell. It is loaded only when the gauge lost it in a
 * power-on reset, or holds another one, so what the gauge learned of the cell survives resets.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
struct pbl_gauge_max17262_state {
  // The model is in the gauge
  bool configured;
};

struct pbl_gauge_max17262 {
  struct pbl_gauge gauge;
  struct pbl_i2c_dev i2c;
  const struct pbl_battery *battery;
  // Empty and recovery voltages, in uV
  uint32_t empty_uv;
  uint32_t recovery_uv;
  struct pbl_gauge_max17262_state *state;
};

extern const struct pbl_gauge_ops pbl_gauge_max17262_ops;
/** @endcond */

/**
 * @brief Define a MAX17262.
 *
 * @param sym Symbol of the gauge.
 * @param _bus I2C bus.
 * @param _addr 7-bit I2C address.
 * @param _battery Battery, a struct pbl_battery.
 * @param _empty_uv Voltage at which the battery is empty under load, in uV.
 * @param _recovery_uv Voltage above which the empty detection clears, in uV.
 */
#define PBL_GAUGE_MAX17262_DEFINE(sym, _bus, _addr, _battery, _empty_uv, _recovery_uv) \
  PBL_GAUGE_STATE_DEFINE(sym);                                                         \
  static struct pbl_gauge_max17262_state sym##_max17262_state;                         \
  PBL_DEVICE_INSTANCE(sym, struct pbl_gauge_max17262, struct pbl_gauge, gauge) {       \
    .gauge = PBL_GAUGE_INIT(sym, "max17262", &(_bus)->dev, &pbl_gauge_max17262_ops),   \
    .i2c = PBL_I2C_DEV(_bus, _addr), .battery = (_battery), .empty_uv = (_empty_uv),   \
    .recovery_uv = (_recovery_uv), .state = &sym##_max17262_state,                     \
  }

/** @} */
