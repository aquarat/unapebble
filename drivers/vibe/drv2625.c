/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/vibe.h>
#include <pbl/drivers/vibe/drv2625.h>
#include <pbl/kernel/delay.h>
#include <pbl/logging/logging.h>
#include <pbl/util/bits.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DECLARE(driver_vibe, CONFIG_DRIVER_VIBE_LOG_LEVEL);

/*
 * Registers as the UNA Watch firmware uses them (una-kernel-public
 * Software/Libs/Header/Hardware/DRV2625.hpp:218-350, Source/Hardware/DRV2625.cpp).
 */
#define DRV2625_ID               0x00
#define DRV2625_ID_REV_MASK      PBL_GENMASK(3, 0)
#define DRV2625_ID_CHIPID_MASK   PBL_GENMASK(7, 4)
#define DRV2625_MODE             0x07
#define DRV2625_MODE_MODE_MASK   PBL_GENMASK(1, 0)
#define DRV2625_MODE_RTP         0x0U
#define DRV2625_MODE_TRIG_MASK   PBL_GENMASK(3, 2)
#define DRV2625_CTRL             0x08
#define DRV2625_CTRL_BRK_STBY    PBL_BIT(3)
#define DRV2625_CTRL_OPEN_LOOP   PBL_BIT(6)
#define DRV2625_CTRL_LRA         PBL_BIT(7)
#define DRV2625_GO               0x0C
#define DRV2625_GO_GO            PBL_BIT(0)
#define DRV2625_LIB              0x0D
#define DRV2625_LIB_SEL_ERM      PBL_BIT(6)
#define DRV2625_RTP_INPUT        0x0E
#define DRV2625_RATED_VOLTAGE    0x1F
#define DRV2625_OD_CLAMP         0x20
#define DRV2625_LOOP             0x23
#define DRV2625_LOOP_GAIN_MASK   PBL_GENMASK(3, 2)
#define DRV2625_LOOP_BRAKE_MASK  PBL_GENMASK(6, 4)

// What the UNA firmware checks (Source/Components/Vibro.cpp:57-94; DRV2625.hpp:34-35)
#define DRV2625_CHIPID 1U

// EN high to the first access (Vibro.cpp:135-136)
#define ENABLE_DELAY_US 2000U

static const struct pbl_drv2625 *prv_get(const struct pbl_vibe *vibe) {
  return PBL_CONTAINER_OF(vibe, const struct pbl_drv2625, vibe);
}

static int prv_write(const struct pbl_drv2625 *dev, uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  int ret;

  ret = pbl_i2c_write(&dev->i2c, buf, sizeof(buf));
  if (ret != 0) {
    PBL_LOG_ERR("Could not write register 0x%02x (%d)", reg, ret);
  }

  return ret;
}

static int prv_update(const struct pbl_drv2625 *dev, uint8_t reg, uint8_t mask, uint8_t val) {
  uint8_t cur;
  int ret;

  ret = pbl_i2c_reg_read_byte(&dev->i2c, reg, &cur);
  if (ret != 0) {
    return ret;
  }

  return prv_write(dev, reg, (uint8_t)((cur & ~mask) | (val & mask)));
}

static int prv_init(const struct pbl_vibe *vibe) {
  const struct pbl_drv2625 *dev = prv_get(vibe);
  uint8_t id;
  int ret;

  ret = pbl_gpio_configure(&dev->enable, PBL_GPIO_OUTPUT_ACTIVE);
  if (ret != 0) {
    return ret;
  }

  pbl_busy_wait_us(ENABLE_DELAY_US);

  ret = pbl_i2c_reg_read_byte(&dev->i2c, DRV2625_ID, &id);
  if (ret != 0) {
    PBL_LOG_ERR("Could not read the ID (%d)", ret);
    (void)pbl_gpio_set(&dev->enable, false);
    return -ENODEV;
  }

  if (PBL_FIELD_GET(DRV2625_ID_CHIPID_MASK, id) != DRV2625_CHIPID) {
    PBL_LOG_WRN("Unexpected ID 0x%02x", id);
  }

  // Real-time playback started by the GO bit, out of standby (Vibro.cpp:138-152); an ERM, open
  // loop rather than calibrated, braking into standby; the ERM library and voltages of the UNA
  // firmware (DRV2625.cpp:140-149; Vibro.cpp:184-190)
  ret = prv_write(dev, DRV2625_MODE, DRV2625_MODE_RTP);
  if (ret == 0) {
    ret = prv_write(dev, DRV2625_CTRL, DRV2625_CTRL_OPEN_LOOP | DRV2625_CTRL_BRK_STBY);
  }
  if (ret == 0) {
    ret = prv_update(dev, DRV2625_LOOP, DRV2625_LOOP_BRAKE_MASK | DRV2625_LOOP_GAIN_MASK,
                     PBL_FIELD_PREP(DRV2625_LOOP_BRAKE_MASK, 3U) |
                         PBL_FIELD_PREP(DRV2625_LOOP_GAIN_MASK, 2U));
  }
  if (ret == 0) {
    ret = prv_write(dev, DRV2625_RATED_VOLTAGE, dev->rated_voltage);
  }
  if (ret == 0) {
    ret = prv_write(dev, DRV2625_OD_CLAMP, dev->od_clamp);
  }
  if (ret == 0) {
    ret = prv_update(dev, DRV2625_LIB, DRV2625_LIB_SEL_ERM, DRV2625_LIB_SEL_ERM);
  }
  if (ret == 0) {
    ret = prv_write(dev, DRV2625_RTP_INPUT, 0U);
  }

  return ret;
}

// The input is signed by default: a negative strength drives the motor backwards, which brakes it
static int prv_start(const struct pbl_vibe *vibe, int8_t strength) {
  const struct pbl_drv2625 *dev = prv_get(vibe);
  int ret;

  ret = prv_write(dev, DRV2625_RTP_INPUT, (uint8_t)(int8_t)((strength * 0x7F) / 100));
  if (ret == 0) {
    ret = prv_write(dev, DRV2625_GO, DRV2625_GO_GO);
  }

  return ret;
}

static int prv_stop(const struct pbl_vibe *vibe) {
  return prv_write(prv_get(vibe), DRV2625_GO, 0U);
}

const struct pbl_vibe_ops pbl_drv2625_ops = {
  .init = prv_init,
  .start = prv_start,
  .stop = prv_stop,
};

#include PBL_DT_INSTANCES(ti_drv2625)
