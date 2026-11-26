/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/drivers/gauge.h>
#include <pbl/logging/logging.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DEFINE(driver_gauge, CONFIG_DRIVER_GAUGE_LOG_LEVEL);

PBL_DEVICE_CLASS_DEFINE(pbl_gauge_class, "gauge");

int pbl_gauge_dev_init(const struct pbl_device *dev) {
  const struct pbl_gauge *gauge = PBL_CONTAINER_OF(dev, const struct pbl_gauge, dev);
  int ret;

  if (gauge->ops->init == nullptr) {
    return 0;
  }

  pbl_mutex_lock(&gauge->state->lock, PBL_FOREVER);
  ret = gauge->ops->init(gauge);
  pbl_mutex_unlock(&gauge->state->lock);

  return ret;
}

int pbl_gauge_read(const struct pbl_gauge *gauge, struct pbl_gauge_reading *reading) {
  struct pbl_gauge_reading r;
  int ret;

  if (!pbl_device_is_ready(&gauge->dev)) {
    return -ENODEV;
  }

  pbl_mutex_lock(&gauge->state->lock, PBL_FOREVER);
  ret = gauge->ops->read(gauge, &r);
  pbl_mutex_unlock(&gauge->state->lock);

  if (ret == 0) {
    *reading = r;
  }

  return ret;
}
