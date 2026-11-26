/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/device.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/work.h>

/**
 * @defgroup drivers_mmc MMC
 * @ingroup drivers
 * @brief Block device class for an eMMC soldered to the board.
 *
 * An MMC is a struct pbl_mmc device: the user data area of the card, in blocks of
 * @ref PBL_MMC_BLOCK_SIZE bytes. The class powers the card up for the first access and down
 * again once it has been idle for @c CONFIG_MMC_IDLE_OFF_MS, serializes the accesses, and retries
 * a failed one once after power cycling the card. A write returns once the card has programmed
 * the blocks.
 *
 * @code{.c}
 * ret = pbl_mmc_read(MMC, lba, buf, 1U);
 * @endcode
 *
 * Calls must not be issued from ISRs. After pbl_mmc_coredump_init() the class runs without OS
 * services: no locking, no timers and no sleeping, and the card stays powered. That is not
 * reversible.
 * @{
 */

/** @brief Size of a block, in bytes. */
#define PBL_MMC_BLOCK_SIZE 512U

struct pbl_mmc;

/**
 * @brief MMC driver operations.
 *
 * Called with the device lock held, or without any OS service in coredump mode, which drivers
 * check with pbl_mmc_in_coredump(): they must then poll instead of sleeping.
 */
struct pbl_mmc_ops {
  /** Optional. Bring the host up, with the card off. Returns 0 or a negative errno. */
  int (*init)(const struct pbl_mmc *mmc);
  /**
   * Power the card up and initialize it for transfers, giving its number of blocks. Returns 0 or
   * a negative errno, with the card off again.
   */
  int (*power_on)(const struct pbl_mmc *mmc, uint32_t *num_blocks);
  /** Power the card down. */
  void (*power_off)(const struct pbl_mmc *mmc);
  /** Read @p count blocks from @p lba. Returns 0 or a negative errno. */
  int (*read)(const struct pbl_mmc *mmc, uint32_t lba, void *buf, uint32_t count);
  /** Write @p count blocks at @p lba, programmed on return. Returns 0 or a negative errno. */
  int (*write)(const struct pbl_mmc *mmc, uint32_t lba, const void *buf, uint32_t count);
};

/** @cond INTERNAL_HIDDEN */
struct pbl_mmc_state {
  const struct pbl_mmc *mmc;
  struct pbl_mutex lock;
  struct pbl_delayed_work idle;
  bool coredump;
  bool powered;
  uint32_t num_blocks;
};

extern const struct pbl_device_class pbl_mmc_class;
int pbl_mmc_dev_init(const struct pbl_device *dev);
/** @endcond */

/** @brief An MMC. */
struct pbl_mmc {
  /** Device. */
  struct pbl_device dev;
  /** Driver operations. */
  const struct pbl_mmc_ops *ops;
  /** Runtime state. */
  struct pbl_mmc_state *state;
};

/** @brief The board's eMMC. */
extern const struct pbl_mmc *const MMC;

/**
 * @brief Define the class state of MMC @p sym, for the @c PBL_MMC_*_DEFINE() macro of a driver.
 *
 * @param sym Symbol of the MMC instance.
 */
#define PBL_MMC_STATE_DEFINE(sym) \
  PBL_DEVICE_STATE_DEFINE(sym);   \
  static struct pbl_mmc_state sym##_mmc_state

/**
 * @brief Initializer for the struct pbl_mmc of MMC @p sym.
 *
 * @param sym Symbol of the MMC instance, with its state defined by PBL_MMC_STATE_DEFINE().
 * @param _name Name.
 * @param _parent Parent device, or NULL.
 * @param _ops Driver operations.
 */
#define PBL_MMC_INIT(sym, _name, _parent, _ops)                                          \
  {                                                                                      \
    .dev = PBL_DEVICE_INIT_CLASS(sym, _name, pbl_mmc_dev_init, _parent, &pbl_mmc_class), \
    .ops = (_ops),                                                                       \
    .state = &sym##_mmc_state,                                                           \
  }

/**
 * @brief Check whether the MMC runs in coredump mode, for drivers.
 *
 * @param mmc MMC.
 * @return True after pbl_mmc_coredump_init().
 */
static inline bool pbl_mmc_in_coredump(const struct pbl_mmc *mmc) {
  return mmc->state->coredump;
}

/**
 * @brief Get the number of blocks of the card, powering it up if it has not been yet.
 *
 * @param mmc MMC.
 * @param[out] num_blocks Number of blocks.
 * @return 0 or a negative errno.
 */
int pbl_mmc_get_num_blocks(const struct pbl_mmc *mmc, uint32_t *num_blocks);

/**
 * @brief Read blocks.
 *
 * @param mmc MMC.
 * @param lba First block.
 * @param[out] buf Buffer of @p count blocks.
 * @param count Number of blocks.
 * @return 0, @c -EINVAL past the end of the card, or another negative errno.
 */
int pbl_mmc_read(const struct pbl_mmc *mmc, uint32_t lba, void *buf, uint32_t count);

/**
 * @brief Write blocks, returning once the card has programmed them.
 *
 * @param mmc MMC.
 * @param lba First block.
 * @param buf Data of @p count blocks.
 * @param count Number of blocks.
 * @return 0, @c -EINVAL past the end of the card, or another negative errno.
 */
int pbl_mmc_write(const struct pbl_mmc *mmc, uint32_t lba, const void *buf, uint32_t count);

/**
 * @brief Power the card down now; the next access powers it up again.
 *
 * @param mmc MMC.
 */
void pbl_mmc_power_off(const struct pbl_mmc *mmc);

/**
 * @brief Re-initialize the MMC for use from a fault handler.
 *
 * @param mmc MMC.
 * @return 0 or a negative errno.
 */
int pbl_mmc_coredump_init(const struct pbl_mmc *mmc);

/** @} */
