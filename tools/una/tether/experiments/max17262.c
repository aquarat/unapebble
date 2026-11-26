/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Reads the MAX17262 fuel gauge (I2C1 0x36) and the charger status of the
 * PCA9420 PMIC (I2C1 0x61). Read only: the only writes are register addresses.
 * Units from the MAX17262 data sheet, internal sense resistor: 0.5 mAh,
 * 156.25 uA, 78.125 uV, 1/256 % and degree C a bit.
 */

#include "tether_blob.h"

#define GAUGE 0x36U
#define PMIC  0x61U

static int s_errors;

static uint16_t prv_reg(uint8_t reg) {
  uint8_t v[2] = {0, 0};
  if (tether->i2c(1, GAUGE, &reg, 1U, v, 2U, 0) != 0) {
    s_errors++;
  }
  return (uint16_t)(v[0] | (v[1] << 8));
}

static uint8_t prv_pmic(uint8_t reg) {
  uint8_t v = 0;
  if (tether->i2c(1, PMIC, &reg, 1U, &v, 1U, 0) != 0) {
    s_errors++;
  }
  return v;
}

/* Hundredths, signed */
static void prv_fixed(const char *name, int32_t v100, const char *unit) {
  const char *sign = v100 < 0 ? "-" : "";
  uint32_t a = (uint32_t)(v100 < 0 ? -v100 : v100);
  tprintf("  %-12s %s%lu.%02lu %s\n", name, sign, (unsigned long)(a / 100U),
          (unsigned long)(a % 100U), unit);
}

uint32_t blob_main(const uint32_t *args, uint32_t nargs) {
  static const struct {
    uint8_t reg;
    const char *name;
  } s_raw[] = {
    {0x00, "Status"},     {0x01, "VAlrtTh"},    {0x05, "RepCap"},   {0x06, "RepSOC"},
    {0x07, "Age"},        {0x08, "Temp"},       {0x09, "VCell"},    {0x0a, "Current"},
    {0x0b, "AvgCurrent"}, {0x10, "FullCapRep"}, {0x11, "TTE"},      {0x17, "Cycles"},
    {0x18, "DesignCap"},  {0x19, "AvgVCell"},   {0x1e, "IChgTerm"}, {0x20, "TTF"},
    {0x21, "DevName"},    {0x23, "FullCapNom"}, {0x3a, "VEmpty"},   {0x3d, "FStat"},
    {0x4d, "QH"},         {0xba, "HibCfg"},     {0xdb, "ModelCFG"},
  };

  tprintf("MAX17262 at I2C1 0x%02x:\n", GAUGE);
  for (unsigned i = 0U; i < sizeof(s_raw) / sizeof(s_raw[0]); i++) {
    tprintf("  %02x %-11s %04x\n", s_raw[i].reg, s_raw[i].name, prv_reg(s_raw[i].reg));
  }

  tprintf("decoded:\n");
  prv_fixed("VCell", (int32_t)(((uint32_t)prv_reg(0x09) * 78125U) / 10000000U), "V");
  prv_fixed("AvgVCell", (int32_t)(((uint32_t)prv_reg(0x19) * 78125U) / 10000000U), "V");
  prv_fixed("Current", (int32_t)((int16_t)prv_reg(0x0a) * 15625) / 1000, "mA");
  prv_fixed("AvgCurrent", (int32_t)((int16_t)prv_reg(0x0b) * 15625) / 1000, "mA");
  prv_fixed("RepSOC", (int32_t)((uint32_t)prv_reg(0x06) * 100U / 256U), "%");
  prv_fixed("RepCap", (int32_t)prv_reg(0x05) * 50, "mAh");
  prv_fixed("FullCapRep", (int32_t)prv_reg(0x10) * 50, "mAh");
  prv_fixed("FullCapNom", (int32_t)prv_reg(0x23) * 50, "mAh");
  prv_fixed("DesignCap", (int32_t)prv_reg(0x18) * 50, "mAh");
  prv_fixed("Temp", (int32_t)(int16_t)prv_reg(0x08) * 100 / 256, "C");
  prv_fixed("Cycles", (int32_t)prv_reg(0x17), "");
  tprintf("  %-12s %lu min\n", "TTE", (unsigned long)((uint32_t)prv_reg(0x11) * 5625U / 60000U));

  /* PCA9420: device info, the charger's settings and status */
  static const struct {
    uint8_t reg;
    const char *name;
  } s_pmic[] = {
    {0x00, "DEV_INFO"},     {0x01, "TOP_INT"},      {0x0c, "TOP_CNTL3"},    {0x10, "CHG_CNTL0"},
    {0x11, "ICHG_CC"},      {0x12, "ICHG_TOPOFF"},  {0x13, "ICHG_LOW"},     {0x14, "ICHG_DEAD"},
    {0x15, "VBAT_REG"},     {0x16, "CHG_CNTL6"},    {0x17, "CHG_CNTL7"},    {0x18, "CHG_STATUS_0"},
    {0x19, "CHG_STATUS_1"}, {0x1a, "CHG_STATUS_2"}, {0x1b, "CHG_STATUS_3"},
  };
  static const char *const s_state[8] = {"idle",    "dead", "low", "fast",
                                         "top-off", "done", "?",   "?"};
  tprintf("PCA9420 at I2C1 0x%02x:\n", PMIC);
  for (unsigned i = 0U; i < sizeof(s_pmic) / sizeof(s_pmic[0]); i++) {
    tprintf("  %02x %-13s %02x\n", s_pmic[i].reg, s_pmic[i].name, prv_pmic(s_pmic[i].reg));
  }
  const uint8_t st0 = prv_pmic(0x18);
  const uint8_t st2 = prv_pmic(0x1a);
  const uint8_t vreg = prv_pmic(0x15);
  tprintf("  input power %s, charge %s, VBAT_REG %lu mV, ICHG_CC %u mA\n",
          (st0 & 0x20U) ? "ok" : "none", s_state[st2 & 7U],
          (unsigned long)(3600U + 20U * (vreg & 0x3fU)), 5U * (prv_pmic(0x11) & 0x3fU));

  tprintf("%d I2C errors\n", s_errors);
  return (uint32_t)s_errors;
}
