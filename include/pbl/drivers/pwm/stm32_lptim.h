/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/clk.h>
#include <pbl/drivers/pinctrl.h>
#include <pbl/drivers/pwm.h>

/**
 * @defgroup drivers_pwm_stm32_lptim STM32 LPTIM PWM
 * @ingroup drivers_pwm
 * @brief @ref drivers_pwm controller driver for the low-power timers of the STM32U5.
 *
 * The two channels of LPTIM1, LPTIM2 or LPTIM3, which share the period. The resolution is one
 * cycle of the counter clock: on the LSE, about 30.5 us, and the timer keeps counting in Stop 2
 * when the clock controller lets it. LPTIM4, whose output takes its polarity elsewhere, is not
 * supported.
 *
 * @code{.c}
 * PBL_PWM_STM32_LPTIM_DEFINE(s_lptim1_pwm, "lptim1_pwm", 0x46004400,
 *                            PBL_CLK(STM32U5_RCC, STM32U5_CLK_LPTIM1), &s_lptim1_pwm_default);
 * @endcode
 * @{
 */

/** @brief STM32 LPTIM PWM controller. */
struct pbl_pwm_stm32_lptim {
  /** PWM controller. */
  struct pbl_pwm_controller ctrl;
  /** Output pins. */
  struct pbl_pinctrl pinctrl;
  /** Timer registers. */
  uintptr_t base;
  /** Counter clock. */
  struct pbl_clk clk;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_pwm_controller_ops pbl_pwm_stm32_lptim_ops;
int pbl_pwm_stm32_lptim_init(const struct pbl_device *dev);
/** @endcond */

/**
 * @brief Define an STM32 LPTIM PWM controller.
 *
 * @param sym Symbol of the controller.
 * @param _name Name.
 * @param _base Timer registers.
 * @param _clk Counter clock, from PBL_CLK().
 * @param _pinctrl Pin state with the outputs, from PBL_PINCTRL_STATE_DEFINE().
 */
#define PBL_PWM_STM32_LPTIM_DEFINE(sym, _name, _base, _clk, _pinctrl)                     \
  PBL_PWM_CONTROLLER_STATE_DEFINE(sym, 2);                                                \
  PBL_DEVICE_INSTANCE(sym, struct pbl_pwm_stm32_lptim, struct pbl_pwm_controller, ctrl) { \
    .ctrl = PBL_PWM_CONTROLLER_INIT(sym, _name, pbl_pwm_stm32_lptim_init, nullptr,        \
                                    &pbl_pwm_stm32_lptim_ops),                            \
    .pinctrl = PBL_PINCTRL(_pinctrl, nullptr, 0), .base = (_base), .clk = _clk,           \
  }

/** @} */
