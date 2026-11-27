/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/flash.h>
#include <pbl/kernel/work.h>
#include <pbl/util/units.h>

/**
 * @defgroup drivers_flash_stm32u5 STM32U5
 * @ingroup drivers_flash
 * @brief @ref drivers_flash driver for the embedded flash of the STM32U5.
 *
 * The flash of the part, from @c CONFIG_FLASH_BASE, @c CONFIG_FLASH_SIZE bytes in two banks of
 * 8 KiB pages, with partitions as offsets from its first byte. The CPU executes from the first
 * bank, so only the second one is written or erased: the bank the CPU reads keeps running while
 * the other one programs. The size must match the part's, read at init.
 *
 * A word of 16 bytes can only be programmed once after an erase, while the class programs bytes
 * and clears bits of bytes programmed before: the driver caches recent writes in RAM and
 * programs words once whole, as drivers/flash/ecc_nor.h describes, and emulates 4 KiB
 * subsectors by erasing a page and programming its other half again. pbl_flash_stop() and
 * pbl_flash_coredump_init() program what is cached; a reset otherwise loses the last second of
 * writes.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
struct ecc_nor;

struct pbl_flash_stm32u5_state {
  const struct pbl_flash *flash;
  struct pbl_delayed_work flush;
};
/** @endcond */

/** @brief STM32U5 embedded flash. */
struct pbl_flash_stm32u5 {
  /** Flash. */
  struct pbl_flash flash;
  /** Registers of the flash controller. */
  uintptr_t base;
  /** Write cache. */
  struct ecc_nor *nor;
  /** Driver state. */
  struct pbl_flash_stm32u5_state *state;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_flash_ops pbl_flash_stm32u5_ops;
extern struct ecc_nor pbl_flash_stm32u5_nor;
/** @endcond */

/**
 * @brief Define the embedded flash.
 *
 * @param sym Symbol of the flash.
 * @param _name Name.
 * @param _base Registers of the flash controller.
 */
#define PBL_FLASH_STM32U5_DEFINE(sym, _name, _base)                                         \
  PBL_FLASH_STATE_DEFINE(sym);                                                              \
  static struct pbl_flash_stm32u5_state sym##_stm32u5_state;                                \
  static const struct pbl_flash_geometry sym##_geometry = {                                 \
    .size = CONFIG_FLASH_SIZE,                                                              \
    .page_size = 16U,                                                                       \
    .sector_size = PBL_KIB(64),                                                             \
    .subsector_size = PBL_KIB(4),                                                           \
    .sector_erase_ms = 27U,                                                                 \
    .subsector_erase_ms = 4U,                                                               \
  };                                                                                        \
  PBL_DEVICE_INSTANCE(sym, struct pbl_flash_stm32u5, struct pbl_flash, flash) {             \
    .flash = PBL_FLASH_INIT(sym, _name, nullptr, &pbl_flash_stm32u5_ops, &sym##_geometry,   \
                            nullptr, nullptr),                                              \
    .base = (_base), .nor = &pbl_flash_stm32u5_nor, .state = &sym##_stm32u5_state,          \
  }

/** @} */
