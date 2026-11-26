/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>

#include <pbl/kernel/init.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/time.h>
#include <pbl/soc/st/icache.h>
#include <pbl/util/bits.h>

#include <devicetree/regs.h>
#include <hardware/stm32u5_exti.h>
#include <hardware/stm32u5_icache.h>
#include <hardware/stm32u5_pwr.h>
#include <hardware/stm32u5_rcc.h>

#define RCC(reg)    (PBL_DT_REG_RCC + STM32U5_RCC_##reg##_OFFSET)
#define PWR(reg)    (PBL_DT_REG_PWR + STM32U5_PWR_##reg##_OFFSET)
#define EXTI(reg)   (PBL_DT_REG_EXTI + STM32U5_EXTI_##reg##_OFFSET)
#define ICACHE(reg) (PBL_DT_REG_ICACHE + STM32U5_ICACHE_##reg##_OFFSET)

// SysTick and the pending bits of the system exceptions
#define SYST_CSR           0xe000e010U
#define SCB_ICSR           0xe000ed04U
#define SCB_ICSR_PENDSTCLR PBL_BIT(25)
#define SCB_ICSR_PENDSVCLR PBL_BIT(27)

// Bus masters a bootloader may leave running, writing to RAM: GPDMA1, DMA2D, SDMMC1, the USB OTG
// HS and LPDMA1
#define AHB1RSTR_MASTERS (PBL_BIT(0) | PBL_BIT(18))
#define AHB2RSTR_MASTERS (PBL_BIT(14) | PBL_BIT(27))
#define AHB3RSTR_MASTERS PBL_BIT(9)
#define AHB2RSTR_OFFSET  (STM32U5_RCC_AHB1RSTR_OFFSET + 0x4U)
#define AHB3RSTR_OFFSET  (STM32U5_RCC_AHB1RSTR_OFFSET + 0xcU)

static void prv_pulse_reset(uintptr_t reg, uint32_t mask) {
  pbl_modifyl(0U, mask, reg);
  (void)pbl_readl(reg);
  pbl_modifyl(mask, 0U, reg);
}

// HPRE: 0xxx divides by 1, 1000 to 1111 by 2 to 512, skipping 32
static uint32_t prv_ahb_div(uint32_t pre) {
  if ((pre & RCC_AHB_PRE_DIVIDED) == 0U) {
    return 1U;
  }

  pre &= ~RCC_AHB_PRE_DIVIDED;

  return 1U << (pre + (pre >= 4U ? 2U : 1U));
}

static uint32_t prv_msis_hz(void) {
  static const uint32_t s_ranges[] = STM32U5_MSI_RANGES_HZ;
  uint32_t range;

  if ((pbl_readl(RCC(ICSCR1)) & RCC_ICSCR1_MSIRGSEL) != 0U) {
    range = PBL_FIELD_GET(RCC_ICSCR1_MSISRANGE_MASK, pbl_readl(RCC(ICSCR1)));
  } else {
    range = PBL_FIELD_GET(RCC_CSR_MSISSRANGE_MASK, pbl_readl(RCC(CSR)));
  }

  return s_ranges[range];
}

// The UNA bootloader jumps here with interrupts masked and the clocks and peripherals as it
// used them: nothing is assumed of them
static void prv_quiesce(void) {
  // No interrupt the kernel has not connected may fire once it unmasks them
  pbl_writel(0U, SYST_CSR);
  for (pbl_irq_t irq = 0; irq < CONFIG_NUM_IRQS; irq++) {
    pbl_irq_disable(irq);
    pbl_irq_clear_pending(irq);
  }
  pbl_writel(SCB_ICSR_PENDSTCLR | SCB_ICSR_PENDSVCLR, SCB_ICSR);

  pbl_writel(0U, EXTI(IMR1));
  pbl_writel(0U, EXTI(EMR1));
  pbl_writel(UINT32_MAX, EXTI(RPR1));
  pbl_writel(UINT32_MAX, EXTI(FPR1));

  prv_pulse_reset(RCC(AHB1RSTR), AHB1RSTR_MASTERS);
  prv_pulse_reset(PBL_DT_REG_RCC + AHB2RSTR_OFFSET, AHB2RSTR_MASTERS);
  prv_pulse_reset(PBL_DT_REG_RCC + AHB3RSTR_OFFSET, AHB3RSTR_MASTERS);

  // The state the kernel starts in after a reset
  __asm volatile("cpsie i" ::: "memory");
}

// The code at the image base may be new since the bootloader last cached it
static void prv_icache_on(void) {
  if ((pbl_readl(ICACHE(CR)) & ICACHE_CR_EN) != 0U) {
    pbl_modifyl(0U, ICACHE_CR_CACHEINV, ICACHE(CR));
  }
  while ((pbl_readl(ICACHE(SR)) & ICACHE_SR_BUSYF) != 0U) {
  }
  pbl_modifyl(0U, ICACHE_CR_EN, ICACHE(CR));
}

void soc_stm32u5_icache_invalidate(void) {
  if ((pbl_readl(ICACHE(CR)) & ICACHE_CR_EN) == 0U) {
    return;
  }

  pbl_modifyl(0U, ICACHE_CR_CACHEINV, ICACHE(CR));
  while ((pbl_readl(ICACHE(SR)) & ICACHE_SR_BUSYF) != 0U) {
  }
}

void pbl_soc_early_init(void) {
  // PWR only takes writes with its clock on
  pbl_modifyl(0U, RCC_AHB3ENR_PWREN, RCC(AHB3ENR));
  (void)pbl_readl(RCC(AHB3ENR));

  prv_quiesce();

  // MSIS runs the CPU and the buses until the clock tree takes over: PLL1 can then be set up
  // again, whatever ran it
  pbl_modifyl(0U, RCC_CR_MSISON, RCC(CR));
  while ((pbl_readl(RCC(CR)) & RCC_CR_MSISRDY) == 0U) {
  }
  pbl_modifyl(RCC_CFGR1_SW_MASK, PBL_FIELD_PREP(RCC_CFGR1_SW_MASK, RCC_CFGR1_SW_MSIS), RCC(CFGR1));
  while (PBL_FIELD_GET(RCC_CFGR1_SWS_MASK, pbl_readl(RCC(CFGR1))) != RCC_CFGR1_SW_MSIS) {
  }

#ifdef CONFIG_SOC_STM32U5_EARLY_PLL1_OFF
  // Off, so that the clock tree sets up all its outputs before the system switches to it
  pbl_modifyl(RCC_CR_PLL1ON, 0U, RCC(CR));
  while ((pbl_readl(RCC(CR)) & RCC_CR_PLL1RDY) != 0U) {
  }
#endif

  // Port G is supplied by VDDIO2, isolated until declared valid
  pbl_modifyl(0U, PWR_SVMCR_IO2SV, PWR(SVMCR));

#ifdef CONFIG_SOC_STM32U5_ICACHE
  prv_icache_on();
#endif

  // Busy waits count CPU cycles
  pbl_cycles_per_sec_set(prv_msis_hz() /
                         prv_ahb_div(PBL_FIELD_GET(RCC_CFGR2_HPRE_MASK, pbl_readl(RCC(CFGR2)))));
}
