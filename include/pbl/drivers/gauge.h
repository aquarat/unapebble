/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/device.h>
#include <pbl/kernel/mutex.h>

/**
 * @defgroup drivers_gauge Fuel gauge
 * @ingroup drivers
 * @brief Battery fuel gauge class.
 *
 * A fuel gauge is a chip that measures the battery and estimates its state of charge itself, as
 * a struct pbl_gauge device. Every call into the driver runs with the device lock held.
 *
 * @code{.c}
 * struct pbl_gauge_reading r;
 *
 * if (pbl_gauge_read(GAUGE, &r) == 0) {
 *   // r.soc_cpct, r.voltage_mv, r.current_ua, r.temp_mc
 * }
 * @endcode
 * @{
 */

struct pbl_gauge;

/** @brief A reading of the gauge. */
struct pbl_gauge_reading {
  /** State of charge in hundredths of a percent, 0 to 10000. */
  uint16_t soc_cpct;
  /** Voltage in mV. */
  int32_t voltage_mv;
  /** Averaged voltage in mV. */
  int32_t avg_voltage_mv;
  /** Current in uA, positive while discharging. */
  int32_t current_ua;
  /** Temperature in millidegrees Celsius. */
  int32_t temp_mc;
  /** Averaged current in uA, positive while discharging. */
  int32_t avg_current_ua;
  /** Remaining capacity in uAh, 0 if not known. */
  uint32_t remaining_uah;
  /** Full capacity, as learned and reported, in uAh, 0 if not known. */
  uint32_t full_uah;
  /** Nominal full capacity, as learned, in uAh, 0 if not known. */
  uint32_t full_nominal_uah;
  /** Design capacity in uAh, 0 if not known. */
  uint32_t design_uah;
  /** Charge cycles counted, in percent of a cycle. */
  uint16_t cycles_pct;
  /** Coulomb counter: rises with charge, falls with discharge, wraps around. */
  uint16_t counter;
  /** uAh a count of @c counter, 0 if the gauge has no counter. */
  uint16_t counter_uah;
  /** Resets of the gauge since boot: @c counter starts again at each. */
  uint8_t resets;
};

/** @brief Fuel gauge driver operations, called with the device lock held. */
struct pbl_gauge_ops {
  /** Optional. Bring the hardware up. */
  int (*init)(const struct pbl_gauge *gauge);
  /** Read the gauge. */
  int (*read)(const struct pbl_gauge *gauge, struct pbl_gauge_reading *reading);
};

/** @brief Fuel gauge runtime state. */
struct pbl_gauge_state {
  /** Device lock. */
  struct pbl_mutex lock;
};

/** @brief Fuel gauge. */
struct pbl_gauge {
  /** Device. */
  struct pbl_device dev;
  /** Driver operations. */
  const struct pbl_gauge_ops *ops;
  /** Runtime state. */
  struct pbl_gauge_state *state;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_device_class pbl_gauge_class;
int pbl_gauge_dev_init(const struct pbl_device *dev);
/** @endcond */

/** @brief The board's fuel gauge. */
extern const struct pbl_gauge *const GAUGE;

/**
 * @brief Define the class state of a fuel gauge.
 *
 * @param sym Symbol of the instance.
 */
#define PBL_GAUGE_STATE_DEFINE(sym) \
  PBL_DEVICE_STATE_DEFINE(sym);     \
  static struct pbl_gauge_state sym##_gauge_state = {.lock = PBL_MUTEX_INITIALIZER}

/**
 * @brief Initialize a struct pbl_gauge.
 *
 * @param sym Symbol of the instance, as passed to PBL_GAUGE_STATE_DEFINE().
 * @param _name Device name.
 * @param _parent Parent device, or NULL.
 * @param _ops Driver operations.
 */
#define PBL_GAUGE_INIT(sym, _name, _parent, _ops)                                            \
  {                                                                                          \
    .dev = PBL_DEVICE_INIT_CLASS(sym, _name, pbl_gauge_dev_init, _parent, &pbl_gauge_class), \
    .ops = (_ops),                                                                           \
    .state = &sym##_gauge_state,                                                             \
  }

/**
 * @brief Read the state of charge and the battery measurements.
 *
 * @param gauge Fuel gauge.
 * @param[out] reading Reading, left as it was on error.
 * @return 0, @c -ENODEV if the gauge is not ready, or a negative errno.
 */
int pbl_gauge_read(const struct pbl_gauge *gauge, struct pbl_gauge_reading *reading);

/** @} */
