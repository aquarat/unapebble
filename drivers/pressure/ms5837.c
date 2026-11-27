/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "ms5837_regs.h"

#include <errno.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/pressure.h>
#include <pbl/drivers/pressure/ms5837.h>
#include <pbl/drivers/regulator.h>
#include <pbl/kernel/delay.h>
#include <pbl/logging/logging.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DEFINE(driver_pressure_ms5837, CONFIG_DRIVER_PRESSURE_LOG_LEVEL);

// The supply off before it comes on again (the stock firmware's BaroMs5837.cpp)
#define SUPPLY_OFF_MS 20U

static const struct pbl_ms5837 *prv_get(const struct pbl_pressure *pressure) {
  return PBL_CONTAINER_OF(pressure, const struct pbl_ms5837, pressure);
}

static int prv_command(const struct pbl_ms5837 *dev, uint8_t cmd) {
  return pbl_i2c_write(&dev->i2c, &cmd, 1U);
}

// Reset, which loads the PROM, then read and check it
static int prv_reset(const struct pbl_ms5837 *dev) {
  uint16_t *prom = dev->state->prom;
  int ret;

  ret = prv_command(dev, MS5837_CMD_RESET);
  if (ret != 0) {
    return ret;
  }
  pbl_sleep_ms(MS5837_RESET_MS);

  for (uint8_t i = 0U; i < MS5837_PROM_WORDS; i++) {
    uint8_t cmd = MS5837_CMD_PROM(i);
    uint8_t word[2];

    ret = pbl_i2c_write_read(&dev->i2c, &cmd, 1U, word, sizeof(word));
    if (ret != 0) {
      return ret;
    }
    prom[i] = (uint16_t)((word[0] << 8) | word[1]);
  }

  if (!ms5837_prom_valid(prom)) {
    PBL_LOG_ERR("PROM not a 02BA's: C0 0x%04x, CRC 0x%x", prom[0], ms5837_crc4(prom));
    return -ENODEV;
  }

  return 0;
}

// The supply off, then on, and the part reset
static int prv_power_up(const struct pbl_ms5837 *dev) {
  int ret;

  if (dev->supply != nullptr) {
    if (pbl_regulator_is_enabled(dev->supply)) {
      (void)pbl_regulator_disable(dev->supply);
    }
    ret = pbl_regulator_power_cycle(dev->supply, SUPPLY_OFF_MS);
    if (ret == 0) {
      ret = pbl_regulator_enable(dev->supply);
    }
    if (ret != 0) {
      return ret;
    }
  }

  return prv_reset(dev);
}

static int prv_convert(const struct pbl_ms5837 *dev, uint8_t cmd, uint32_t *adc) {
  uint8_t read = MS5837_CMD_ADC_READ;
  uint8_t data[3];
  int ret;

  ret = prv_command(dev, cmd);
  if (ret != 0) {
    return ret;
  }

  pbl_sleep_us(ms5837_conversion_us(CONFIG_PRESSURE_MS5837_OSR) + 500U);

  ret = pbl_i2c_write_read(&dev->i2c, &read, 1U, data, sizeof(data));
  if (ret != 0) {
    return ret;
  }

  // A read before the end of the conversion gives 0
  *adc = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];

  return (*adc == 0U) ? -EIO : 0;
}

static int prv_measure(const struct pbl_ms5837 *dev, uint32_t *pa) {
  uint32_t d1, d2;
  int32_t p;
  int ret;

  ret = prv_convert(dev, MS5837_CMD_D1(CONFIG_PRESSURE_MS5837_OSR), &d1);
  if (ret == 0) {
    ret = prv_convert(dev, MS5837_CMD_D2(CONFIG_PRESSURE_MS5837_OSR), &d2);
  }
  if (ret != 0) {
    return ret;
  }

  ms5837_compensate(dev->state->prom, d1, d2, &p, &dev->state->temp_cdeg);
  *pa = (p > 0) ? (uint32_t)p : 0U;

  return 0;
}

static int prv_init(const struct pbl_pressure *pressure) {
  const struct pbl_ms5837 *dev = prv_get(pressure);
  int ret;

  if (dev->supply != nullptr) {
    ret = pbl_device_init(&dev->supply->dev);
    if (ret != 0) {
      return -ENODEV;
    }
  }

  ret = prv_power_up(dev);
  if (ret != 0) {
    PBL_LOG_ERR("Not found (%d)", ret);
    if (dev->supply != nullptr && pbl_regulator_is_enabled(dev->supply)) {
      (void)pbl_regulator_disable(dev->supply);
    }
    return -ENODEV;
  }

  PBL_LOG_INFO("MS5837-02BA at 0x%02x, version 0x%02x", dev->i2c.addr,
               ms5837_version(dev->state->prom));

  return 0;
}

static int prv_read(const struct pbl_pressure *pressure, uint32_t *pa) {
  const struct pbl_ms5837 *dev = prv_get(pressure);
  int ret;

  ret = prv_measure(dev, pa);
  if (ret != 0) {
    PBL_LOG_WRN("Read failed (%d), powering the sensor again", ret);
    ret = prv_power_up(dev);
    if (ret == 0) {
      ret = prv_measure(dev, pa);
    }
  }

  return ret;
}

const struct pbl_pressure_ops pbl_ms5837_ops = {
  .init = prv_init,
  .read = prv_read,
};

#include PBL_DT_INSTANCES(meas_ms5837)
