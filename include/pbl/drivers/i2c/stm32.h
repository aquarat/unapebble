/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/clk.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/pinctrl.h>
#include <pbl/kernel/irq.h>

/**
 * @defgroup drivers_i2c_stm32 STM32
 * @ingroup drivers_i2c
 * @brief @ref drivers_i2c driver for the I2C controller of the STM32F7 and later, as on the
 * STM32U5.
 *
 * Transfers are moved a byte at a time from the interrupts, in chunks of up to 255 bytes, a
 * repeated START between messages and a STOP after the last. The bus timing comes from the
 * kernel clock and the bus rate, with the minimum low and high times of its mode; as the rise and
 * fall times are left out, the bus runs somewhat below the rate.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
struct pbl_i2c_stm32_state {
  uint32_t timingr;
  struct pbl_i2c_msg *msgs;
  uint8_t num_msgs;
  uint8_t idx;
  uint32_t pos;
  uint16_t addr;
  // Bytes left in the current START and in its chunk
  uint32_t seg_left;
  uint32_t chunk_left;
  bool seg_moved;
  int error;
};
/** @endcond */

/** @brief STM32 I2C controller. */
struct pbl_i2c_stm32 {
  /** I2C bus. */
  struct pbl_i2c_bus bus;
  /** Driver runtime state. */
  struct pbl_i2c_stm32_state *state;
  /** Registers. */
  uintptr_t base;
  /** Event interrupt. */
  pbl_irq_t irq_ev;
  /** Error interrupt. */
  pbl_irq_t irq_er;
  /** Kernel clock, from PBL_CLK(). */
  struct pbl_clk clk;
  /** Bus clock frequency, in Hz, up to 1 MHz. */
  uint32_t clock_hz;
  /** Pins. */
  struct pbl_pinctrl pinctrl;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_i2c_bus_ops pbl_i2c_stm32_ops;
/** @endcond */

/**
 * @brief Interrupt handler of both interrupts, connected by PBL_I2C_STM32_DEFINE().
 *
 * @param i2c Bus.
 */
void pbl_i2c_stm32_irq_handler(const struct pbl_i2c_stm32 *i2c);

/**
 * @brief Define a bus on an STM32 I2C controller.
 *
 * @param sym Symbol of the bus, a struct pbl_i2c_bus.
 * @param _name Name.
 * @param _base Registers.
 * @param _irq_ev Event interrupt line, a plain number.
 * @param _irq_er Error interrupt line, a plain number.
 * @param _clock_hz Bus clock frequency, up to 1000000.
 * @param _pinctrl Default pin state.
 * @param _clk Kernel clock, from PBL_CLK().
 */
#define PBL_I2C_STM32_DEFINE(sym, _name, _base, _irq_ev, _irq_er, _clock_hz, _pinctrl, _clk)      \
  PBL_I2C_BUS_STATE_DEFINE(sym);                                                                  \
  static struct pbl_i2c_stm32_state sym##_stm32_state;                                            \
  PBL_DEVICE_INSTANCE(sym, struct pbl_i2c_stm32, struct pbl_i2c_bus, bus){                        \
    .bus = PBL_I2C_BUS_INIT(sym, _name, nullptr, &pbl_i2c_stm32_ops),                             \
    .state = &sym##_stm32_state,                                                                  \
    .base = (_base),                                                                              \
    .irq_ev = (_irq_ev),                                                                          \
    .irq_er = (_irq_er),                                                                          \
    .clk = _clk,                                                                                  \
    .clock_hz = (_clock_hz),                                                                      \
    .pinctrl = PBL_PINCTRL(_pinctrl, nullptr, 0),                                                 \
  };                                                                                              \
  PBL_IRQ_CONNECT_NUM(_irq_ev, CONFIG_I2C_STM32_IRQ_PRIO, pbl_i2c_stm32_irq_handler, &sym##_inst, \
                      0);                                                                         \
  PBL_IRQ_CONNECT_NUM(_irq_er, CONFIG_I2C_STM32_IRQ_PRIO, pbl_i2c_stm32_irq_handler, &sym##_inst, 0)

/** @} */
