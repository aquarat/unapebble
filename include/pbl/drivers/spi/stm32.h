/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/clk.h>
#include <pbl/drivers/pinctrl.h>
#include <pbl/drivers/spi.h>
#include <pbl/kernel/irq.h>

/**
 * @defgroup drivers_spi_stm32 STM32
 * @ingroup drivers_spi
 * @brief @ref drivers_spi bus driver for the SPI of the STM32H7 and later, as on the STM32U5.
 *
 * A controller master, 8-bit frames, with software slave management: chip selects are GPIOs.
 * Transfers of up to 65535 bytes are moved a byte at a time from the interrupt, through the FIFO;
 * no DMA. The clock is the kernel clock divided by a power of two, 2 to 256, the highest at or
 * below the device's maximum. The controller keeps its pins at their idle levels between
 * transfers (AFCNTR); with a sleep pin state they are put in it while the bus is disabled
 * instead. The CPU stays out of Stop during a transfer, as the kernel clock stops there.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
struct pbl_spi_stm32_state {
  const struct pbl_spi_xfer *xfer;
  uint32_t tx;
  uint32_t rx;
  uint32_t cfg1;
  uint32_t cfg2;
};
/** @endcond */

/** @brief STM32 SPI bus. */
struct pbl_spi_stm32 {
  /** SPI bus. */
  struct pbl_spi_bus bus;
  /** Driver runtime state. */
  struct pbl_spi_stm32_state *state;
  /** Registers. */
  uintptr_t base;
  /** Interrupt line. */
  pbl_irq_t irq;
  /** Kernel clock, from PBL_CLK(). */
  struct pbl_clk clk;
  /** Pins: their default state, and optionally a sleep one. */
  struct pbl_pinctrl pinctrl;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_spi_bus_ops pbl_spi_stm32_ops;
/** @endcond */

/**
 * @brief Interrupt handler, connected by PBL_SPI_STM32_DEFINE().
 *
 * @param spi Bus.
 */
void pbl_spi_stm32_irq_handler(const struct pbl_spi_stm32 *spi);

/**
 * @brief Define a bus on an STM32 SPI controller.
 *
 * @param sym Symbol of the bus, a struct pbl_spi_bus.
 * @param _name Name.
 * @param _base Registers.
 * @param _irq Interrupt line, a plain number.
 * @param _default Default pin state.
 * @param _sleep Sleep pin state, or NULL.
 * @param _clk Kernel clock, from PBL_CLK().
 * @param ... Chip select pins, from PBL_GPIO(), by chip select number.
 */
#define PBL_SPI_STM32_DEFINE(sym, _name, _base, _irq, _default, _sleep, _clk, ...) \
  PBL_SPI_BUS_STATE_DEFINE(sym, __VA_ARGS__);                                      \
  static struct pbl_spi_stm32_state sym##_stm32_state;                             \
  PBL_DEVICE_INSTANCE(sym, struct pbl_spi_stm32, struct pbl_spi_bus, bus){         \
    .bus = PBL_SPI_BUS_INIT(sym, _name, nullptr, &pbl_spi_stm32_ops),              \
    .state = &sym##_stm32_state,                                                   \
    .base = (_base),                                                               \
    .irq = (_irq),                                                                 \
    .clk = _clk,                                                                   \
    .pinctrl = PBL_PINCTRL(_default, _sleep, 0),                                   \
  };                                                                               \
  PBL_IRQ_CONNECT_NUM(_irq, CONFIG_SPI_STM32_IRQ_PRIO, pbl_spi_stm32_irq_handler, &sym##_inst, 0)

/** @} */
