/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "max17262_regs.h"

#include <errno.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/devicetree.h>
#include <pbl/drivers/gauge/max17262.h>
#include <pbl/kernel/delay.h>
#include <pbl/logging/logging.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DECLARE(driver_gauge, CONFIG_DRIVER_GAUGE_LOG_LEVEL);

// The gauge has its first values about 710 ms after power-on; the model refresh takes less
#define DNR_TIMEOUT_MS     1500U
#define REFRESH_TIMEOUT_MS 1000U
#define POLL_MS            10U

static const struct pbl_gauge_max17262 *prv_fg(const struct pbl_gauge *gauge) {
  return PBL_CONTAINER_OF(gauge, const struct pbl_gauge_max17262, gauge);
}

static int prv_read(const struct pbl_gauge_max17262 *fg, uint8_t reg, uint16_t *val) {
  uint8_t buf[2];
  int ret;

  ret = pbl_i2c_burst_read(&fg->i2c, reg, buf, sizeof(buf));
  if (ret == 0) {
    *val = max17262_reg(buf, 0U);
  }

  return ret;
}

static int prv_write(const struct pbl_gauge_max17262 *fg, uint8_t reg, uint16_t val) {
  uint8_t buf[] = {reg, (uint8_t)val, (uint8_t)(val >> 8)};

  return pbl_i2c_write(&fg->i2c, buf, sizeof(buf));
}

static void prv_model(const struct pbl_gauge_max17262 *fg, uint16_t model[MAX17262_MODEL_COUNT]) {
  const struct pbl_battery *bat = fg->battery;

  max17262_model_encode(bat->charge_full_design_uah, bat->charge_term_current_ua, fg->empty_uv,
                        fg->recovery_uv, bat->constant_charge_voltage_uv,
                        CONFIG_GAUGE_MAX17262_FULL_SOC_THRESHOLD, model);
}

// The EZ configuration of UG6595: wait for the first values, leave hibernate, write the model
// and have the gauge refresh it. A refresh keeps what the gauge learned of the cell.
static int prv_load_model(const struct pbl_gauge_max17262 *fg,
                          const uint16_t model[MAX17262_MODEL_COUNT]) {
  uint16_t hibcfg;
  uint16_t val = MAX17262_FSTAT_DNR;
  int ret;

  ret = PBL_POLL_SLEEP_MS(
      (prv_read(fg, MAX17262_FSTAT, &val) == 0) && ((val & MAX17262_FSTAT_DNR) == 0U), POLL_MS,
      DNR_TIMEOUT_MS);
  if (ret != 0) {
    return ret;
  }

  ret = prv_read(fg, MAX17262_HIBCFG, &hibcfg);
  if (ret != 0) {
    return ret;
  }

  ret = prv_write(fg, MAX17262_COMMAND, MAX17262_COMMAND_SOFT_WAKEUP);
  if (ret == 0) {
    ret = prv_write(fg, MAX17262_HIBCFG, 0U);
  }
  if (ret == 0) {
    ret = prv_write(fg, MAX17262_COMMAND, 0U);
  }

  for (unsigned int i = 0U; (ret == 0) && (i < MAX17262_MODEL_COUNT); i++) {
    val = model[i];
    if (i == MAX17262_MODEL_MODELCFG) {
      val |= MAX17262_MODELCFG_REFRESH;
    }
    ret = prv_write(fg, max17262_model_addr[i], val);
  }

  if (ret == 0) {
    val = MAX17262_MODELCFG_REFRESH;
    ret = PBL_POLL_SLEEP_MS(
        (prv_read(fg, MAX17262_MODELCFG, &val) == 0) && ((val & MAX17262_MODELCFG_REFRESH) == 0U),
        POLL_MS, REFRESH_TIMEOUT_MS);
  }

  // Hibernate again as it was, whatever happened
  if (prv_write(fg, MAX17262_HIBCFG, hibcfg) != 0 && ret == 0) {
    ret = -EIO;
  }

  return ret;
}

static int prv_configure(const struct pbl_gauge_max17262 *fg) {
  uint16_t model[MAX17262_MODEL_COUNT];
  uint16_t actual[MAX17262_MODEL_COUNT];
  uint16_t status;
  int ret;

  prv_model(fg, model);

  ret = prv_read(fg, MAX17262_STATUS, &status);
  for (unsigned int i = 0U; (ret == 0) && (i < MAX17262_MODEL_COUNT); i++) {
    ret = prv_read(fg, max17262_model_addr[i], &actual[i]);
  }
  if (ret != 0) {
    return ret;
  }

  if (max17262_model_needs_load(status, actual, model)) {
    PBL_LOG_INFO("Loading the model (status 0x%04x)", status);
    ret = prv_load_model(fg, model);
    if (ret != 0) {
      PBL_LOG_ERR("Model load failed (%d)", ret);
      return ret;
    }
  }

  // Handled: the next power-on reset sets it again
  if ((status & MAX17262_STATUS_POR) != 0U) {
    ret = prv_write(fg, MAX17262_STATUS, status & ~MAX17262_STATUS_POR);
    if (ret != 0) {
      return ret;
    }
    fg->state->resets++;
  }

  fg->state->configured = true;

  return 0;
}

static int prv_init(const struct pbl_gauge *gauge) {
  const struct pbl_gauge_max17262 *fg = prv_fg(gauge);
  int ret;

  if (fg->battery->charge_full_design_uah == 0U || fg->battery->charge_term_current_ua == 0U ||
      fg->battery->constant_charge_voltage_uv == 0U) {
    PBL_LOG_ERR("Battery description incomplete");
    return -EINVAL;
  }

  // A gauge that does not answer yet stays up, to be configured on its next read: readings
  // fail until then
  PBL_BOOT_TRACE(PBL_BT_BATT, PBL_BT_BATT_GAUGE);
  ret = prv_configure(fg);
  if (ret != 0) {
    PBL_LOG_ERR("Configuration failed (%d)", ret);
  }

  // Hibernate held off by a run that reset meanwhile: the gauge's own setting again
  uint16_t hibcfg;
  if (ret == 0 && prv_read(fg, MAX17262_HIBCFG, &hibcfg) == 0 && hibcfg == 0U) {
    (void)prv_write(fg, MAX17262_HIBCFG, MAX17262_HIBCFG_DEFAULT);
  }
  PBL_BOOT_TRACE(PBL_BT_BATT, PBL_BT_BATT_GAUGE_READY);

  return 0;
}

static int prv_read_gauge(const struct pbl_gauge *gauge, struct pbl_gauge_reading *reading) {
  const struct pbl_gauge_max17262 *fg = prv_fg(gauge);
  uint8_t block[2U * MAX17262_READ_COUNT];
  uint16_t model[MAX17262_MODEL_COUNT];
  int ret;

  if (!fg->state->configured) {
    ret = prv_configure(fg);
    if (ret != 0) {
      return ret;
    }
  }

  ret = pbl_i2c_burst_read(&fg->i2c, MAX17262_READ_FIRST, block, sizeof(block));
  if (ret != 0) {
    return ret;
  }

  prv_model(fg, model);
  ret = max17262_decode(block, model[MAX17262_MODEL_DESIGNCAP], reading);
  if (ret == -EAGAIN || ret == -EIO) {
    // Reset since, or another model: configure again on the next read
    fg->state->configured = false;
  }
  if (ret == 0) {
    ret = prv_read(fg, MAX17262_QH, &reading->counter);
    reading->resets = fg->state->resets;
  }

  return ret;
}

// Out of hibernate at once, as the EZ configuration leaves it (UG6595), then back as it was
static int prv_hold_full_rate(const struct pbl_gauge *gauge, bool hold) {
  const struct pbl_gauge_max17262 *fg = prv_fg(gauge);
  int ret;

  if (hold == fg->state->full_rate) {
    return 0;
  }

  if (!hold) {
    ret = prv_write(fg, MAX17262_HIBCFG, fg->state->hibcfg);
    if (ret == 0) {
      fg->state->full_rate = false;
    }
    return ret;
  }

  ret = prv_read(fg, MAX17262_HIBCFG, &fg->state->hibcfg);
  if (ret != 0) {
    return ret;
  }
  ret = prv_write(fg, MAX17262_COMMAND, MAX17262_COMMAND_SOFT_WAKEUP);
  if (ret == 0) {
    ret = prv_write(fg, MAX17262_HIBCFG, 0U);
  }
  if (ret == 0) {
    ret = prv_write(fg, MAX17262_COMMAND, 0U);
  }
  if (ret != 0) {
    (void)prv_write(fg, MAX17262_HIBCFG, fg->state->hibcfg);
    return ret;
  }
  fg->state->full_rate = true;

  return 0;
}

const struct pbl_gauge_ops pbl_gauge_max17262_ops = {
  .init = prv_init,
  .read = prv_read_gauge,
  .hold_full_rate = prv_hold_full_rate,
};

#include PBL_DT_INSTANCES(maxim_max17262)
