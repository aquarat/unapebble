/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/clk.h>

#include <dt-bindings/clock/st,stm32u5-rcc.h>

/**
 * @defgroup drivers_clk_stm32u5 STM32U5
 * @ingroup drivers_clk
 * @brief @ref drivers_clk controller of the STM32U5 RCC.
 *
 * The oscillators, PLL1, the system clock and the bus clocks are clocks of the controller,
 * parents of the peripheral clocks they run. The peripheral clocks are gated, and those with a
 * kernel clock multiplexer take its current input as their parent. The clocks are numbered as in
 * the binding, dt-bindings/clock/st,stm32u5-rcc.h.
 *
 * Before any clock, the controller selects the core supply, the LDO or the SMPS as the board is
 * wired, and the highest core voltage, range 1 with the EPOD booster, then applies its assigned
 * clocks. The flash wait states follow HCLK: raised before it gets faster, lowered after it got
 * slower. PLL1 is stopped while it does not run the system clock, then configured by the rate of
 * one of its outputs, the other outputs keeping their dividers, and the system clock moves to
 * PLL1 R once PLL1 runs. Everything runs up to 160 MHz.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_clk_controller pbl_clk_stm32u5;
/** @endcond */

/** @brief Clock controller. */
#define STM32U5_RCC (&pbl_clk_stm32u5)

/** @cond INTERNAL_HIDDEN */
#define PBL_CLK_STM32U5_NUM_CLKS (STM32U5_CLK_ICLK + 1)
/** @endcond */

/**
 * @name Supply configurations
 * @{
 */
#define PBL_CLK_STM32U5_SUPPLY_LDO  0U
#define PBL_CLK_STM32U5_SUPPLY_SMPS 1U
/** @} */

/** @brief STM32U5 RCC. */
struct pbl_clk_stm32u5 {
  /** Clock controller. */
  struct pbl_clk_controller ctrl;
  /** Registers. */
  uintptr_t base;
  /** PWR registers. */
  uintptr_t pwr;
  /** Core supply, a @c PBL_CLK_STM32U5_SUPPLY_ value. */
  uint32_t supply;
  /** HSE input. */
  struct pbl_clk hse;
  /** LSE input. */
  struct pbl_clk lse;
  /** The LSE input is an external clock, bypassing the oscillator. */
  bool lse_bypass;
  /** Assigned clocks, ending with an empty entry. */
  const struct pbl_clk_assigned *assigned;
};

/**
 * @brief Read the reset flags.
 *
 * The RCC keeps the causes of the resets since the flags were last cleared.
 *
 * @param ctrl The RCC.
 * @param clear Clear the flags after reading them.
 * @return The CSR register, @c RCC_CSR_ bits of hardware/stm32u5_rcc.h.
 */
uint32_t pbl_clk_stm32u5_reset_flags(const struct pbl_clk_controller *ctrl, bool clear);

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_clk_controller_ops pbl_clk_stm32u5_ops;
int pbl_clk_stm32u5_init(const struct pbl_device *dev);
/** @endcond */

/**
 * @brief Define the RCC.
 *
 * @param sym Symbol of the controller.
 * @param _name Name.
 * @param _base Registers.
 * @param _pwr PWR registers.
 * @param _supply Core supply, a @c PBL_CLK_STM32U5_SUPPLY_ value.
 * @param _hse HSE input, from PBL_CLK().
 * @param _lse LSE input, from PBL_CLK().
 * @param _lse_bypass The LSE input is an external clock.
 * @param ... Assigned clocks, from PBL_CLK_ASSIGNED().
 */
#define PBL_CLK_STM32U5_DEFINE(sym, _name, _base, _pwr, _supply, _hse, _lse, _lse_bypass, ...)    \
  PBL_CLK_CONTROLLER_STATE_DEFINE(sym, PBL_CLK_STM32U5_NUM_CLKS);                                 \
  PBL_CLK_ASSIGNED_DEFINE(sym##_assigned, __VA_ARGS__);                                           \
  PBL_DEVICE_INSTANCE(sym, struct pbl_clk_stm32u5, struct pbl_clk_controller, ctrl) {             \
    .ctrl =                                                                                       \
        PBL_CLK_CONTROLLER_INIT(sym, _name, pbl_clk_stm32u5_init, nullptr, &pbl_clk_stm32u5_ops), \
    .base = (_base), .pwr = (_pwr), .supply = (_supply), .hse = _hse, .lse = _lse,                \
    .lse_bypass = (_lse_bypass), .assigned = sym##_assigned,                                      \
  }

/** @} */
