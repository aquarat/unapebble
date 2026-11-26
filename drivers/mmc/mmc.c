/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/drivers/mmc.h>
#include <pbl/kernel/reboot.h>
#include <pbl/logging/logging.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DEFINE(driver_mmc, CONFIG_DRIVER_MMC_LOG_LEVEL);

PBL_DEVICE_CLASS_DEFINE(pbl_mmc_class, "mmc");

// Power-ups of a card that does not answer, each after a power cycle
#define POWER_ON_TRIES 3U

static void prv_lock(const struct pbl_mmc *mmc) {
  if (!mmc->state->coredump) {
    pbl_mutex_lock(&mmc->state->lock, PBL_FOREVER);
  }
}

static void prv_unlock(const struct pbl_mmc *mmc) {
  if (!mmc->state->coredump) {
    pbl_mutex_unlock(&mmc->state->lock);
  }
}

static void prv_power_off(const struct pbl_mmc *mmc) {
  if (mmc->state->powered) {
    mmc->ops->power_off(mmc);
    mmc->state->powered = false;
    PBL_LOG_DBG("Card off");
  }
}

static int prv_power_on(const struct pbl_mmc *mmc) {
  struct pbl_mmc_state *st = mmc->state;
  uint32_t num_blocks = 0U;
  int ret = -EIO;

  if (st->powered) {
    return 0;
  }

  for (uint32_t i = 0U; i < POWER_ON_TRIES; i++) {
    PBL_BOOT_TRACE(PBL_BT_MMC, PBL_BT_MMC_POWER_ON | i);
    ret = mmc->ops->power_on(mmc, &num_blocks);
    if (ret == 0) {
      break;
    }
    PBL_LOG_WRN("Card power-up %" PRIu32 " failed: %d", i + 1U, ret);
  }
  if (ret != 0) {
    PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_MMC_ERR, (uint32_t)ret);
    PBL_BOOT_TRACE(PBL_BT_MMC, PBL_BT_MMC_POWER_ON_FAILED);
    return ret;
  }

  // Another card than the one found first is not what the board describes
  if (st->num_blocks != 0U && st->num_blocks != num_blocks) {
    mmc->ops->power_off(mmc);
    PBL_LOG_ERR("Card of %" PRIu32 " blocks, was %" PRIu32, num_blocks, st->num_blocks);
    return -ENODEV;
  }

  st->num_blocks = num_blocks;
  st->powered = true;
  PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_MMC_BLOCKS, num_blocks);
  PBL_BOOT_TRACE(PBL_BT_MMC, PBL_BT_MMC_ON);
  PBL_LOG_DBG("Card on, %" PRIu32 " blocks", num_blocks);

  return 0;
}

static void prv_idle_fn(struct pbl_work *work) {
  struct pbl_mmc_state *st = PBL_CONTAINER_OF(work, struct pbl_mmc_state, idle.work);
  const struct pbl_mmc *mmc = st->mmc;

  // An access in progress schedules the timer again when it is done
  if (pbl_mutex_lock(&st->lock, PBL_NO_WAIT) != 0) {
    return;
  }
  // As did one since the timer fired
  if (!pbl_delayed_work_is_pending(&st->idle)) {
    prv_power_off(mmc);
  }
  pbl_mutex_unlock(&st->lock);
}

static void prv_touch(const struct pbl_mmc *mmc) {
  if (!mmc->state->coredump && CONFIG_MMC_IDLE_OFF_MS != 0 && mmc->state->powered) {
    (void)pbl_delayed_work_reschedule(&pbl_sys_workq, &mmc->state->idle,
                                      PBL_MSEC(CONFIG_MMC_IDLE_OFF_MS));
  }
}

static bool prv_in_range(const struct pbl_mmc *mmc, uint32_t lba, uint32_t count) {
  const uint32_t num_blocks = mmc->state->num_blocks;

  return count <= num_blocks && lba <= num_blocks - count;
}

// One transfer, retried once after a power cycle; a card that keeps failing stays off
static int prv_transfer(const struct pbl_mmc *mmc, bool write, uint32_t lba, void *buf,
                        uint32_t count) {
  int ret = 0;

  if (count == 0U) {
    return 0;
  }

  prv_lock(mmc);
  for (uint32_t attempt = 0U; attempt < 2U; attempt++) {
    ret = prv_power_on(mmc);
    if (ret != 0) {
      break;
    }

    if (!prv_in_range(mmc, lba, count)) {
      ret = -EINVAL;
      break;
    }

    ret = write ? mmc->ops->write(mmc, lba, buf, count) : mmc->ops->read(mmc, lba, buf, count);
    if (ret == 0) {
      break;
    }

    PBL_LOG_WRN("%s of %" PRIu32 "+%" PRIu32 " failed: %d", write ? "Write" : "Read", lba, count,
                ret);
    PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_MMC_ERR, (uint32_t)ret);
    PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_VALUE, lba);
    PBL_BOOT_TRACE(PBL_BT_MMC,
                   (write ? PBL_BT_MMC_WRITE_FAILED : PBL_BT_MMC_READ_FAILED) | attempt);
    prv_power_off(mmc);
  }
  prv_touch(mmc);
  prv_unlock(mmc);

  return ret;
}

int pbl_mmc_dev_init(const struct pbl_device *dev) {
  const struct pbl_mmc *mmc = PBL_CONTAINER_OF(dev, const struct pbl_mmc, dev);
  struct pbl_mmc_state *st = mmc->state;

  st->mmc = mmc;
  pbl_mutex_init(&st->lock);
  pbl_delayed_work_init(&st->idle, prv_idle_fn);

  if (mmc->ops->init == nullptr) {
    return 0;
  }

  return mmc->ops->init(mmc);
}

int pbl_mmc_get_num_blocks(const struct pbl_mmc *mmc, uint32_t *num_blocks) {
  int ret;

  if (!mmc->state->coredump && !pbl_device_is_ready(&mmc->dev)) {
    return -ENODEV;
  }

  prv_lock(mmc);
  ret = prv_power_on(mmc);
  if (ret == 0) {
    *num_blocks = mmc->state->num_blocks;
  }
  prv_touch(mmc);
  prv_unlock(mmc);

  return ret;
}

int pbl_mmc_read(const struct pbl_mmc *mmc, uint32_t lba, void *buf, uint32_t count) {
  if (!mmc->state->coredump && !pbl_device_is_ready(&mmc->dev)) {
    return -ENODEV;
  }

  return prv_transfer(mmc, false, lba, buf, count);
}

int pbl_mmc_write(const struct pbl_mmc *mmc, uint32_t lba, const void *buf, uint32_t count) {
  if (!mmc->state->coredump && !pbl_device_is_ready(&mmc->dev)) {
    return -ENODEV;
  }

  return prv_transfer(mmc, true, lba, (void *)buf, count);
}

void pbl_mmc_power_off(const struct pbl_mmc *mmc) {
  if (mmc->state->coredump || !pbl_device_is_ready(&mmc->dev)) {
    return;
  }

  pbl_mutex_lock(&mmc->state->lock, PBL_FOREVER);
  (void)pbl_delayed_work_cancel(&mmc->state->idle);
  prv_power_off(mmc);
  pbl_mutex_unlock(&mmc->state->lock);
}

int pbl_mmc_coredump_init(const struct pbl_mmc *mmc) {
  struct pbl_mmc_state *st = mmc->state;
  int ret;

  if (st->coredump) {
    return 0;
  }

  // Whatever the interrupted thread was doing on the bus is lost: start the card again
  st->coredump = true;
  st->powered = false;
  mmc->ops->power_off(mmc);
  if (mmc->ops->init != nullptr) {
    ret = mmc->ops->init(mmc);
    if (ret != 0) {
      return ret;
    }
  }

  return prv_power_on(mmc);
}

// After the flashes on it are stopped: the card is idle, and powered down rather than cut
static void prv_sys_off_prepare(enum pbl_sys_off_mode mode, void *user_data) {
  const struct pbl_device *dev;

  PBL_DEVICE_CLASS_FOREACH(&pbl_mmc_class, dev) {
    pbl_mmc_power_off(PBL_CONTAINER_OF(dev, const struct pbl_mmc, dev));
  }
}

PBL_SYS_OFF_HANDLER_DEFINE(mmc_restart, PBL_SYS_OFF_RESTART_PREPARE, PBL_SYS_OFF_PRIO_LOW,
                           prv_sys_off_prepare, nullptr);
PBL_SYS_OFF_HANDLER_DEFINE(mmc_power_off, PBL_SYS_OFF_POWER_OFF_PREPARE, PBL_SYS_OFF_PRIO_LOW,
                           prv_sys_off_prepare, nullptr);
