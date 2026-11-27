/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>

#include <pbl/kernel/mmio.h>
#include <pbl/system/rtc_registers.h>

#include <devicetree/regs.h>
#include <hardware/stm32u5_pwr.h>
#include <hardware/stm32u5_rcc.h>
#include <hardware/stm32u5_tamp.h>

#define RCC(reg) (PBL_DT_REG_RCC + STM32U5_RCC_##reg##_OFFSET)
#define PWR(reg) (PBL_DT_REG_PWR + STM32U5_PWR_##reg##_OFFSET)

// The backup registers are in TAMP, clocked with the RTC, and only take writes with the backup
// domain unlocked: the clock controller and the RTC driver leave both so, but these may be used
// before either comes up
static void prv_access(void) {
  if ((pbl_readl(RCC(APB3ENR)) & RCC_APB3ENR_RTCAPBEN) == 0U) {
    pbl_modifyl(0U, RCC_AHB3ENR_PWREN, RCC(AHB3ENR));
    pbl_modifyl(0U, RCC_APB3ENR_RTCAPBEN, RCC(APB3ENR));
    (void)pbl_readl(RCC(APB3ENR));
  }
}

void retained_write(uint8_t id, uint32_t value) {
  prv_access();
  if ((pbl_readl(PWR(DBPR)) & PWR_DBPR_DBP) == 0U) {
    pbl_modifyl(0U, RCC_AHB3ENR_PWREN, RCC(AHB3ENR));
    pbl_modifyl(0U, PWR_DBPR_DBP, PWR(DBPR));
    (void)pbl_readl(PWR(DBPR));
  }

  pbl_writel(value, PBL_DT_REG_TAMP + STM32U5_TAMP_BKPR_OFFSET(id));
}

uint32_t retained_read(uint8_t id) {
  prv_access();

  return pbl_readl(PBL_DT_REG_TAMP + STM32U5_TAMP_BKPR_OFFSET(id));
}
