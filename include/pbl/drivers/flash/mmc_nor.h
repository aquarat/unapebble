/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/flash.h>
#include <pbl/drivers/mmc.h>
#include <pbl/util/units.h>

/**
 * @defgroup drivers_flash_mmc_nor MMC NOR window
 * @ingroup drivers_flash
 * @brief @ref drivers_flash driver for a range of an @ref drivers_mmc, as a NOR flash.
 *
 * The flash is @c size bytes of the card from byte @c offset, both whole blocks, with
 * partitions as offsets from its first byte, in 4 KiB subsectors and 64 KiB sectors. Writes
 * read, change and write back whole blocks, and erases write blocks of 0xFF, before returning,
 * as drivers/flash/blk_nor.h describes: nothing is cached that a reset would lose. The card
 * levels its wear itself.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
struct blk_nor;
/** @endcond */

/** @brief NOR window on an MMC. */
struct pbl_flash_mmc_nor {
  /** Flash. */
  struct pbl_flash flash;
  /** MMC the window is on. */
  const struct pbl_mmc *mmc;
  /** First block of the window on the card. */
  uint32_t first_block;
  /** NOR emulation and its cache. */
  struct blk_nor *nor;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_flash_ops pbl_flash_mmc_nor_ops;
/** @endcond */

/**
 * @brief Define a NOR window on an MMC.
 *
 * @param sym Symbol of the flash.
 * @param _name Name.
 * @param _mmc MMC the window is on.
 * @param _offset Offset of the window on the card, in bytes, a whole number of blocks.
 * @param _size Size of the window, in bytes, a whole number of 64 KiB sectors.
 */
#define PBL_FLASH_MMC_NOR_DEFINE(sym, _name, _mmc, _offset, _size)                             \
  static_assert(((_offset) % PBL_MMC_BLOCK_SIZE) == 0U, "window not on a block");              \
  static_assert(((_size) % PBL_KIB(64)) == 0U, "window not in whole sectors");                 \
  PBL_FLASH_STATE_DEFINE(sym);                                                                 \
  static struct blk_nor sym##_nor;                                                             \
  static const struct pbl_flash_geometry sym##_geometry = {                                    \
    .size = (_size),                                                                           \
    .page_size = PBL_MMC_BLOCK_SIZE,                                                           \
    .sector_size = PBL_KIB(64),                                                                \
    .subsector_size = PBL_KIB(4),                                                              \
    .sector_erase_ms = 20U,                                                                    \
    .subsector_erase_ms = 2U,                                                                  \
  };                                                                                           \
  PBL_DEVICE_INSTANCE(sym, struct pbl_flash_mmc_nor, struct pbl_flash, flash) {                \
    .flash = PBL_FLASH_INIT(sym, _name, &(_mmc)->dev, &pbl_flash_mmc_nor_ops, &sym##_geometry, \
                            nullptr, nullptr),                                                 \
    .mmc = (_mmc), .first_block = (_offset) / PBL_MMC_BLOCK_SIZE, .nor = &sym##_nor,           \
  }

/** @} */
