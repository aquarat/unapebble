/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>

#include <pbl/drivers/gnss.h>
#include <pbl/logging/logging.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DEFINE(driver_gnss, CONFIG_DRIVER_GNSS_LOG_LEVEL);
PBL_DEVICE_CLASS_DEFINE(pbl_gnss_class, "gnss");

int pbl_gnss_dev_init(const struct pbl_device *dev) {
  const struct pbl_gnss *gnss = PBL_CONTAINER_OF(dev, const struct pbl_gnss, dev);
  int ret = 0;

  if (gnss->ops->init == nullptr) {
    return 0;
  }

  pbl_gnss_lock(gnss);
  ret = gnss->ops->init(gnss);
  pbl_gnss_unlock(gnss);

  return ret;
}

void pbl_gnss_lock(const struct pbl_gnss *gnss) {
  pbl_mutex_lock(&gnss->state->lock, PBL_FOREVER);
}

void pbl_gnss_unlock(const struct pbl_gnss *gnss) {
  pbl_mutex_unlock(&gnss->state->lock);
}

int pbl_gnss_set_listener(const struct pbl_gnss *gnss, const struct pbl_gnss_listener *listener) {
  pbl_gnss_lock(gnss);
  gnss->state->listener = listener;
  pbl_gnss_unlock(gnss);

  return 0;
}

int pbl_gnss_start(const struct pbl_gnss *gnss, uint32_t interval_ms) {
  int ret;

  if (!pbl_device_is_ready(&gnss->dev)) {
    return -ENODEV;
  }

  if (interval_ms == 0U) {
    return -EINVAL;
  }

  pbl_gnss_lock(gnss);
  if (gnss->state->interval_ms == 0U) {
    gnss->state->has_last = false;
  }
  ret = gnss->ops->start(gnss, interval_ms);
  if (ret == 0) {
    gnss->state->interval_ms = interval_ms;
    PBL_LOG_DBG("started, %" PRIu32 " ms", interval_ms);
  }
  pbl_gnss_unlock(gnss);

  return ret;
}

int pbl_gnss_stop(const struct pbl_gnss *gnss) {
  int ret = 0;

  if (!pbl_device_is_ready(&gnss->dev)) {
    return -ENODEV;
  }

  pbl_gnss_lock(gnss);
  if (gnss->state->interval_ms != 0U) {
    ret = gnss->ops->stop(gnss);
    if (ret == 0) {
      gnss->state->interval_ms = 0U;
      PBL_LOG_DBG("stopped");
    }
  }
  pbl_gnss_unlock(gnss);

  return ret;
}

bool pbl_gnss_is_running(const struct pbl_gnss *gnss) {
  if (!pbl_device_is_ready(&gnss->dev)) {
    return false;
  }

  return gnss->state->interval_ms != 0U;
}

int pbl_gnss_peek(const struct pbl_gnss *gnss, struct pbl_gnss_fix *fix) {
  int ret = 0;

  if (!pbl_device_is_ready(&gnss->dev)) {
    return -ENODEV;
  }

  pbl_gnss_lock(gnss);
  if (gnss->state->has_last) {
    *fix = gnss->state->last;
  } else {
    ret = -ENODATA;
  }
  pbl_gnss_unlock(gnss);

  return ret;
}

void pbl_gnss_report_fix(const struct pbl_gnss *gnss, const struct pbl_gnss_fix *fix) {
  const struct pbl_gnss_listener *listener = gnss->state->listener;

  pbl_mutex_assert_held(&gnss->state->lock, true);

  gnss->state->last = *fix;
  gnss->state->has_last = true;

  if (listener == nullptr || listener->fix == nullptr) {
    return;
  }

  listener->fix(gnss, fix, listener->user_data);
}
