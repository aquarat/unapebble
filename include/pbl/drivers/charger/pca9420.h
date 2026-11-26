/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

#include <pbl/drivers/battery.h>
#include <pbl/drivers/charger.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/intc.h>
#include <pbl/drivers/pinctrl.h>
#include <pbl/kernel/work.h>

/**
 * @defgroup drivers_charger_pca9420 PCA9420 charger
 * @ingroup drivers_charger
 * @brief @ref drivers_charger driver for the linear charger of the NXP PCA9420 PMIC.
 *
 * The charge current, termination current and charge voltage come from the battery, the
 * voltage capped by CONFIG_CHARGER_PCA9420_CHARGE_LIMIT_MV. The charger starts disabled: its
 * user enables it once it has checked the battery may be charged. It measures nothing: the
 * battery's fuel gauge does. The driver leaves the regulators of the PMIC alone.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
struct pbl_charger_pca9420_data {
  const struct pbl_charger_pca9420 *chg;
  struct pbl_delayed_work debounce_timer;
  // The charge registers hold the configuration
  bool configured;
  // Charging is allowed
  bool enabled;
};

struct pbl_charger_pca9420 {
  struct pbl_charger charger;
  struct pbl_i2c_dev i2c;
  // INTB, shared with the alert of other devices on the line
  struct pbl_irq_spec irq;
  const struct pbl_battery *battery;
  struct pbl_pinctrl pinctrl;
  struct pbl_charger_pca9420_data *data;
};

extern const struct pbl_charger_ops pbl_charger_pca9420_ops;
/** @endcond */

/**
 * @brief Define the charger of a PCA9420.
 *
 * @param sym Symbol of the charger.
 * @param _bus I2C bus.
 * @param _addr 7-bit I2C address.
 * @param _irq Interrupt of INTB, a struct pbl_irq_spec.
 * @param _battery Battery, a struct pbl_battery.
 * @param _pinctrl Default pin state, for the interrupt line, or NULL.
 */
#define PBL_CHARGER_PCA9420_DEFINE(sym, _bus, _addr, _irq, _battery, _pinctrl)                   \
  PBL_CHARGER_STATE_DEFINE(sym);                                                                 \
  static struct pbl_charger_pca9420_data sym##_data;                                             \
  PBL_DEVICE_INSTANCE(sym, struct pbl_charger_pca9420, struct pbl_charger, charger) {            \
    .charger = PBL_CHARGER_INIT(sym, "pca9420_charger", &(_bus)->dev, &pbl_charger_pca9420_ops), \
    .i2c = PBL_I2C_DEV(_bus, _addr), .irq = _irq, .battery = (_battery),                         \
    .pinctrl = PBL_PINCTRL(_pinctrl, nullptr, 0), .data = &sym##_data,                           \
  }

/** @} */
