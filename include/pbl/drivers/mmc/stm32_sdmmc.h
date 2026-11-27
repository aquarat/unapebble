/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/clk.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/mmc.h>
#include <pbl/drivers/pinctrl.h>
#include <pbl/drivers/reset.h>

/**
 * @defgroup drivers_mmc_stm32_sdmmc STM32 SDMMC
 * @ingroup drivers_mmc
 * @brief @ref drivers_mmc driver for an eMMC on the SDMMC host of the STM32U5.
 *
 * The card is powered by a load switch on @c enable-gpios, given @c post-power-on-delay-ms to
 * come up, then identified at 400 kHz on one data line and switched to four, and to High Speed
 * if it supports it, up to @c max-frequency. Only cards addressed in sectors, of more than 2 GB,
 * are supported, as every eMMC from version 4.2 on is.
 *
 * Transfers are polled, sleeping while the card is busy, and their data is moved by the internal
 * DMA of the host, from and to SRAM only: other buffers, or unaligned ones, go through a block
 * of the driver's. Writes are reliable writes, which a power loss leaves either done or not, if
 * the card supports the enhanced reliable write; otherwise only single blocks are written so. The
 * CPU is kept out of Stop mode, where the host's clock stops, during each access.
 *
 * Powered down, the host is off and its pins analog (the @c sleep pin state), so that their
 * pull-ups do not feed the card.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
struct pbl_mmc_stm32_sdmmc_state {
  uint32_t ker_hz;
  uint64_t off_since_ms;
  bool reliable;
  bool no_hs;
  uint8_t rel_sectors;
  uint8_t cmd6_time_10ms;
  uint8_t buf[PBL_MMC_BLOCK_SIZE] __attribute__((aligned(4)));
};
/** @endcond */

/** @brief STM32 SDMMC host with an eMMC. */
struct pbl_mmc_stm32_sdmmc {
  /** MMC. */
  struct pbl_mmc mmc;
  /** Driver state. */
  struct pbl_mmc_stm32_sdmmc_state *state;
  /** Registers. */
  uintptr_t base;
  /** Bus and kernel clock, from PBL_CLK(). */
  struct pbl_clk clk;
  /** Reset line. */
  struct pbl_reset rst;
  /** Pins, with the sleep state of the card powered down. */
  struct pbl_pinctrl pinctrl;
  /** Enable of the card's supply. */
  struct pbl_gpio enable;
  /** Time the card takes to come up once powered, in milliseconds. */
  uint32_t power_on_delay_ms;
  /** Highest bus clock, in Hz. */
  uint32_t max_hz;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_mmc_ops pbl_mmc_stm32_sdmmc_ops;
/** @endcond */

/**
 * @brief Define an eMMC on an STM32 SDMMC host.
 *
 * @param sym Symbol of the MMC.
 * @param _name Name.
 * @param _base Registers.
 * @param _clk Bus and kernel clock, from PBL_CLK().
 * @param _rst Reset line, from PBL_RESET().
 * @param _default Default pin state.
 * @param _sleep Pin state of the card powered down.
 * @param _enable Enable of the card's supply, from PBL_GPIO().
 * @param _power_on_delay_ms Time the card takes to come up once powered.
 * @param _max_hz Highest bus clock.
 */
#define PBL_MMC_STM32_SDMMC_DEFINE(sym, _name, _base, _clk, _rst, _default, _sleep, _enable, \
                                   _power_on_delay_ms, _max_hz)                              \
  PBL_MMC_STATE_DEFINE(sym);                                                                 \
  static struct pbl_mmc_stm32_sdmmc_state sym##_sdmmc_state;                                 \
  PBL_DEVICE_INSTANCE(sym, struct pbl_mmc_stm32_sdmmc, struct pbl_mmc, mmc) {                \
    .mmc = PBL_MMC_INIT(sym, _name, nullptr, &pbl_mmc_stm32_sdmmc_ops),                      \
    .state = &sym##_sdmmc_state, .base = (_base), .clk = _clk, .rst = _rst,                  \
    .pinctrl = PBL_PINCTRL(_default, _sleep, 0), .enable = _enable,                          \
    .power_on_delay_ms = (_power_on_delay_ms), .max_hz = (_max_hz),                          \
  }

/** @} */
