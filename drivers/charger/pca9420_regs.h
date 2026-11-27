/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// PCA9420 charger registers and their encoding, apart from the bus so the unit tests can check
// them. Reference: PCA9420 data sheet, Rev. 3.1, section 9.5.

#include <errno.h>
#include <stdint.h>

#include <pbl/drivers/charger.h>

#define PCA9420_SUB_INT0      0x02U
#define PCA9420_SUB_INT0_MASK 0x03U
#define PCA9420_SUB_INT1      0x04U
#define PCA9420_SUB_INT1_MASK 0x05U
#define PCA9420_TOP_CNTL0     0x09U
#define PCA9420_TOP_CNTL1     0x0AU
#define PCA9420_CHG_CNTL0     0x10U
#define PCA9420_CHG_CNTL1     0x11U
#define PCA9420_CHG_CNTL5     0x15U
#define PCA9420_CHG_CNTL7     0x17U
#define PCA9420_CHG_STATUS_0  0x18U
#define PCA9420_CHG_STATUS_2  0x1AU

// SUB_INT0 VIN, SUB_INT1 CHG_OK: the only interrupts unmasked, as the stock firmware does
#define PCA9420_INT_MASK_VIN_ONLY    0xFEU
#define PCA9420_INT_MASK_CHG_OK_ONLY 0xFEU

// VIN_ILIM_SEL, bits 7:5: 001 is 255 mA typical, 222 mA minimum
#define PCA9420_TOP_CNTL0_ILIM_MASK  0xE0U
#define PCA9420_TOP_CNTL0_ILIM_255MA 0x20U

// ASYS_INPUT_SEL, bits 5:4: 00 ASYS from VIN or VBAT, whichever is higher (the default, which a
// PMIC reset or power cycle restores), 01 from VBAT only. ASYS feeds the regulators and the linear
// charger: from VBAT only, the system runs from the battery and nothing charges, VIN connected.
#define PCA9420_TOP_CNTL1_ASYS_MASK     0x30U
#define PCA9420_TOP_CNTL1_ASYS_VIN_VBAT 0x00U
#define PCA9420_TOP_CNTL1_ASYS_VBAT     0x10U

#define PCA9420_CHG_CNTL0_CHG_EN       (1U << 0)
#define PCA9420_CHG_CNTL0_CHG_TIMER_EN (1U << 1)
#define PCA9420_CHG_CNTL0_NTC_EN       (1U << 2)
#define PCA9420_CHG_CNTL0_LOCK_MASK    0xF8U
// CHG_LOCK = 10101b opens CHG_CNTL1 to CHG_CNTL6 to writes, anything else locks them
#define PCA9420_CHG_CNTL0_UNLOCK 0xA8U

// THM_REG, bits 2:0: 000 folds the charge current back from a die at 80 degrees C
#define PCA9420_CHG_CNTL7_THM_REG_MASK 0x07U
#define PCA9420_CHG_CNTL7_THM_REG_80C  0x00U

#define PCA9420_CHG_STATUS_0_IN_PWR_OK (1U << 5)

#define PCA9420_CHG_STATUS_2_CHG_MASK     0x07U
#define PCA9420_CHG_STATUS_2_DETAIL_MASK  0x70U
#define PCA9420_CHG_STATUS_2_DETAIL_SHIFT 4U
// BAT_DETAIL_STATUS 101b: VBAT above VBAT_REG - VBAT_RESTART, near the end of the charge
#define PCA9420_BAT_DETAIL_NEAR_FULL 5U

enum pca9420_chg_state {
  PCA9420_CHG_IDLE = 0,
  PCA9420_CHG_DEAD = 1,
  PCA9420_CHG_LOW = 2,
  PCA9420_CHG_FAST = 3,
  PCA9420_CHG_FAST2 = 4,
  PCA9420_CHG_TOPOFF = 5,
  PCA9420_CHG_DONE = 6,
};

// CHG_CNTL1 to CHG_CNTL6, the registers CHG_LOCK guards
#define PCA9420_CHG_REGS 6U

#define PCA9420_ICHG_STEP_UA   5000U
#define PCA9420_ICHG_MAX_CODE  0x3FU
#define PCA9420_ITERM_STEP_UA  1000U
#define PCA9420_ITERM_MAX_CODE 0x3FU
#define PCA9420_VBAT_MIN_UV    3600000U
#define PCA9420_VBAT_MAX_UV    4600000U
#define PCA9420_VBAT_STEP_UV   20000U

// Pre-charge below VBAT_LOW (2.5 V) and dead-battery charge below VBAT_DEAD (1.9 V): the
// data sheet defaults, written all the same as they are MTP-programmable
#define PCA9420_ILOW_MA  8U
#define PCA9420_IDEAD_MA 4U

// CHG_CNTL5 VBAT_RESTART, bit 6: recharge 240 mV below VBAT_REG rather than 140 mV
#define PCA9420_CHG_CNTL5_RESTART_240MV (1U << 6)

// CHG_CNTL6: timers not doubled, 5 h fast charge timer, 30 min pre-qualification timer, 6.4 min
// top-off timer. Bit 7, the NTC bias, stays clear: the NTC is off, and the stock bootloader reads
// the bit at boot as a request for ship mode (hardware.md, section 9.6), so with it set a reset on
// battery could leave the watch looking dead. Clear is also what the bootloader leaves.
#define PCA9420_CHG_CNTL6_VALUE ((0U << 7) | (0U << 6) | (1U << 4) | (1U << 2) | (1U << 0))

//! The charge registers, CHG_CNTL1 to CHG_CNTL6, for a charge current, a termination current
//! and a charge voltage. Every value rounds down, so the charger never exceeds what is asked.
static inline int pca9420_charge_encode(uint32_t ichg_ua, uint32_t iterm_ua, uint32_t vbat_uv,
                                        uint8_t regs[PCA9420_CHG_REGS]) {
  const uint32_t ichg = ichg_ua / PCA9420_ICHG_STEP_UA;
  const uint32_t iterm = iterm_ua / PCA9420_ITERM_STEP_UA;
  uint32_t ilow = PCA9420_ILOW_MA;
  uint32_t idead = PCA9420_IDEAD_MA;

  if ((ichg == 0U) || (ichg > PCA9420_ICHG_MAX_CODE) || (iterm == 0U) ||
      (iterm > PCA9420_ITERM_MAX_CODE) || (vbat_uv < PCA9420_VBAT_MIN_UV) ||
      (vbat_uv > PCA9420_VBAT_MAX_UV)) {
    return -EINVAL;
  }

  // Neither pre-charge current may exceed the one before it
  if (ilow > (ichg * PCA9420_ICHG_STEP_UA) / 1000U) {
    ilow = (ichg * PCA9420_ICHG_STEP_UA) / 1000U;
  }
  if (idead > ilow) {
    idead = ilow;
  }

  regs[0] = (uint8_t)ichg;
  regs[1] = (uint8_t)iterm;
  regs[2] = (uint8_t)ilow;
  // ICHG_DEAD_TIMER 5 s
  regs[3] = (uint8_t)idead;
  regs[4] = (uint8_t)(PCA9420_CHG_CNTL5_RESTART_240MV |
                      ((vbat_uv - PCA9420_VBAT_MIN_UV) / PCA9420_VBAT_STEP_UV));
  regs[5] = (uint8_t)PCA9420_CHG_CNTL6_VALUE;

  return 0;
}

//! The charge status from CHG_STATUS_0 and CHG_STATUS_2; CHG_STATUS_2 is only valid with input
//! power.
static inline enum pbl_charger_status pca9420_status_decode(uint8_t status0, uint8_t status2) {
  const uint8_t detail =
      (uint8_t)((status2 & PCA9420_CHG_STATUS_2_DETAIL_MASK) >> PCA9420_CHG_STATUS_2_DETAIL_SHIFT);

  if ((status0 & PCA9420_CHG_STATUS_0_IN_PWR_OK) == 0U) {
    return PBL_CHARGER_STATUS_UNKNOWN;
  }

  switch (status2 & PCA9420_CHG_STATUS_2_CHG_MASK) {
    case PCA9420_CHG_DEAD:
    case PCA9420_CHG_LOW:
      return PBL_CHARGER_STATUS_TRICKLE;
    case PCA9420_CHG_FAST:
    case PCA9420_CHG_FAST2:
      // The fast charge state does not tell CC from CV: near VBAT_REG it is CV
      return (detail == PCA9420_BAT_DETAIL_NEAR_FULL) ? PBL_CHARGER_STATUS_CONSTANT_VOLTAGE
                                                      : PBL_CHARGER_STATUS_CONSTANT_CURRENT;
    case PCA9420_CHG_TOPOFF:
      return PBL_CHARGER_STATUS_CONSTANT_VOLTAGE;
    case PCA9420_CHG_DONE:
      return PBL_CHARGER_STATUS_FULL;
    default:
      return PBL_CHARGER_STATUS_UNKNOWN;
  }
}
