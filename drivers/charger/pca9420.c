/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pca9420_regs.h"

#include <errno.h>
#include <inttypes.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/devicetree.h>
#include <pbl/drivers/charger/pca9420.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/util/misc.h>

PBL_LOG_MODULE_DECLARE(driver_charger, CONFIG_DRIVER_CHARGER_LOG_LEVEL);

#define CHARGER_DEBOUNCE_MS 400
// Rounds of reading and clearing the interrupt flags, for events during the last round
#define IRQ_CLEAR_ROUNDS 3U

static const struct pbl_charger_pca9420 *prv_chg(const struct pbl_charger *charger) {
  return PBL_CONTAINER_OF(charger, const struct pbl_charger_pca9420, charger);
}

static int prv_read(const struct pbl_charger_pca9420 *chg, uint8_t reg, uint8_t *val) {
  return pbl_i2c_reg_read_byte(&chg->i2c, reg, val);
}

static int prv_write(const struct pbl_charger_pca9420 *chg, uint8_t reg, uint8_t val) {
  return pbl_i2c_reg_write_byte(&chg->i2c, reg, val);
}

// Charging off and the charge registers locked: what a failure leaves
static void prv_disable_locked(const struct pbl_charger_pca9420 *chg) {
  (void)prv_write(chg, PCA9420_CHG_CNTL0, PCA9420_CHG_CNTL0_CHG_TIMER_EN);
}

static int prv_charge_regs(const struct pbl_charger_pca9420 *chg, uint8_t regs[PCA9420_CHG_REGS]) {
  const struct pbl_battery *bat = chg->battery;
  uint32_t vbat_uv = bat->constant_charge_voltage_uv;

#if CONFIG_CHARGER_PCA9420_CHARGE_LIMIT_MV != 0
  if (vbat_uv > CONFIG_CHARGER_PCA9420_CHARGE_LIMIT_MV * 1000U) {
    vbat_uv = CONFIG_CHARGER_PCA9420_CHARGE_LIMIT_MV * 1000U;
  }
#endif

  return pca9420_charge_encode(bat->constant_charge_current_ua, bat->charge_term_current_ua,
                               vbat_uv, regs);
}

// Charging stays off: the charge registers are written with it off, read back, and locked
static int prv_configure(const struct pbl_charger_pca9420 *chg) {
  uint8_t regs[PCA9420_CHG_REGS];
  uint8_t val;
  int ret;

  chg->data->configured = false;

  ret = prv_charge_regs(chg, regs);
  if (ret != 0) {
    return ret;
  }

  // NTC off: the TS pin has no thermistor on it, the battery temperature is checked from the
  // fuel gauge instead
  ret =
      prv_write(chg, PCA9420_CHG_CNTL0, PCA9420_CHG_CNTL0_UNLOCK | PCA9420_CHG_CNTL0_CHG_TIMER_EN);
  for (uint8_t i = 0U; (ret == 0) && (i < PCA9420_CHG_REGS); i++) {
    ret = prv_write(chg, PCA9420_CHG_CNTL1 + i, regs[i]);
  }
  for (uint8_t i = 0U; (ret == 0) && (i < PCA9420_CHG_REGS); i++) {
    ret = prv_read(chg, PCA9420_CHG_CNTL1 + i, &val);
    if ((ret == 0) && (val != regs[i])) {
      PBL_LOG_ERR("CHG_CNTL%u reads 0x%02x, not 0x%02x", i + 1U, val, regs[i]);
      ret = -EIO;
    }
  }
  prv_disable_locked(chg);
  if (ret != 0) {
    return ret;
  }

  ret = pbl_i2c_reg_update_byte(&chg->i2c, PCA9420_CHG_CNTL7, PCA9420_CHG_CNTL7_THM_REG_MASK,
                                PCA9420_CHG_CNTL7_THM_REG_80C);
  if (ret == 0) {
    ret = pbl_i2c_reg_update_byte(&chg->i2c, PCA9420_TOP_CNTL0, PCA9420_TOP_CNTL0_ILIM_MASK,
                                  PCA9420_TOP_CNTL0_ILIM_255MA);
  }
  if (ret == 0) {
    ret = prv_write(chg, PCA9420_SUB_INT0_MASK, PCA9420_INT_MASK_VIN_ONLY);
  }
  if (ret == 0) {
    ret = prv_write(chg, PCA9420_SUB_INT1_MASK, PCA9420_INT_MASK_CHG_OK_ONLY);
  }
  if (ret != 0) {
    return ret;
  }

  chg->data->configured = true;

  return 0;
}

static int prv_apply_enable(const struct pbl_charger_pca9420 *chg) {
  int ret;

  if (!chg->data->configured) {
    // Never charge on a configuration that did not take
    ret = prv_configure(chg);
    if (ret != 0) {
      return ret;
    }
  }

  // Writing CHG_CNTL0 with CHG_LOCK cleared keeps the charge registers locked
  return pbl_i2c_reg_update_byte(
      &chg->i2c, PCA9420_CHG_CNTL0, PCA9420_CHG_CNTL0_LOCK_MASK | PCA9420_CHG_CNTL0_CHG_EN,
      (chg->data->enabled && !chg->data->input_suspended) ? PCA9420_CHG_CNTL0_CHG_EN : 0U);
}

// ASYS from VBAT only, or from VIN or VBAT as the PMIC starts, read back
static int prv_asys_set(const struct pbl_charger_pca9420 *chg, bool vbat_only) {
  const uint8_t sel = vbat_only ? PCA9420_TOP_CNTL1_ASYS_VBAT : PCA9420_TOP_CNTL1_ASYS_VIN_VBAT;
  uint8_t val;
  int ret;

  ret = pbl_i2c_reg_update_byte(&chg->i2c, PCA9420_TOP_CNTL1, PCA9420_TOP_CNTL1_ASYS_MASK, sel);
  if (ret == 0) {
    ret = prv_read(chg, PCA9420_TOP_CNTL1, &val);
  }
  if ((ret == 0) && ((val & PCA9420_TOP_CNTL1_ASYS_MASK) != sel)) {
    ret = -EIO;
  }

  return ret;
}

// The PMIC loses its registers in a reset of its own, and takes its defaults back: check the
// current and the voltage are still ours, and configure it again otherwise
static int prv_check(const struct pbl_charger_pca9420 *chg) {
  uint8_t regs[PCA9420_CHG_REGS];
  uint8_t ichg;
  uint8_t vbat;
  int ret;

  if (chg->data->configured) {
    ret = prv_charge_regs(chg, regs);
    if (ret == 0) {
      ret = prv_read(chg, PCA9420_CHG_CNTL1, &ichg);
    }
    if (ret == 0) {
      ret = prv_read(chg, PCA9420_CHG_CNTL5, &vbat);
    }
    if (ret != 0) {
      return ret;
    }
    if ((ichg == regs[0]) && (vbat == regs[4])) {
      return 0;
    }

    PBL_LOG_WRN("Charge configuration lost, configuring again");
    prv_disable_locked(chg);
    chg->data->configured = false;
  }

  return prv_apply_enable(chg);
}

// Write-1-to-clear: clear what was read, then look again for what came meanwhile, so INTB goes
// back high and the next event gives an edge
static void prv_clear_irqs(const struct pbl_charger_pca9420 *chg) {
  uint8_t int0;
  uint8_t int1;

  for (unsigned int i = 0U; i < IRQ_CLEAR_ROUNDS; i++) {
    if ((prv_read(chg, PCA9420_SUB_INT0, &int0) != 0) ||
        (prv_read(chg, PCA9420_SUB_INT1, &int1) != 0) || ((int0 | int1) == 0U)) {
      return;
    }
    (void)prv_write(chg, PCA9420_SUB_INT0, int0);
    (void)prv_write(chg, PCA9420_SUB_INT1, int1);
  }
}

static void prv_handle_irq(struct pbl_work *work) {
  struct pbl_charger_pca9420_data *data =
      PBL_CONTAINER_OF(work, struct pbl_charger_pca9420_data, debounce_timer.work);

  prv_clear_irqs(data->chg);
  pbl_charger_report_online_changed(&data->chg->charger);
}

static void prv_irq_cb(void *arg) {
  const struct pbl_charger_pca9420 *chg = arg;

  pbl_delayed_work_reschedule(&pbl_sys_workq_high, &chg->data->debounce_timer,
                              PBL_MSEC(CHARGER_DEBOUNCE_MS));
}

static void prv_irq_handler(void *arg) {
  pbl_sys_work_call_from_isr(prv_irq_cb, arg);
}

static int prv_irq_setup(const struct pbl_charger_pca9420 *chg) {
  int ret;

  ret = pbl_pinctrl_select_state(&chg->pinctrl, PBL_PINCTRL_STATE_DEFAULT);
  if (ret != 0 && ret != -ENOENT) {
    return ret;
  }

  ret = pbl_device_init(&chg->irq.intc->dev);
  if (ret == 0) {
    ret = pbl_intc_request(&chg->irq, prv_irq_handler, (void *)chg);
  }
  if (ret == 0) {
    ret = pbl_intc_enable(&chg->irq);
  }

  return ret;
}

static int prv_init(const struct pbl_charger *charger) {
  const struct pbl_charger_pca9420 *chg = prv_chg(charger);
  const struct pbl_battery *bat = chg->battery;
  uint8_t regs[PCA9420_CHG_REGS];
  int ret;

  chg->data->chg = chg;
  chg->data->enabled = false;
  chg->data->input_suspended = false;
  pbl_delayed_work_init(&chg->data->debounce_timer, prv_handle_irq);

  // No more than 1C, whatever the description says
  ret = prv_charge_regs(chg, regs);
  if ((ret != 0) || (bat->constant_charge_current_ua > bat->charge_full_design_uah)) {
    PBL_LOG_ERR("Invalid charge parameters");
    return -EINVAL;
  }

  // A PMIC that does not answer leaves the device up with charging off, to be configured again
  // on its next use: the battery service keeps polling it
  PBL_BOOT_TRACE(PBL_BT_BATT, PBL_BT_BATT_CHARGER);
  ret = prv_configure(chg);
  if (ret != 0) {
    PBL_LOG_ERR("Configuration failed (%d)", ret);
  }

  // A run that suspended the input and reset meanwhile: ASYS takes VIN again
  ret = prv_asys_set(chg, false);
  if (ret != 0) {
    PBL_LOG_ERR("Input restore failed (%d)", ret);
  }

  // Without the interrupt, plugging in shows on the next poll
  ret = prv_irq_setup(chg);
  if (ret != 0) {
    PBL_LOG_ERR("Interrupt setup failed (%d)", ret);
  }
  // Events from before boot would hold INTB low
  prv_clear_irqs(chg);
  PBL_BOOT_TRACE(PBL_BT_BATT, PBL_BT_BATT_CHARGER_READY);

  return 0;
}

static int prv_is_online(const struct pbl_charger *charger, bool *online) {
  uint8_t status0;
  int ret;

  ret = prv_read(prv_chg(charger), PCA9420_CHG_STATUS_0, &status0);
  if (ret == 0) {
    *online = (status0 & PCA9420_CHG_STATUS_0_IN_PWR_OK) != 0U;
  }

  return ret;
}

static int prv_get_status(const struct pbl_charger *charger, enum pbl_charger_status *status) {
  const struct pbl_charger_pca9420 *chg = prv_chg(charger);
  uint8_t status0;
  uint8_t status2;
  int ret;

  ret = prv_check(chg);
  if (ret == 0) {
    ret = prv_read(chg, PCA9420_CHG_STATUS_0, &status0);
  }
  if (ret == 0) {
    ret = prv_read(chg, PCA9420_CHG_STATUS_2, &status2);
  }
  if (ret == 0) {
    *status = pca9420_status_decode(status0, status2);
  }

  return ret;
}

static int prv_get_measurements(const struct pbl_charger *charger,
                                struct pbl_charger_measurements *meas) {
  return -ENOTSUP;
}

static int prv_enable(const struct pbl_charger *charger, bool enable) {
  const struct pbl_charger_pca9420 *chg = prv_chg(charger);

  chg->data->enabled = enable;

  return prv_apply_enable(chg);
}

// Charging stops before the input goes, and starts again, if allowed, once it is back
static int prv_set_input_suspend(const struct pbl_charger *charger, bool suspend) {
  const struct pbl_charger_pca9420 *chg = prv_chg(charger);
  int ret;

  if (suspend) {
    chg->data->input_suspended = true;
    ret = prv_apply_enable(chg);
    if (ret == 0) {
      ret = prv_asys_set(chg, true);
    }
    if (ret != 0) {
      (void)prv_asys_set(chg, false);
      chg->data->input_suspended = false;
      (void)prv_apply_enable(chg);
    }
    return ret;
  }

  ret = prv_asys_set(chg, false);
  if (ret != 0) {
    return ret;
  }
  chg->data->input_suspended = false;

  return prv_apply_enable(chg);
}

const struct pbl_charger_ops pbl_charger_pca9420_ops = {
  .init = prv_init,
  .is_online = prv_is_online,
  .get_status = prv_get_status,
  .get_measurements = prv_get_measurements,
  .enable = prv_enable,
  .set_input_suspend = prv_set_input_suspend,
};

#include PBL_DT_INSTANCES(nxp_pca9420)
