/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/vibe.h>

/**
 * @defgroup drivers_vibe_drv2625 DRV2625
 * @ingroup drivers_vibe
 * @brief TI DRV2625 @ref drivers_vibe driver, driving an ERM in real-time playback mode.
 *
 * The ERM runs open loop, at a strength out of the overdrive clamp voltage, so it needs no
 * calibration; it brakes into standby when stopped. The ERM library of the waveform sequencer is
 * selected, as the UNA Watch firmware does, though playback only uses the real-time input. The
 * enable line stays high once initialized: the part keeps its registers and idles in standby.
 *
 * @code{.c}
 * PBL_DRV2625_DEFINE(s_drv2625, &s_i2c1, 0x5A, PBL_GPIO(&dt_gpiof, 2, 0), 123, 137);
 *
 * const struct pbl_vibe *const VIBE = &s_drv2625;
 * @endcode
 * @{
 */

/** @brief DRV2625 haptic driver. */
struct pbl_drv2625 {
  /** Vibration motor, child of the I2C bus. */
  struct pbl_vibe vibe;
  /** I2C device. */
  struct pbl_i2c_dev i2c;
  /** Enable line. */
  struct pbl_gpio enable;
  /** RATED_VOLTAGE register. */
  uint8_t rated_voltage;
  /** OD_CLAMP register. */
  uint8_t od_clamp;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_vibe_ops pbl_drv2625_ops;
/** @endcond */

/**
 * @brief Define a DRV2625.
 *
 * @param sym Symbol of the instance, a struct pbl_vibe; the motor is @c sym.vibe.
 * @param _bus I2C bus.
 * @param _addr 7-bit I2C address.
 * @param _enable Enable line (struct pbl_gpio).
 * @param _rated_voltage RATED_VOLTAGE register, the closed-loop average voltage.
 * @param _od_clamp OD_CLAMP register, the open-loop average voltage at full strength.
 */
#define PBL_DRV2625_DEFINE(sym, _bus, _addr, _enable, _rated_voltage, _od_clamp)              \
  PBL_VIBE_STATE_DEFINE(sym);                                                                 \
  PBL_DEVICE_INSTANCE(sym, struct pbl_drv2625, struct pbl_vibe, vibe) {                       \
    .vibe = PBL_VIBE_INIT(sym, "drv2625", &(_bus)->dev, &pbl_drv2625_ops, VIBE_STRENGTH_OFF), \
    .i2c = PBL_I2C_DEV(_bus, _addr), .enable = _enable, .rated_voltage = (_rated_voltage),    \
    .od_clamp = (_od_clamp),                                                                  \
  }

/** @} */
