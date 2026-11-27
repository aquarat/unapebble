/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/accel.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/intc.h>
#include <pbl/kernel/cache.h>
#include <pbl/kernel/compiler.h>
#include <pbl/kernel/work.h>

/**
 * @defgroup drivers_imu_bmi270 BMI270
 * @ingroup drivers_imu
 * @brief Bosch BMI270 @ref drivers_accel driver.
 *
 * The accelerometer only, the gyroscope off, in low power mode. Samples go through the FIFO, read
 * at its watermark interrupt; shakes are the feature engine's any-motion, on the same interrupt
 * pin. The driver loads Bosch's configuration file into the part at init.
 *
 * @code{.c}
 * PBL_BMI270_DEFINE(s_bmi270, &s_i2c4, 0x68,
 *                   .irq = ...,
 *                   .irq_in = ...,
 *                   .int_pin = 2,
 *                   .axis_map = {1, 0, 2},
 *                   .axis_dir = {1, -1, 1});
 *
 * const struct pbl_accel *const ACCEL = &s_bmi270;
 * @endcode
 * @{
 */

/** @brief Samples the driver batches at most, and reads from the FIFO at once. */
#define BMI270_SAMPLES_MAX 128

/** @brief BMI270 driver statistics, since boot. */
struct pbl_bmi270_stats {
  /** Interrupts taken. */
  uint32_t irqs;
  /** FIFO reads that brought samples. */
  uint32_t fifo_reads;
  /** Samples reported. */
  uint32_t samples;
  /** FIFO overruns, where samples were lost. */
  uint32_t overruns;
  /** Shakes reported. */
  uint32_t shakes;
  /** Services by the watchdog, which found the interrupt late. */
  uint32_t watchdog;
  /** Bus errors. */
  uint32_t errors;
};

/** @brief BMI270 driver state, owned by the driver. */
struct pbl_bmi270_state {
  /** Watch mounted rotated 180 degrees: X and Y are inverted. */
  bool rotated;
  /** Shake detection requested. */
  bool shake;
  /** High shake sensitivity requested. */
  bool shake_high;
  /** Advanced power save on, which limits writes. */
  bool aps;
  /** Normal shake sensitivity, 0 to 100. */
  uint8_t shake_percent;
  /** ODR code of the FIFO's samples. */
  uint8_t fifo_odr;
  /** Accelerometer ODR code in use, 0 when off. */
  uint8_t odr;
  /** FIFO down-sampling in use, as a power of 2. */
  uint8_t downs;
  /** Samples to batch, 0 when not streaming. */
  uint16_t num_samples;
  /** Last sample, in milli-g on the watch axes, for a shake's axis. */
  int16_t last[3];
  /** Uptime of the last FIFO service, in milliseconds. */
  uint64_t last_service_ms;
  /** Interrupt servicing. */
  struct pbl_work irq_work;
  /** Watchdog, servicing the FIFO when its interrupt is late. */
  struct pbl_delayed_work watchdog;
  /** Statistics. */
  struct pbl_bmi270_stats stats;
  /** FIFO read buffer. */
  uint8_t buf[PBL_DCACHE_ROUND_UP(BMI270_SAMPLES_MAX * 6)] PBL_ALIGNED(PBL_DCACHE_LINE_SIZE);
};

/** @brief BMI270 accelerometer. */
struct pbl_bmi270 {
  /** Accelerometer, child of the I2C bus. */
  struct pbl_accel accel;
  /** Driver state. */
  struct pbl_bmi270_state *state;
  /** I2C device. */
  struct pbl_i2c_dev i2c;
  /** Interrupt line. */
  struct pbl_irq_spec irq;
  /** Interrupt input, to read back the pad level. */
  struct pbl_gpio irq_in;
  /** The part's interrupt pin wired to it: 1 or 2. */
  uint8_t int_pin;
  /** Sensor axis feeding each watch axis (0: X, 1: Y, 2: Z). */
  uint8_t axis_map[3];
  /** Direction of each watch axis: 1 or -1. */
  int8_t axis_dir[3];
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_accel_ops pbl_bmi270_ops;
/** @endcond */

/**
 * @brief Get the driver statistics of the BMI270.
 *
 * @param accel The BMI270's accelerometer.
 * @param[out] stats Statistics.
 */
void pbl_bmi270_get_stats(const struct pbl_accel *accel, struct pbl_bmi270_stats *stats);

/**
 * @brief Define a BMI270.
 *
 * Only one instance is supported.
 *
 * @param sym Symbol of the instance, a struct pbl_accel.
 * @param _bus I2C bus.
 * @param _addr 7-bit I2C address.
 * @param ... Designated initializers of the remaining fields: @c irq, @c irq_in, @c int_pin,
 *            @c axis_map and @c axis_dir.
 */
#define PBL_BMI270_DEFINE(sym, _bus, _addr, ...)                                \
  PBL_ACCEL_STATE_DEFINE(sym);                                                  \
  static struct pbl_bmi270_state sym##_bmi270_state;                            \
  PBL_DEVICE_INSTANCE(sym, struct pbl_bmi270, struct pbl_accel, accel) {        \
    .accel = PBL_ACCEL_INIT(sym, "bmi270", &(_bus)->dev, &pbl_bmi270_ops),      \
    .state = &sym##_bmi270_state, .i2c = PBL_I2C_DEV(_bus, _addr), __VA_ARGS__, \
  }

/** @} */
