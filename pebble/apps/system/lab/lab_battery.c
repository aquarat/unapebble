/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The battery's chips read raw over I2C1, without their drivers and without writing to them: the
// MAX17262 fuel gauge and the PCA9420's charger, its charge voltage as the last firmware set it.

#include "lab.h"

#include <errno.h>
#include <inttypes.h>

#include <pbl/drivers/i2c.h>
#include <pbl/util/size.h>

#include <devicetree/devices.h>

#define GAUGE_ADDR 0x36U
#define PMIC_ADDR  0x61U

static const struct pbl_i2c_dev s_gauge = PBL_I2C_DEV(&dt_i2c1, GAUGE_ADDR);
static const struct pbl_i2c_dev s_pmic = PBL_I2C_DEV(&dt_i2c1, PMIC_ADDR);

static int prv_word(uint8_t reg, uint16_t *value) {
  uint8_t b[2];
  int ret = pbl_i2c_burst_read(&s_gauge, reg, b, sizeof(b));

  *value = (uint16_t)(b[0] | (b[1] << 8));

  return ret;
}

static int prv_gauge(void) {
  uint16_t v;
  int ret;

  ret = prv_word(0x09U, &v);
  if (ret != 0) {
    lab_printf("no answer: %d", ret);
    return ret;
  }
  lab_printf("VCell %lu mV", (unsigned long)((uint32_t)v * 78125U / 1000000U));
  if (prv_word(0x19U, &v) == 0) {
    lab_printf("AvgVCell %lu mV", (unsigned long)((uint32_t)v * 78125U / 1000000U));
  }
  if (prv_word(0x0AU, &v) == 0) {
    lab_printf("Current %ld uA", (long)((int16_t)v * 15625L / 100L));
  }
  if (prv_word(0x0BU, &v) == 0) {
    lab_printf("AvgCurrent %ld uA", (long)((int16_t)v * 15625L / 100L));
  }
  if (prv_word(0x06U, &v) == 0) {
    lab_printf("RepSOC %u.%02u %%", v >> 8, (unsigned int)((v & 0xFFU) * 100U / 256U));
  }
  if (prv_word(0x05U, &v) == 0) {
    lab_printf("RepCap %u.%u mAh", v / 2U, (v & 1U) * 5U);
  }
  if (prv_word(0x10U, &v) == 0) {
    lab_printf("FullCapRep %u mAh", v / 2U);
  }
  if (prv_word(0x18U, &v) == 0) {
    lab_printf("DesignCap %u mAh", v / 2U);
  }
  if (prv_word(0x08U, &v) == 0) {
    lab_printf("Temp %d.%u C", (int16_t)v / 256, (unsigned int)((v & 0xFFU) * 10U / 256U));
  }
  if (prv_word(0x00U, &v) == 0) {
    lab_printf("Status 0x%04x", v);
  }
  if (prv_word(0xDBU, &v) == 0) {
    lab_printf("ModelCFG 0x%04x", v);
  }

  return 0;
}

static int prv_pmic(void) {
  static const char *const s_chg[] = {"off", "dead", "low", "fast", "fast", "top-off", "done", "?"};
  uint8_t r[8];
  uint8_t st[4];
  int ret;

  ret = pbl_i2c_burst_read(&s_pmic, 0x10U, r, sizeof(r));
  if (ret == 0) {
    ret = pbl_i2c_burst_read(&s_pmic, 0x18U, st, sizeof(st));
  }
  if (ret != 0) {
    lab_printf("no answer: %d", ret);
    return ret;
  }
  lab_printf("charge voltage %lu mV", 3600UL + (unsigned long)(r[5] & 0x3FU) * 20UL);
  lab_printf("charge current %u mA", (unsigned int)(r[1] & 0x3FU) * 5U);
  lab_printf("termination %u mA", (unsigned int)(r[2] & 0x3FU));
  lab_printf("charger %s", (r[0] & 1U) ? "on" : "off");
  lab_printf("input power %s", (st[0] & (1U << 5)) ? "yes" : "no");
  lab_printf("state %s", s_chg[st[2] & 0x7U]);
  lab_hex("CNTL0-7", r, sizeof(r));
  lab_hex("STATUS0-3", st, sizeof(st));

  return 0;
}

static const struct lab_step s_steps[] = {
  {"fuel gauge (MAX17262)", prv_gauge},
  {"charger (PCA9420)", prv_pmic},
};

const struct lab_experiment lab_exp_battery = {
  .title = "Battery",
  .subtitle = "Gauge and charger, read only",
  .steps = s_steps,
  .num_steps = PBL_ARRAY_SIZE(s_steps),
};
