/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/reboot.h>
#include <pbl/soc/st/pwr.h>
#include <pbl/util/bits.h>

#include <devicetree/regs.h>
#include <hardware/stm32u5_pwr.h>
#include <hardware/stm32u5_rcc.h>
#include <hardware/stm32u5_tamp.h>

#define RCC(reg) (PBL_DT_REG_RCC + STM32U5_RCC_##reg##_OFFSET)
#define PWR(reg) (PBL_DT_REG_PWR + STM32U5_PWR_##reg##_OFFSET)

#define SCB_AIRCR               0xe000ed0cU
#define SCB_AIRCR_VECTKEY       (0x05faU << 16)
#define SCB_AIRCR_PRIGROUP_MASK PBL_GENMASK(10, 8)
#define SCB_AIRCR_SYSRESETREQ   PBL_BIT(2)

// The PMIC, not the SoC, powers the watch on again, from the power button
int pbl_soc_stm32u5_wakeup_pin_set(uintptr_t port, uint32_t pin, bool on) {
  return -ENOTSUP;
}

/*
 * The watch powers off through its bootloader: the flag set in a backup register, it powers the
 * PMIC down at once after the reset, unless the power button is held, and clears the flag. The
 * button has to be released by then, which the board sees to. A PMIC driver may power the watch
 * off itself before this runs.
 */
[[noreturn]] void pbl_soc_power_off(void) {
  (void)pbl_irq_disable_all();

#if CONFIG_SOC_STM32U5_BKP_POWER_OFF >= 0
  pbl_modifyl(0U, RCC_AHB3ENR_PWREN, RCC(AHB3ENR));
  pbl_modifyl(0U, RCC_APB3ENR_RTCAPBEN, RCC(APB3ENR));
  (void)pbl_readl(RCC(APB3ENR));
  pbl_modifyl(0U, PWR_DBPR_DBP, PWR(DBPR));
  pbl_writel(1U, PBL_DT_REG_TAMP + STM32U5_TAMP_BKPR_OFFSET(CONFIG_SOC_STM32U5_BKP_POWER_OFF));
  (void)pbl_readl(PBL_DT_REG_TAMP + STM32U5_TAMP_BKPR_OFFSET(CONFIG_SOC_STM32U5_BKP_POWER_OFF));
#endif

  __asm volatile("dsb" ::: "memory");
  pbl_writel(
      SCB_AIRCR_VECTKEY | (pbl_readl(SCB_AIRCR) & SCB_AIRCR_PRIGROUP_MASK) | SCB_AIRCR_SYSRESETREQ,
      SCB_AIRCR);
  __asm volatile("dsb" ::: "memory");

  while (true) {
  }
}
