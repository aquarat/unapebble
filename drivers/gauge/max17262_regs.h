/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// MAX17262 registers and their encoding, apart from the bus so the unit tests can check them.
// References: MAX17262 data sheet (19-100229 Rev 1), MAX1726x ModelGauge m5 EZ user guide
// (UG6597 Rev 2), MAX1726x software implementation guide (UG6595).

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/gauge.h>

#define MAX17262_STATUS     0x00U
#define MAX17262_REPSOC     0x06U
#define MAX17262_TEMP       0x08U
#define MAX17262_VCELL      0x09U
#define MAX17262_CURRENT    0x0AU
#define MAX17262_FULLSOCTHR 0x13U
#define MAX17262_DESIGNCAP  0x18U
#define MAX17262_AVGVCELL   0x19U
#define MAX17262_ICHGTERM   0x1EU
#define MAX17262_VEMPTY     0x3AU
#define MAX17262_FSTAT      0x3DU
#define MAX17262_COMMAND    0x60U
#define MAX17262_HIBCFG     0xBAU
#define MAX17262_SOCHOLD    0xD3U
#define MAX17262_MODELCFG   0xDBU

#define MAX17262_STATUS_POR          (1U << 1)
#define MAX17262_STATUS_BST          (1U << 3)
#define MAX17262_FSTAT_DNR           (1U << 0)
#define MAX17262_MODELCFG_REFRESH    (1U << 15)
#define MAX17262_MODELCFG_VCHG       (1U << 10)
#define MAX17262_COMMAND_SOFT_WAKEUP 0x0090U
// SOCHold: 99% hold on, so RepSOC reaches 100% only at end of charge; empty hold off
#define MAX17262_SOCHOLD_VALUE 0x1000U

// The registers read at once for a reading: Status to AvgVCell
#define MAX17262_READ_FIRST MAX17262_STATUS
#define MAX17262_READ_COUNT (MAX17262_AVGVCELL - MAX17262_STATUS + 1U)

// A cell above this charge voltage is a high-voltage one, ModelCFG.VChg = 1
#define MAX17262_VCHG_THRESHOLD_UV 4250000U

// Plausible readings: anything else is a bad transfer or a gauge that lost its senses
#define MAX17262_MIN_MV      2000
#define MAX17262_MAX_MV      4700
#define MAX17262_MIN_TEMP_MC (-40000)
#define MAX17262_MAX_TEMP_MC 85000

// The EZ model registers, in the order they are written; ModelCFG last, as it starts the model
// refresh
enum max17262_model_reg {
  MAX17262_MODEL_DESIGNCAP,
  MAX17262_MODEL_ICHGTERM,
  MAX17262_MODEL_VEMPTY,
  MAX17262_MODEL_FULLSOCTHR,
  MAX17262_MODEL_SOCHOLD,
  MAX17262_MODEL_MODELCFG,
  MAX17262_MODEL_COUNT,
};

static const uint8_t max17262_model_addr[MAX17262_MODEL_COUNT] = {
  MAX17262_DESIGNCAP,  MAX17262_ICHGTERM, MAX17262_VEMPTY,
  MAX17262_FULLSOCTHR, MAX17262_SOCHOLD,  MAX17262_MODELCFG,
};

//! The EZ model of a cell, the units of the battery description. With the internal sense
//! resistor: 0.5 mAh and 156.25 uA a bit; VEmpty in 10 mV (empty) and 40 mV (recovery) steps.
static inline void max17262_model_encode(uint32_t capacity_uah, uint32_t term_ua, uint32_t empty_uv,
                                         uint32_t recovery_uv, uint32_t charge_uv,
                                         uint8_t full_soc_pct,
                                         uint16_t model[MAX17262_MODEL_COUNT]) {
  model[MAX17262_MODEL_DESIGNCAP] = (uint16_t)(capacity_uah / 500U);
  model[MAX17262_MODEL_ICHGTERM] = (uint16_t)((term_ua * 4U) / 625U);
  model[MAX17262_MODEL_VEMPTY] =
      (uint16_t)((((empty_uv / 10000U) << 7) & 0xFF80U) | ((recovery_uv / 40000U) & 0x7FU));
  model[MAX17262_MODEL_FULLSOCTHR] = (uint16_t)(((uint16_t)full_soc_pct << 8) | 0x05U);
  model[MAX17262_MODEL_SOCHOLD] = MAX17262_SOCHOLD_VALUE;
  model[MAX17262_MODEL_MODELCFG] =
      (charge_uv > MAX17262_VCHG_THRESHOLD_UV) ? (uint16_t)MAX17262_MODELCFG_VCHG : 0U;
}

//! Whether the model has to be loaded: after a power-on reset of the gauge, which lost it, or
//! when the registers hold another one. A gauge that kept this model keeps what it learned.
static inline bool max17262_model_needs_load(uint16_t status,
                                             const uint16_t actual[MAX17262_MODEL_COUNT],
                                             const uint16_t model[MAX17262_MODEL_COUNT]) {
  if ((status & MAX17262_STATUS_POR) != 0U) {
    return true;
  }

  for (unsigned int i = 0U; i < MAX17262_MODEL_COUNT; i++) {
    if (actual[i] != model[i]) {
      return true;
    }
  }

  return false;
}

static inline uint16_t max17262_reg(const uint8_t *block, uint8_t reg) {
  return (uint16_t)(block[2U * reg] | ((uint16_t)block[(2U * reg) + 1U] << 8));
}

//! Decode the Status to AvgVCell block. Refuses a reading of a gauge that has reset (-EAGAIN),
//! lost the battery (-ENODEV), or holds another DesignCap or implausible values (-EIO), so a
//! bad transfer never reads as a battery state.
static inline int max17262_decode(const uint8_t *block, uint16_t design_cap,
                                  struct pbl_gauge_reading *r) {
  const uint16_t status = max17262_reg(block, MAX17262_STATUS);
  uint32_t soc;

  if ((status & MAX17262_STATUS_POR) != 0U) {
    return -EAGAIN;
  }
  if ((status & MAX17262_STATUS_BST) != 0U) {
    return -ENODEV;
  }
  if (max17262_reg(block, MAX17262_DESIGNCAP) != design_cap) {
    return -EIO;
  }

  // RepSOC in 1/256 %, rounded down: never more than the gauge says
  soc = ((uint32_t)max17262_reg(block, MAX17262_REPSOC) * 100U) >> 8;
  r->soc_cpct = (uint16_t)((soc > 10000U) ? 10000U : soc);
  // 78.125 uV a bit
  r->voltage_mv = (int32_t)(((uint32_t)max17262_reg(block, MAX17262_VCELL) * 5U) / 64U);
  r->avg_voltage_mv = (int32_t)(((uint32_t)max17262_reg(block, MAX17262_AVGVCELL) * 5U) / 64U);
  // 156.25 uA a bit, positive while charging
  r->current_ua = -(((int32_t)(int16_t)max17262_reg(block, MAX17262_CURRENT) * 625) / 4);
  // 1/256 degree Celsius a bit
  r->temp_mc = ((int32_t)(int16_t)max17262_reg(block, MAX17262_TEMP) * 1000) / 256;

  if ((r->voltage_mv < MAX17262_MIN_MV) || (r->voltage_mv > MAX17262_MAX_MV) ||
      (r->temp_mc < MAX17262_MIN_TEMP_MC) || (r->temp_mc > MAX17262_MAX_TEMP_MC)) {
    return -EIO;
  }

  return 0;
}
