/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "blk_nor.h"

#include <errno.h>
#include <inttypes.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/devicetree.h>
#include <pbl/drivers/flash/mmc_nor.h>
#include <pbl/logging/logging.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DECLARE(driver_flash, CONFIG_DRIVER_FLASH_LOG_LEVEL);

static_assert(BLK_NOR_BLOCK_SIZE == PBL_MMC_BLOCK_SIZE, "block sizes differ");

static const struct pbl_flash_mmc_nor *prv_mn(const struct pbl_flash *flash) {
  return PBL_CONTAINER_OF(flash, const struct pbl_flash_mmc_nor, flash);
}

static int prv_hw_read(void *ctx, uint32_t block, void *buf, uint32_t count) {
  const struct pbl_flash_mmc_nor *mn = ctx;

  return pbl_mmc_read(mn->mmc, mn->first_block + block, buf, count);
}

static int prv_hw_write(void *ctx, uint32_t block, const void *buf, uint32_t count) {
  const struct pbl_flash_mmc_nor *mn = ctx;

  return pbl_mmc_write(mn->mmc, mn->first_block + block, buf, count);
}

static const struct blk_nor_hw s_hw = {
  .read = prv_hw_read,
  .write = prv_hw_write,
};

static int prv_init(const struct pbl_flash *flash) {
  const struct pbl_flash_mmc_nor *mn = prv_mn(flash);
  const uint32_t blocks = flash->geometry->size / PBL_MMC_BLOCK_SIZE;
  uint32_t num_blocks;
  int ret;

  // What the interrupted thread had cached may not be what the card holds
  if (pbl_flash_in_coredump(flash)) {
    blk_nor_invalidate(mn->nor);
    return pbl_mmc_coredump_init(mn->mmc);
  }

  if (!pbl_device_is_ready(&mn->mmc->dev)) {
    return -ENODEV;
  }

  PBL_BOOT_TRACE(PBL_BT_NOR, PBL_BT_NOR_INIT);
  ret = pbl_mmc_get_num_blocks(mn->mmc, &num_blocks);
  if (ret != 0) {
    PBL_LOG_ERR("No card: %d", ret);
    PBL_BOOT_TRACE(PBL_BT_NOR, PBL_BT_NOR_FAILED);
    return ret;
  }

  if (mn->first_block > num_blocks || blocks > num_blocks - mn->first_block) {
    PBL_LOG_ERR("Window past the %" PRIu32 " blocks of the card", num_blocks);
    return -EINVAL;
  }

  blk_nor_init(mn->nor, &s_hw, (void *)mn);
  PBL_BOOT_TRACE(PBL_BT_NOR, PBL_BT_NOR_READY);

  return 0;
}

static int prv_read(const struct pbl_flash *flash, uint32_t addr, void *buf, size_t len) {
  return blk_nor_read(prv_mn(flash)->nor, addr, buf, len);
}

static int prv_write(const struct pbl_flash *flash, uint32_t addr, const void *buf, size_t len) {
  return blk_nor_write(prv_mn(flash)->nor, addr, buf, len);
}

static int prv_erase(const struct pbl_flash *flash, uint32_t addr, uint32_t size) {
  PBL_BOOT_TRACE(PBL_BT_NOR, PBL_BT_NOR_ERASE | ((addr >> 12) & 0xFFFFU));
  int ret = blk_nor_erase(prv_mn(flash)->nor, addr, size);

  return (ret == 0) ? 1 : ret;
}

const struct pbl_flash_ops pbl_flash_mmc_nor_ops = {
  .init = prv_init,
  .read = prv_read,
  .write = prv_write,
  .erase = prv_erase,
};

#include PBL_DT_INSTANCES(pebble_mmc_nor)
